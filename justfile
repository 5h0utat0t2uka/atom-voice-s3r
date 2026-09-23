set positional-arguments

default:
  @just --list

boards:
  arduino-cli board list

# 固定した依存物でコンパイル
build:
  arduino-cli compile --profile cores3 \
    --build-property "tools.ctags.path=$ARDUINO_CTAGS_PATH" \
    firmware/cores3_check

# ビルド後、全消去せずに書き込み
upload port: build
  arduino-cli upload --profile cores3 \
    --port "$1" \
    firmware/cores3_check

# ビルド後、本体フラッシュ内の全データを消去して書き込み
upload-erase port: build
  arduino-cli upload --profile cores3 \
    --fqbn m5stack:esp32:m5stack_cores3:PSRAM=enabled,EraseFlash=all \
    --port "$1" \
    firmware/cores3_check
