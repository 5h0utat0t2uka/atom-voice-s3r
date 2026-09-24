set positional-arguments

sketch := "firmware/atom_voice_s3r_check"
profile := "atom_voice_s3r"
output := "build/firmware"

default:
  @just --list

boards:
  arduino-cli board list

# 固定した依存物でコンパイル
build:
  arduino-cli compile --profile {{profile}} \
    --build-property "tools.ctags.path=$ARDUINO_CTAGS_PATH" \
    --output-dir {{output}} \
    {{sketch}}

# ビルド後、全消去せずに書き込み
upload port: build
  arduino-cli upload --profile {{profile}} \
    --input-dir {{output}} \
    --port "$1" \
    {{sketch}}

# ビルド後、本体フラッシュ内の全データを消去して書き込み
upload-erase port: build
  arduino-cli upload --profile {{profile}} \
    --board-options EraseFlash=all \
    --input-dir {{output}} \
    --port "$1" \
    {{sketch}}

# シリアルログを表示（終了は Ctrl+C）
monitor port:
  arduino-cli monitor --port "$1" --config baudrate=115200

# 次のボタン録音を USB 経由で WAV 保存（monitor は先に終了）
capture port dma="128":
  python3 firmware/capture.py "$1" --dma "$2"
