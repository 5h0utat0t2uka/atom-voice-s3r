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

# SOPS のキーを生成プロセスの環境変数にのみ渡す（保存済みなら API 呼び出しなし）
speech-generate:
  sops exec-env .enc.env 'python3 firmware/speech.py generate'

# 保存済み音声を含む再生専用ファームウェアをビルド
speech-build:
  python3 firmware/speech.py prepare
  arduino-cli compile --profile {{profile}} \
    --build-property "tools.ctags.path=$ARDUINO_CTAGS_PATH" \
    --output-dir build/speech/firmware \
    build/speech/speech_playback

# 再生専用テストを書き込み（録音テストに戻すときは just upload）
speech-upload port: speech-build
  arduino-cli upload --profile {{profile}} \
    --input-dir build/speech/firmware \
    --port "$1" \
    build/speech/speech_playback

# 接続情報を含まない Wi-Fi / HTTPS 疎通テストをビルド
wifi-build:
  python3 firmware/wifi.py prepare
  arduino-cli compile --profile {{profile}} \
    --build-property "tools.ctags.path=$ARDUINO_CTAGS_PATH" \
    --output-dir build/wifi/firmware \
    build/wifi/wifi_check

wifi-upload port: wifi-build
  arduino-cli upload --profile {{profile}} \
    --input-dir build/wifi/firmware \
    --port "$1" \
    build/wifi/wifi_check

# monitor を終了して実行。SOPS の SSID / PASS を USB で RAM に渡す
wifi-check port:
  WIFI_CHECK_PORT="$1" sops exec-env .enc.env 'python3 -u firmware/wifi.py check "$WIFI_CHECK_PORT"'

# デバイス認証用トークンを .enc.env に暗号化保存（既存なら変更しない）
realtime-init:
  sops exec-env .enc.env 'python3 firmware/realtime.py init'

# ボタン録音 → Realtime 音声応答（p コマンドで固定テキストの再生テストも可能）
realtime-build:
  python3 firmware/realtime.py prepare
  arduino-cli compile --profile {{profile}} \
    --build-property "tools.ctags.path=$ARDUINO_CTAGS_PATH" \
    --output-dir build/realtime/firmware \
    build/realtime/realtime_playback

realtime-upload port: realtime-build
  arduino-cli upload --profile {{profile}} \
    --input-dir build/realtime/firmware \
    --port "$1" \
    build/realtime/realtime_playback

# SOPS の Wi-Fi 情報・DEVICE_TOKEN・REALTIME_TOKEN_URL を USB で渡す
realtime-connect port:
  REALTIME_PORT="$1" sops exec-env .enc.env 'python3 -u firmware/realtime.py configure "$REALTIME_PORT"'

# Terraform の依存 Provider を取得（秘密情報・Vercel 接続は不要）
infra-init:
  @umask 077; terraform -chdir=infra init -input=false

infra-check:
  terraform -chdir=infra fmt -check
  terraform -chdir=infra validate

# 既存リソースの import と変更を確認し、適用する plan を保存
infra-plan:
  sops exec-env .enc.env 'sh infra/terraform.sh plan -input=false -out=.terraform/reviewed.tfplan'

# infra-plan で確認した plan を適用（アプリのデプロイは Git 連携が担当）
infra-apply:
  sops exec-env .enc.env 'sh infra/terraform.sh apply .terraform/reviewed.tfplan'

# 本番 Firewall と認証を検証。短命トークンを1件発行し、音声は生成しない
infra-verify:
  sops exec-env .enc.env 'node infra/verify.mjs'

# 本番の発行制限を検証。最大6件の短命トークンを発行（実行中は本体を使わない）
infra-verify-limit:
  sops exec-env .enc.env 'node infra/verify.mjs --rate-limit'
