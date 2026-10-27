#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Sửa lại đường dẫn này trỏ đúng đến bộ tool Hexagon SDK đã cài.
TOOLS_BIN="$SCRIPT_DIR/../tools/hexagon-sdk/tools/HEXAGON_Tools/19.0.04/Tools/bin"

CLANG="$TOOLS_BIN/hexagon-clang"
SIM="$TOOLS_BIN/hexagon-sim"
SRC="$SCRIPT_DIR/hvx_int8_add.c"
OUT="$SCRIPT_DIR/hvx_int8_add"

if [[ ! -x "$CLANG" || ! -x "$SIM" ]]; then
    echo "Error: không tìm thấy hexagon-clang hoặc hexagon-sim trong: $TOOLS_BIN" >&2
    exit 1
fi

echo "=== Compiling Code Thực Hành HVX ==="
# -mv73 hoặc -mv75 tùy đời chip giả lập mày muốn target. 
# -mhvx kích hoạt tập lệnh Vector mở rộng. -mhvx-length=128B ép dùng thanh ghi 128 bytes.
$CLANG -O2 -mv73 -mhvx -mhvx-length=128B "$SRC" -o "$OUT"

echo "=== Running on Hexagon Simulator ==="
# Chạy trực tiếp bằng simulator mà không cần hypervisor (vì bài này không dùng HMX, không lo lỗi vaddr VTCM)
$SIM "$OUT"

echo "=== Done ==="
