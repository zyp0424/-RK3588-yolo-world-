// Copyright (c) 2024 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "clip_text.h"
#include "file_utils.h"
#include "image_drawing.h"
#include "image_utils.h"
#include "yolo_world.h"

static std::atomic<bool> g_running(true);

static void signal_handler(int)
{
    g_running.store(false);
}

static int xioctl(int fd, unsigned long request, void* arg)
{
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

static uint64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

template <typename T>
class DroppingQueue {
public:
    explicit DroppingQueue(size_t capacity) : capacity_(capacity) {}

    bool push(T&& item)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopped_) {
            return false;
        }
        if (queue_.size() >= capacity_) {
            queue_.pop_front();
            drops_++;
        }
        queue_.push_back(std::move(item));
        cv_.notify_one();
        return true;
    }

    bool pop(T& item)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
        if (queue_.empty()) {
            return false;
        }
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        cv_.notify_all();
    }

    uint64_t drops() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return drops_;
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    size_t capacity() const
    {
        return capacity_;
    }

private:
    size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> queue_;
    bool stopped_ = false;
    uint64_t drops_ = 0;
};

struct Config {
    std::string text_model_path;
    std::string text_path;
    std::string yolo_model_path;
    std::string device = "/dev/video11";
    std::string output_path = "/tmp/yolo_world_realtime.nv12";
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int stream_fps = 60;
    int v4l2_buffers = 8;
    int skip_frames = 5;
    int preprocess_workers = 6;
    int inference_workers = 3;
    int raw_queue_depth = 2;
    int infer_queue_depth = 1;
    int result_queue_depth = 1;
};

struct RawFrame {
    uint64_t seq = 0;
    uint64_t timestamp_ms = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> nv12;
};

struct PreprocessedFrame {
    RawFrame frame;
    std::vector<uint8_t> rgb;
    letterbox_t letterbox;
};

struct DetectedFrame {
    RawFrame frame;
    object_detect_result_list results;
    int worker_id = -1;
};

struct Stats {
    std::atomic<uint64_t> captured{0};
    std::atomic<uint64_t> preprocessed{0};
    std::atomic<uint64_t> inferred{0};
    std::atomic<uint64_t> streamed{0};
    std::atomic<uint64_t> stale_results{0};
    std::atomic<uint64_t> last_stream_age_ms{0};
    std::atomic<uint64_t> capture_errors{0};
    std::atomic<uint64_t> preprocess_errors{0};
    std::atomic<uint64_t> inference_errors{0};
    std::atomic<uint64_t> output_errors{0};
};

static void print_usage(const char* prog)
{
    fprintf(stderr,
            "Usage: %s <clip_text.rknn> <detect_classes.txt> <yolo_world.rknn> [options]\n"
            "Options:\n"
            "  --device PATH              V4L2 device, default /dev/video11\n"
            "  --width N                  capture width, default 1920\n"
            "  --height N                 capture height, default 1080\n"
            "  --fps N                    capture fps, default 60\n"
            "  --stream-fps N             output/stream fps, default 60\n"
            "  --buffers N                V4L2 mmap buffer count, default 8\n"
            "  --skip N                   initial captured frames to skip, default 5\n"
            "  --preprocess-workers N     RGA preprocess workers, default 6\n"
            "  --workers N                RKNN inference workers, default 3\n"
            "  --raw-queue N              raw frame queue depth, default 2\n"
            "  --infer-queue N            preprocessed frame queue depth, default 1\n"
            "  --result-queue N           result queue depth, default 1\n"
            "  --output PATH              raw NV12 output path or '-', default /tmp/yolo_world_realtime.nv12\n",
            prog);
}

static bool parse_args(int argc, char** argv, Config* cfg)
{
    if (argc < 4) {
        print_usage(argv[0]);
        return false;
    }

    cfg->text_model_path = argv[1];
    cfg->text_path = argv[2];
    cfg->yolo_model_path = argv[3];

    for (int i = 4; i < argc; i++) {
        std::string arg = argv[i];
        auto require_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "Option %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--device") {
            const char* v = require_value("--device");
            if (!v) return false;
            cfg->device = v;
        } else if (arg == "--width") {
            const char* v = require_value("--width");
            if (!v) return false;
            cfg->width = atoi(v);
        } else if (arg == "--height") {
            const char* v = require_value("--height");
            if (!v) return false;
            cfg->height = atoi(v);
        } else if (arg == "--fps") {
            const char* v = require_value("--fps");
            if (!v) return false;
            cfg->fps = atoi(v);
        } else if (arg == "--stream-fps") {
            const char* v = require_value("--stream-fps");
            if (!v) return false;
            cfg->stream_fps = atoi(v);
        } else if (arg == "--buffers") {
            const char* v = require_value("--buffers");
            if (!v) return false;
            cfg->v4l2_buffers = atoi(v);
        } else if (arg == "--skip") {
            const char* v = require_value("--skip");
            if (!v) return false;
            cfg->skip_frames = atoi(v);
        } else if (arg == "--preprocess-workers") {
            const char* v = require_value("--preprocess-workers");
            if (!v) return false;
            cfg->preprocess_workers = atoi(v);
        } else if (arg == "--workers") {
            const char* v = require_value("--workers");
            if (!v) return false;
            cfg->inference_workers = atoi(v);
        } else if (arg == "--raw-queue") {
            const char* v = require_value("--raw-queue");
            if (!v) return false;
            cfg->raw_queue_depth = atoi(v);
        } else if (arg == "--infer-queue") {
            const char* v = require_value("--infer-queue");
            if (!v) return false;
            cfg->infer_queue_depth = atoi(v);
        } else if (arg == "--result-queue") {
            const char* v = require_value("--result-queue");
            if (!v) return false;
            cfg->result_queue_depth = atoi(v);
        } else if (arg == "--output") {
            const char* v = require_value("--output");
            if (!v) return false;
            cfg->output_path = v;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return false;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            print_usage(argv[0]);
            return false;
        }
    }

    cfg->v4l2_buffers = std::max(2, cfg->v4l2_buffers);
    cfg->fps = std::max(1, cfg->fps);
    cfg->stream_fps = std::max(1, cfg->stream_fps);
    cfg->preprocess_workers = std::max(1, cfg->preprocess_workers);
    cfg->inference_workers = std::max(1, cfg->inference_workers);
    cfg->raw_queue_depth = std::max(1, cfg->raw_queue_depth);
    cfg->infer_queue_depth = std::max(1, cfg->infer_queue_depth);
    cfg->result_queue_depth = std::max(1, cfg->result_queue_depth);
    return true;
}

static int open_output_fd(const std::string& path)
{
    if (path == "-") {
        int fd = dup(STDOUT_FILENO);
        if (fd < 0) {
            fprintf(stderr, "dup stdout failed: %s\n", strerror(errno));
        }
        return fd;
    }

    struct stat st;
    int flags = O_WRONLY;
    mode_t mode = 0644;
    if (stat(path.c_str(), &st) != 0 || !S_ISFIFO(st.st_mode)) {
        flags |= O_CREAT | O_TRUNC;
    }

    int fd = open(path.c_str(), flags, mode);
    if (fd < 0) {
        fprintf(stderr, "open output %s failed: %s\n", path.c_str(), strerror(errno));
    }
    return fd;
}

static void silence_stdout()
{
    fflush(stdout);
    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
        dup2(null_fd, STDOUT_FILENO);
        close(null_fd);
    }
}

static bool write_all(int fd, const uint8_t* data, size_t size)
{
    size_t written = 0;
    while (written < size && g_running.load()) {
        ssize_t ret = write(fd, data + written, size - written);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "write output failed: %s\n", strerror(errno));
            return false;
        }
        if (ret == 0) {
            return false;
        }
        written += static_cast<size_t>(ret);
    }
    return written == size;
}

static void set_thread_name(const char* name)
{
#if defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

class V4L2Capture {
public:
    ~V4L2Capture()
    {
        close_device();
    }

    bool open_device(const Config& cfg)
    {
        width_ = cfg.width;
        height_ = cfg.height;
        frame_size_ = static_cast<size_t>(width_) * height_ * 3 / 2;

        fd_ = open(cfg.device.c_str(), O_RDWR | O_NONBLOCK, 0);
        if (fd_ < 0) {
            fprintf(stderr, "open %s failed: %s\n", cfg.device.c_str(), strerror(errno));
            return false;
        }

        if (!configure_format(cfg)) {
            fprintf(stderr, "VIDIOC_S_FMT failed for single-plane NV12 and multi-plane NV12/NV12M\n");
            list_formats();
            return false;
        }

        v4l2_streamparm parm;
        memset(&parm, 0, sizeof(parm));
        parm.type = buf_type_;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = cfg.fps;
        if (xioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
            fprintf(stderr, "VIDIOC_S_PARM warning: %s\n", strerror(errno));
        }

        v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.count = cfg.v4l2_buffers;
        req.type = buf_type_;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
            fprintf(stderr, "VIDIOC_REQBUFS failed: %s\n", strerror(errno));
            return false;
        }
        if (req.count < 2) {
            fprintf(stderr, "insufficient V4L2 buffers\n");
            return false;
        }

        buffers_.resize(req.count);
        for (uint32_t i = 0; i < req.count; i++) {
            v4l2_buffer buf;
            v4l2_plane planes[VIDEO_MAX_PLANES];
            memset(&buf, 0, sizeof(buf));
            memset(planes, 0, sizeof(planes));
            buf.type = buf_type_;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (multi_plane_) {
                buf.length = VIDEO_MAX_PLANES;
                buf.m.planes = planes;
            }
            if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                fprintf(stderr, "VIDIOC_QUERYBUF failed: %s\n", strerror(errno));
                return false;
            }

            buffers_[i].num_planes = multi_plane_ ? num_planes_ : 1;
            for (int p = 0; p < buffers_[i].num_planes; p++) {
                size_t length = multi_plane_ ? planes[p].length : buf.length;
                unsigned int offset = multi_plane_ ? planes[p].m.mem_offset : buf.m.offset;
                buffers_[i].length[p] = length;
                buffers_[i].start[p] = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, offset);
                if (buffers_[i].start[p] == MAP_FAILED) {
                    fprintf(stderr, "mmap V4L2 buffer plane %d failed: %s\n", p, strerror(errno));
                    return false;
                }
            }
        }

        for (uint32_t i = 0; i < buffers_.size(); i++) {
            if (!queue_buffer(i, "initial")) {
                return false;
            }
        }

        v4l2_buf_type type = buf_type_;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
            fprintf(stderr, "VIDIOC_STREAMON failed: %s\n", strerror(errno));
            return false;
        }
        streaming_ = true;

        fprintf(stderr, "V4L2 capture started: %dx%d %s fps=%d buffers=%zu type=%s planes=%d y_stride=%d uv_stride=%d\n",
                width_, height_, fourcc_to_string(pixel_format_).c_str(), cfg.fps, buffers_.size(),
                multi_plane_ ? "mplane" : "single-plane", num_planes_, y_stride_, uv_stride_);
        return true;
    }

    bool read_frame(std::vector<uint8_t>* out)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(fd_ + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                return true;
            }
            fprintf(stderr, "select camera failed: %s\n", strerror(errno));
            return false;
        }
        if (ret == 0) {
            fprintf(stderr, "camera select timeout\n");
            return true;
        }

        v4l2_buffer buf;
        v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = buf_type_;
        buf.memory = V4L2_MEMORY_MMAP;
        if (multi_plane_) {
            buf.length = VIDEO_MAX_PLANES;
            buf.m.planes = planes;
        }
        if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) {
                return true;
            }
            fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", strerror(errno));
            return false;
        }

        if (buf.index >= buffers_.size()) {
            fprintf(stderr, "invalid V4L2 buffer index %u\n", buf.index);
            return false;
        }

        out->resize(frame_size_);
        copy_tight_nv12(buffers_[buf.index], *out);

        if (!queue_buffer(buf.index, "after capture")) {
            return false;
        }
        return true;
    }

    void close_device()
    {
        if (streaming_) {
            v4l2_buf_type type = buf_type_;
            xioctl(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        for (auto& buffer : buffers_) {
            for (int p = 0; p < buffer.num_planes; p++) {
                if (buffer.start[p] && buffer.start[p] != MAP_FAILED) {
                    munmap(buffer.start[p], buffer.length[p]);
                }
            }
        }
        buffers_.clear();
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

private:
    struct MmapBuffer {
        void* start[VIDEO_MAX_PLANES] = {nullptr};
        size_t length[VIDEO_MAX_PLANES] = {0};
        int num_planes = 0;
    };

    static std::string fourcc_to_string(uint32_t fmt)
    {
        char code[5];
        code[0] = fmt & 0xff;
        code[1] = (fmt >> 8) & 0xff;
        code[2] = (fmt >> 16) & 0xff;
        code[3] = (fmt >> 24) & 0xff;
        code[4] = '\0';
        return std::string(code);
    }

    bool configure_format(const Config& cfg)
    {
        if (try_set_single_plane(cfg, V4L2_FIELD_NONE) ||
            try_set_single_plane(cfg, V4L2_FIELD_ANY) ||
            try_set_multi_plane(cfg, V4L2_PIX_FMT_NV12, V4L2_FIELD_NONE) ||
            try_set_multi_plane(cfg, V4L2_PIX_FMT_NV12, V4L2_FIELD_ANY) ||
            try_set_multi_plane(cfg, V4L2_PIX_FMT_NV12M, V4L2_FIELD_NONE) ||
            try_set_multi_plane(cfg, V4L2_PIX_FMT_NV12M, V4L2_FIELD_ANY)) {
            return true;
        }
        return false;
    }

    bool try_set_single_plane(const Config& cfg, v4l2_field field)
    {
        v4l2_format fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = cfg.width;
        fmt.fmt.pix.height = cfg.height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix.field = field;

        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            return false;
        }
        if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12 ||
            static_cast<int>(fmt.fmt.pix.width) != cfg.width ||
            static_cast<int>(fmt.fmt.pix.height) != cfg.height) {
            return false;
        }

        buf_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        multi_plane_ = false;
        pixel_format_ = fmt.fmt.pix.pixelformat;
        num_planes_ = 1;
        y_stride_ = fmt.fmt.pix.bytesperline > 0 ? fmt.fmt.pix.bytesperline : cfg.width;
        uv_stride_ = y_stride_;
        sizeimage_ = fmt.fmt.pix.sizeimage > 0 ? fmt.fmt.pix.sizeimage : y_stride_ * cfg.height * 3 / 2;
        return true;
    }

    bool try_set_multi_plane(const Config& cfg, uint32_t pixfmt, v4l2_field field)
    {
        v4l2_format fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.fmt.pix_mp.width = cfg.width;
        fmt.fmt.pix_mp.height = cfg.height;
        fmt.fmt.pix_mp.pixelformat = pixfmt;
        fmt.fmt.pix_mp.field = field;

        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            return false;
        }
        if ((fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 &&
             fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12M) ||
            static_cast<int>(fmt.fmt.pix_mp.width) != cfg.width ||
            static_cast<int>(fmt.fmt.pix_mp.height) != cfg.height ||
            fmt.fmt.pix_mp.num_planes <= 0) {
            return false;
        }

        buf_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        multi_plane_ = true;
        pixel_format_ = fmt.fmt.pix_mp.pixelformat;
        num_planes_ = std::min<int>(fmt.fmt.pix_mp.num_planes, VIDEO_MAX_PLANES);
        y_stride_ = fmt.fmt.pix_mp.plane_fmt[0].bytesperline > 0 ?
                    fmt.fmt.pix_mp.plane_fmt[0].bytesperline : cfg.width;
        uv_stride_ = (num_planes_ > 1 && fmt.fmt.pix_mp.plane_fmt[1].bytesperline > 0) ?
                     fmt.fmt.pix_mp.plane_fmt[1].bytesperline : y_stride_;
        sizeimage_ = 0;
        for (int i = 0; i < num_planes_; i++) {
            sizeimage_ += fmt.fmt.pix_mp.plane_fmt[i].sizeimage;
        }
        return true;
    }

    bool queue_buffer(uint32_t index, const char* stage)
    {
        v4l2_buffer buf;
        v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = buf_type_;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = index;
        if (multi_plane_) {
            buf.length = num_planes_;
            buf.m.planes = planes;
        }
        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "VIDIOC_QBUF failed (%s): %s\n", stage, strerror(errno));
            return false;
        }
        return true;
    }

    void list_formats()
    {
        list_formats_for_type(V4L2_BUF_TYPE_VIDEO_CAPTURE, "single-plane");
        list_formats_for_type(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "multi-plane");
    }

    void list_formats_for_type(v4l2_buf_type type, const char* name)
    {
        fprintf(stderr, "Supported V4L2 formats for %s:\n", name);
        for (uint32_t index = 0;; index++) {
            v4l2_fmtdesc desc;
            memset(&desc, 0, sizeof(desc));
            desc.index = index;
            desc.type = type;
            if (xioctl(fd_, VIDIOC_ENUM_FMT, &desc) < 0) {
                if (index == 0) {
                    fprintf(stderr, "  none or enum failed: %s\n", strerror(errno));
                }
                break;
            }
            fprintf(stderr, "  [%u] %s (%s)\n", index,
                    fourcc_to_string(desc.pixelformat).c_str(), desc.description);
        }
    }

    void copy_tight_nv12(const MmapBuffer& buffer, std::vector<uint8_t>& dst) const
    {
        const int width = width_;
        const int height = height_;
        if (multi_plane_ && buffer.num_planes >= 2) {
            const uint8_t* y_src = static_cast<const uint8_t*>(buffer.start[0]);
            const uint8_t* uv_src = static_cast<const uint8_t*>(buffer.start[1]);
            uint8_t* y_dst = dst.data();
            uint8_t* uv_dst = dst.data() + width * height;

            for (int y = 0; y < height; y++) {
                memcpy(y_dst + y * width, y_src + y * y_stride_, width);
            }
            for (int y = 0; y < height / 2; y++) {
                memcpy(uv_dst + y * width, uv_src + y * uv_stride_, width);
            }
            return;
        }

        const uint8_t* src = static_cast<const uint8_t*>(buffer.start[0]);
        if (y_stride_ == width) {
            memcpy(dst.data(), src, frame_size_);
            return;
        }

        uint8_t* y_dst = dst.data();
        const uint8_t* y_src = src;
        for (int y = 0; y < height; y++) {
            memcpy(y_dst + y * width, y_src + y * y_stride_, width);
        }

        uint8_t* uv_dst = dst.data() + width * height;
        const uint8_t* uv_src = src + y_stride_ * height;
        for (int y = 0; y < height / 2; y++) {
            memcpy(uv_dst + y * width, uv_src + y * uv_stride_, width);
        }
    }

    int fd_ = -1;
    int width_ = 0;
    int height_ = 0;
    int y_stride_ = 0;
    int uv_stride_ = 0;
    size_t frame_size_ = 0;
    size_t sizeimage_ = 0;
    bool multi_plane_ = false;
    int num_planes_ = 1;
    uint32_t pixel_format_ = V4L2_PIX_FMT_NV12;
    v4l2_buf_type buf_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bool streaming_ = false;
    std::vector<MmapBuffer> buffers_;
};

static void capture_loop(const Config& cfg, DroppingQueue<RawFrame>* raw_queue, Stats* stats)
{
    set_thread_name("cap_v4l2");
    V4L2Capture camera;
    if (!camera.open_device(cfg)) {
        stats->capture_errors++;
        g_running.store(false);
        raw_queue->stop();
        return;
    }

    uint64_t seq = 0;
    int skipped = 0;
    while (g_running.load()) {
        std::vector<uint8_t> nv12;
        if (!camera.read_frame(&nv12)) {
            stats->capture_errors++;
            g_running.store(false);
            break;
        }
        if (nv12.empty()) {
            continue;
        }

        if (skipped < cfg.skip_frames) {
            skipped++;
            continue;
        }

        RawFrame frame;
        frame.seq = seq++;
        frame.timestamp_ms = now_ms();
        frame.width = cfg.width;
        frame.height = cfg.height;
        frame.nv12 = std::move(nv12);
        raw_queue->push(std::move(frame));
        stats->captured++;
    }
    raw_queue->stop();
}

static void preprocess_loop(int id,
                            int model_width,
                            int model_height,
                            int model_channel,
                            DroppingQueue<RawFrame>* raw_queue,
                            DroppingQueue<PreprocessedFrame>* infer_queue,
                            Stats* stats)
{
    char thread_name[16];
    snprintf(thread_name, sizeof(thread_name), "pre_rga%d", id);
    set_thread_name(thread_name);

    const int bg_color = 114;
    RawFrame raw;
    while (raw_queue->pop(raw)) {
        PreprocessedFrame item;
        item.frame = std::move(raw);
        item.rgb.resize(model_width * model_height * model_channel);
        memset(&item.letterbox, 0, sizeof(item.letterbox));

        image_buffer_t src;
        memset(&src, 0, sizeof(src));
        src.width = item.frame.width;
        src.height = item.frame.height;
        src.format = IMAGE_FORMAT_YUV420SP_NV12;
        src.virt_addr = item.frame.nv12.data();
        src.size = item.frame.nv12.size();

        image_buffer_t dst;
        memset(&dst, 0, sizeof(dst));
        dst.width = model_width;
        dst.height = model_height;
        dst.format = IMAGE_FORMAT_RGB888;
        dst.virt_addr = item.rgb.data();
        dst.size = item.rgb.size();

        int ret = convert_image_with_letterbox(&src, &dst, &item.letterbox, bg_color);
        if (ret != 0) {
            fprintf(stderr, "preprocess worker %d failed on frame %llu\n",
                    id, static_cast<unsigned long long>(item.frame.seq));
            stats->preprocess_errors++;
            continue;
        }

        stats->preprocessed++;
        infer_queue->push(std::move(item));
    }
}

static int run_yolo_preprocessed(rknn_app_context_t* app_ctx,
                                 uint8_t* rgb,
                                 float* text_input,
                                 int text_size,
                                 letterbox_t* letter_box,
                                 object_detect_result_list* od_results)
{
    if (!app_ctx || !rgb || !text_input || !letter_box || !od_results) {
        return -1;
    }

    memset(od_results, 0, sizeof(*od_results));

    std::vector<rknn_input> inputs(app_ctx->io_num.n_input);
    std::vector<rknn_output> outputs(app_ctx->io_num.n_output);
    memset(inputs.data(), 0, inputs.size() * sizeof(rknn_input));
    memset(outputs.data(), 0, outputs.size() * sizeof(rknn_output));

    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = app_ctx->model_width * app_ctx->model_height * app_ctx->model_channel;
    inputs[0].buf = rgb;

    inputs[1].index = 1;
    inputs[1].type = RKNN_TENSOR_FLOAT32;
    inputs[1].fmt = RKNN_TENSOR_UNDEFINED;
    inputs[1].size = text_size * sizeof(float);
    inputs[1].buf = text_input;

    int ret = rknn_inputs_set(app_ctx->rknn_ctx, app_ctx->io_num.n_input, inputs.data());
    if (ret < 0) {
        fprintf(stderr, "rknn_inputs_set failed: %d\n", ret);
        return -1;
    }

    ret = rknn_run(app_ctx->rknn_ctx, nullptr);
    if (ret < 0) {
        fprintf(stderr, "rknn_run failed: %d\n", ret);
        return -1;
    }

    for (int i = 0; i < app_ctx->io_num.n_output; i++) {
        outputs[i].index = i;
        outputs[i].want_float = (!app_ctx->is_quant);
    }

    ret = rknn_outputs_get(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs.data(), NULL);
    if (ret < 0) {
        fprintf(stderr, "rknn_outputs_get failed: %d\n", ret);
        return -1;
    }

    ret = post_process(app_ctx, outputs.data(), letter_box, BOX_THRESH, NMS_THRESH, od_results);
    rknn_outputs_release(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs.data());
    return ret;
}

static rknn_core_mask worker_core_mask(int worker_id)
{
    switch (worker_id % 3) {
    case 0:
        return RKNN_NPU_CORE_0;
    case 1:
        return RKNN_NPU_CORE_1;
    default:
        return RKNN_NPU_CORE_2;
    }
}

static const char* core_mask_name(rknn_core_mask mask)
{
    switch (mask) {
    case RKNN_NPU_CORE_0:
        return "RKNN_NPU_CORE_0";
    case RKNN_NPU_CORE_1:
        return "RKNN_NPU_CORE_1";
    case RKNN_NPU_CORE_2:
        return "RKNN_NPU_CORE_2";
    default:
        return "RKNN_NPU_CORE_AUTO";
    }
}

struct YoloWorker {
    int id = 0;
    rknn_core_mask core_mask = RKNN_NPU_CORE_AUTO;
    rknn_app_context_t ctx;
};

static void inference_loop(YoloWorker* worker,
                           std::vector<float>* text_output,
                           int text_size,
                           DroppingQueue<PreprocessedFrame>* infer_queue,
                           DroppingQueue<DetectedFrame>* result_queue,
                           Stats* stats)
{
    char thread_name[16];
    snprintf(thread_name, sizeof(thread_name), "infer%d", worker->id);
    set_thread_name(thread_name);

    PreprocessedFrame item;
    while (infer_queue->pop(item)) {
        DetectedFrame detected;
        detected.frame = std::move(item.frame);
        detected.worker_id = worker->id;
        int ret = run_yolo_preprocessed(&worker->ctx,
                                        item.rgb.data(),
                                        text_output->data(),
                                        text_size,
                                        &item.letterbox,
                                        &detected.results);
        if (ret != 0) {
            fprintf(stderr, "inference worker %d failed on frame %llu\n",
                    worker->id, static_cast<unsigned long long>(detected.frame.seq));
            stats->inference_errors++;
            continue;
        }

        stats->inferred++;
        result_queue->push(std::move(detected));
    }
}

static void render_loop(int output_fd,
                        int stream_fps,
                        DroppingQueue<DetectedFrame>* result_queue,
                        Stats* stats)
{
    set_thread_name("render_out");
    bool have_last = false;
    uint64_t last_seq = 0;
    const auto min_interval = std::chrono::microseconds(1000000 / std::max(1, stream_fps));
    auto last_emit = std::chrono::steady_clock::now() - min_interval;
    DetectedFrame detected;
    while (result_queue->pop(detected)) {
        if (have_last && detected.frame.seq <= last_seq) {
            stats->stale_results++;
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - last_emit < min_interval) {
            stats->stale_results++;
            continue;
        }
        last_emit = now;
        have_last = true;
        last_seq = detected.frame.seq;

        image_buffer_t image;
        memset(&image, 0, sizeof(image));
        image.width = detected.frame.width;
        image.height = detected.frame.height;
        image.format = IMAGE_FORMAT_YUV420SP_NV12;
        image.virt_addr = detected.frame.nv12.data();
        image.size = detected.frame.nv12.size();

        for (int i = 0; i < detected.results.count; i++) {
            object_detect_result* det = &(detected.results.results[i]);
            int x1 = std::max(0, det->box.left);
            int y1 = std::max(0, det->box.top);
            int x2 = std::min(image.width - 1, det->box.right);
            int y2 = std::min(image.height - 1, det->box.bottom);
            if (x2 <= x1 || y2 <= y1) {
                continue;
            }

            draw_rectangle(&image, x1, y1, x2 - x1, y2 - y1, COLOR_BLUE, 4);
            char text[128];
            snprintf(text, sizeof(text), "%s %.1f%%", coco_cls_to_name(det->cls_id), det->prop * 100.0f);
            draw_text(&image, text, x1, std::max(0, y1 - 28), COLOR_RED, 18);
        }

        if (!write_all(output_fd, detected.frame.nv12.data(), detected.frame.nv12.size())) {
            stats->output_errors++;
            g_running.store(false);
            break;
        }
        stats->last_stream_age_ms.store(now_ms() - detected.frame.timestamp_ms);

        uint64_t streamed = ++stats->streamed;
        if (streamed % 120 == 0) {
            fprintf(stderr,
                    "streamed=%llu captured=%llu preprocessed=%llu inferred=%llu stale=%llu\n",
                    static_cast<unsigned long long>(streamed),
                    static_cast<unsigned long long>(stats->captured.load()),
                    static_cast<unsigned long long>(stats->preprocessed.load()),
                    static_cast<unsigned long long>(stats->inferred.load()),
                    static_cast<unsigned long long>(stats->stale_results.load()));
        }
    }
}

static void stats_loop(DroppingQueue<RawFrame>* raw_queue,
                       DroppingQueue<PreprocessedFrame>* infer_queue,
                       DroppingQueue<DetectedFrame>* result_queue,
                       Stats* stats)
{
    set_thread_name("stats");

    uint64_t last_captured = stats->captured.load();
    uint64_t last_preprocessed = stats->preprocessed.load();
    uint64_t last_inferred = stats->inferred.load();
    uint64_t last_streamed = stats->streamed.load();
    uint64_t last_raw_drops = raw_queue->drops();
    uint64_t last_infer_drops = infer_queue->drops();
    uint64_t last_result_drops = result_queue->drops();
    uint64_t last_stale = stats->stale_results.load();

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        uint64_t captured = stats->captured.load();
        uint64_t preprocessed = stats->preprocessed.load();
        uint64_t inferred = stats->inferred.load();
        uint64_t streamed = stats->streamed.load();
        uint64_t raw_drops = raw_queue->drops();
        uint64_t infer_drops = infer_queue->drops();
        uint64_t result_drops = result_queue->drops();
        uint64_t stale = stats->stale_results.load();
        uint64_t age_ms = stats->last_stream_age_ms.load();

        fprintf(stderr,
                "[stats] fps cap=%llu pre=%llu infer=%llu stream=%llu | "
                "age_ms=%llu | queue raw=%zu/%zu infer=%zu/%zu result=%zu/%zu | "
                "drops raw=%llu(+%llu) infer=%llu(+%llu) result=%llu(+%llu) stale=%llu(+%llu) | "
                "errors cap=%llu pre=%llu infer=%llu out=%llu\n",
                static_cast<unsigned long long>(captured - last_captured),
                static_cast<unsigned long long>(preprocessed - last_preprocessed),
                static_cast<unsigned long long>(inferred - last_inferred),
                static_cast<unsigned long long>(streamed - last_streamed),
                static_cast<unsigned long long>(age_ms),
                raw_queue->size(), raw_queue->capacity(),
                infer_queue->size(), infer_queue->capacity(),
                result_queue->size(), result_queue->capacity(),
                static_cast<unsigned long long>(raw_drops),
                static_cast<unsigned long long>(raw_drops - last_raw_drops),
                static_cast<unsigned long long>(infer_drops),
                static_cast<unsigned long long>(infer_drops - last_infer_drops),
                static_cast<unsigned long long>(result_drops),
                static_cast<unsigned long long>(result_drops - last_result_drops),
                static_cast<unsigned long long>(stale),
                static_cast<unsigned long long>(stale - last_stale),
                static_cast<unsigned long long>(stats->capture_errors.load()),
                static_cast<unsigned long long>(stats->preprocess_errors.load()),
                static_cast<unsigned long long>(stats->inference_errors.load()),
                static_cast<unsigned long long>(stats->output_errors.load()));

        last_captured = captured;
        last_preprocessed = preprocessed;
        last_inferred = inferred;
        last_streamed = streamed;
        last_raw_drops = raw_drops;
        last_infer_drops = infer_drops;
        last_result_drops = result_drops;
        last_stale = stale;
    }
}

static bool build_text_embedding(const Config& cfg, std::vector<float>* text_output, int* text_size)
{
    int text_lines = 0;
    char** input_texts = read_lines_from_file(cfg.text_path.c_str(), &text_lines);
    if (input_texts == NULL || text_lines <= 0) {
        fprintf(stderr, "read input texts failed: %s\n", cfg.text_path.c_str());
        return false;
    }

    rknn_clip_context clip_ctx;
    memset(&clip_ctx, 0, sizeof(clip_ctx));
    int ret = init_clip_text_model(&clip_ctx, cfg.text_model_path.c_str());
    if (ret != 0) {
        fprintf(stderr, "init clip text model failed: %d\n", ret);
        free_lines(input_texts, text_lines);
        return false;
    }

    int embed_dim = clip_ctx.output_attrs[0].dims[1];
    *text_size = text_lines * embed_dim;
    text_output->resize(*text_size);

    fprintf(stderr, "building text embedding once: lines=%d embed_dim=%d floats=%d\n",
            text_lines, embed_dim, *text_size);
    ret = inference_clip_text_model(&clip_ctx, input_texts, text_lines, text_output->data());
    release_clip_text_model(&clip_ctx);
    free_lines(input_texts, text_lines);

    if (ret != 0) {
        fprintf(stderr, "inference clip text model failed: %d\n", ret);
        return false;
    }
    return true;
}

int main(int argc, char** argv)
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    Config cfg;
    if (!parse_args(argc, argv, &cfg)) {
        return -1;
    }

    int output_fd = open_output_fd(cfg.output_path);
    if (output_fd < 0) {
        return -1;
    }
    silence_stdout();

    fprintf(stderr, "Realtime YOLO-World starting\n");
    fprintf(stderr, "device=%s capture=%dx%d@%d stream_fps=%d output=%s preprocess_workers=%d inference_workers=%d\n",
            cfg.device.c_str(), cfg.width, cfg.height, cfg.fps, cfg.stream_fps, cfg.output_path.c_str(),
            cfg.preprocess_workers, cfg.inference_workers);

    if (init_post_process_with_label_path(cfg.text_path.c_str()) != 0) {
        fprintf(stderr, "init post process failed\n");
        close(output_fd);
        return -1;
    }

    std::vector<float> text_output;
    int text_size = 0;
    if (!build_text_embedding(cfg, &text_output, &text_size)) {
        deinit_post_process();
        close(output_fd);
        return -1;
    }

    std::vector<YoloWorker> workers(cfg.inference_workers);
    for (int i = 0; i < cfg.inference_workers; i++) {
        workers[i].id = i;
        workers[i].core_mask = worker_core_mask(i);
        memset(&workers[i].ctx, 0, sizeof(workers[i].ctx));
        int ret = init_yolo_world_model(&workers[i].ctx, cfg.yolo_model_path.c_str());
        if (ret != 0) {
            fprintf(stderr, "init yolo worker %d failed: %d\n", i, ret);
            g_running.store(false);
            for (int j = 0; j < i; j++) {
                release_yolo_world_model(&workers[j].ctx);
            }
            deinit_post_process();
            close(output_fd);
            return -1;
        }

        ret = rknn_set_core_mask(workers[i].ctx.rknn_ctx, workers[i].core_mask);
        if (ret != RKNN_SUCC) {
            fprintf(stderr, "warning: worker %d set core mask %s failed: %d\n",
                    i, core_mask_name(workers[i].core_mask), ret);
        } else {
            fprintf(stderr, "worker %d bound to %s\n", i, core_mask_name(workers[i].core_mask));
        }
    }

    int expected_text = workers[0].ctx.input_attrs[1].n_elems;
    if (expected_text > 0 && expected_text != text_size) {
        fprintf(stderr, "text embedding size mismatch: model expects %d floats, got %d\n",
                expected_text, text_size);
        for (auto& worker : workers) {
            release_yolo_world_model(&worker.ctx);
        }
        deinit_post_process();
        close(output_fd);
        return -1;
    }

    int model_width = workers[0].ctx.model_width;
    int model_height = workers[0].ctx.model_height;
    int model_channel = workers[0].ctx.model_channel;
    fprintf(stderr, "YOLO input tensor: %dx%dx%d\n", model_width, model_height, model_channel);

    DroppingQueue<RawFrame> raw_queue(cfg.raw_queue_depth);
    DroppingQueue<PreprocessedFrame> infer_queue(cfg.infer_queue_depth);
    DroppingQueue<DetectedFrame> result_queue(cfg.result_queue_depth);
    Stats stats;

    std::thread capture_thread(capture_loop, std::cref(cfg), &raw_queue, &stats);

    std::vector<std::thread> preprocess_threads;
    for (int i = 0; i < cfg.preprocess_workers; i++) {
        preprocess_threads.emplace_back(preprocess_loop,
                                        i,
                                        model_width,
                                        model_height,
                                        model_channel,
                                        &raw_queue,
                                        &infer_queue,
                                        &stats);
    }

    std::vector<std::thread> inference_threads;
    for (int i = 0; i < cfg.inference_workers; i++) {
        inference_threads.emplace_back(inference_loop,
                                       &workers[i],
                                       &text_output,
                                       text_size,
                                       &infer_queue,
                                       &result_queue,
                                       &stats);
    }

    std::thread render_thread(render_loop, output_fd, cfg.stream_fps, &result_queue, &stats);
    std::thread stats_thread(stats_loop, &raw_queue, &infer_queue, &result_queue, &stats);

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    raw_queue.stop();
    if (capture_thread.joinable()) {
        capture_thread.join();
    }

    for (auto& t : preprocess_threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    infer_queue.stop();
    for (auto& t : inference_threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    result_queue.stop();
    if (render_thread.joinable()) {
        render_thread.join();
    }
    if (stats_thread.joinable()) {
        stats_thread.join();
    }

    for (auto& worker : workers) {
        release_yolo_world_model(&worker.ctx);
    }
    deinit_post_process();
    close(output_fd);

    fprintf(stderr,
            "Realtime YOLO-World stopped. captured=%llu preprocessed=%llu inferred=%llu streamed=%llu "
            "queue_drops(raw=%llu infer=%llu result=%llu) stale=%llu errors(capture=%llu preprocess=%llu inference=%llu output=%llu)\n",
            static_cast<unsigned long long>(stats.captured.load()),
            static_cast<unsigned long long>(stats.preprocessed.load()),
            static_cast<unsigned long long>(stats.inferred.load()),
            static_cast<unsigned long long>(stats.streamed.load()),
            static_cast<unsigned long long>(raw_queue.drops()),
            static_cast<unsigned long long>(infer_queue.drops()),
            static_cast<unsigned long long>(result_queue.drops()),
            static_cast<unsigned long long>(stats.stale_results.load()),
            static_cast<unsigned long long>(stats.capture_errors.load()),
            static_cast<unsigned long long>(stats.preprocess_errors.load()),
            static_cast<unsigned long long>(stats.inference_errors.load()),
            static_cast<unsigned long long>(stats.output_errors.load()));

    return 0;
}
