set positional-arguments

profile := "atom_voice_s3r"
sketch := "firmware/live_chat"
output := "build/live/firmware"

default:
  @just --list

boards:
  arduino-cli board list

# シリアルログを表示（終了は Ctrl+C）
monitor port:
  arduino-cli monitor --port "$1" --config baudrate=115200

# デバイス認証用トークンを .enc.env に暗号化保存（既存なら変更しない）
live-init:
  sops exec-env .enc.env 'python3 firmware/live.py init'

# 固定したボード定義で GPT-Live ファームウェアをビルド
live-build:
  arduino-cli compile --profile {{profile}} \
    --build-property "tools.ctags.path=$ARDUINO_CTAGS_PATH" \
    --build-path {{output}} \
    {{sketch}}

live-upload port: live-build
  arduino-cli upload --profile {{profile}} \
    --input-dir {{output}} \
    --port "$1" \
    {{sketch}}

# Wi-Fi・接続先・DEVICE_TOKEN を本体に保存（初回・設定変更時のみ）
live-connect port:
  LIVE_PORT="$1" sops exec-env .enc.env 'python3 -u firmware/live.py configure "$LIVE_PORT"'

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
