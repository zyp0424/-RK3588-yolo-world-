#!/bin/sh

set -eu

export LD_LIBRARY_PATH=./lib:${LD_LIBRARY_PATH:-}

TEXT_MODEL="${TEXT_MODEL:-model/clip_text_fp16.rknn}"
YOLO_MODEL="${YOLO_MODEL:-model/yolo_world_v2s_i8.rknn}"
TEXT_FILE="${TEXT_FILE:-model/detect_classes.txt}"
DEVICE="${DEVICE:-/dev/video11}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"
CAPTURE_FPS="${CAPTURE_FPS:-60}"
STREAM_FPS="${STREAM_FPS:-60}"
FPS="${FPS:-$CAPTURE_FPS}"
UDP_URL="${UDP_URL:-udp://192.168.1.141:1235?pkt_size=1316}"
PIPE="${PIPE:-/tmp/yolo_world_realtime_nv12.pipe}"
BITRATE="${BITRATE:-10M}"
V4L2_BUFFERS="${V4L2_BUFFERS:-8}"
WORKERS="${WORKERS:-3}"
PREPROCESS_WORKERS="${PREPROCESS_WORKERS:-6}"
RAW_QUEUE="${RAW_QUEUE:-2}"
INFER_QUEUE="${INFER_QUEUE:-1}"
RESULT_QUEUE="${RESULT_QUEUE:-1}"

if [ ! -f "$TEXT_MODEL" ] && [ -f "./clip_text_fp16.rknn" ]; then
  TEXT_MODEL="./clip_text_fp16.rknn"
fi

if [ ! -f "$YOLO_MODEL" ] && [ -f "./yolo_world_v2s_i8.rknn" ]; then
  YOLO_MODEL="./yolo_world_v2s_i8.rknn"
fi

cleanup() {
  if [ -n "${FFMPEG_PID:-}" ]; then
    kill "$FFMPEG_PID" 2>/dev/null || true
    wait "$FFMPEG_PID" 2>/dev/null || true
  fi
  rm -f "$PIPE"
}

trap cleanup INT TERM EXIT

rm -f "$PIPE"
mkfifo "$PIPE"

ffmpeg -hide_banner -loglevel warning \
  -fflags nobuffer \
  -flags low_delay \
  -f rawvideo \
  -pix_fmt nv12 \
  -s "${WIDTH}x${HEIGHT}" \
  -r "$STREAM_FPS" \
  -i "$PIPE" \
  -an \
  -c:v h264_rkmpp \
  -b:v "$BITRATE" \
  -g 10 \
  -bf 0 \
  -f mpegts \
  -flush_packets 1 \
  -muxdelay 0 \
  -muxpreload 0 \
  "$UDP_URL" &
FFMPEG_PID=$!

./rknn_yolo_world_realtime \
  "$TEXT_MODEL" \
  "$TEXT_FILE" \
  "$YOLO_MODEL" \
  --device "$DEVICE" \
  --width "$WIDTH" \
  --height "$HEIGHT" \
  --fps "$FPS" \
  --stream-fps "$STREAM_FPS" \
  --buffers "$V4L2_BUFFERS" \
  --skip 5 \
  --preprocess-workers "$PREPROCESS_WORKERS" \
  --workers "$WORKERS" \
  --raw-queue "$RAW_QUEUE" \
  --infer-queue "$INFER_QUEUE" \
  --result-queue "$RESULT_QUEUE" \
  --output "$PIPE"
