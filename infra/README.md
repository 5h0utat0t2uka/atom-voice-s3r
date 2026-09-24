# Vercel を Terraform で管理する

このプロジェクトでは、Vercel プロジェクト、`OPENAI_API_KEY` / `DEVICE_TOKEN`、
`realtime-token` の発行制限を Terraform で管理します。
アプリのビルド・デプロイは引き続き Vercel の Git 連携で行います。
Next.js のルートは `web/` で、`vercel_project.app.root_directory = "web"` としています。
pnpm workspace と lockfile はリポジトリルートに置きます。
Functions は `resource_config.function_default_regions = ["hnd1"]` で東京を指定し、
既存の Fluid Compute を有効のまま維持します。リージョン変更は次の Git 連携デプロイから反映されます。
本体とトークン発行 API の通信距離を短くできますが、API から OpenAI への通信も含むため、
改善幅は本体の `TOKEN_MS` で確認します。音声自体は本体と OpenAI の直接通信です。

## 通常の操作

リポジトリルートで実行します。Terraform は Nix の開発環境に含めています。
Vercel Provider は `5.16.0` に固定し、チェックサムを `.terraform.lock.hcl` で管理します。

```sh
just infra-init    # 初回・Provider 更新時
just infra-check   # フォーマット・スキーマ検証
just infra-plan    # 現在の Vercel 設定を読み、差分と適用する plan を保存
just infra-apply   # 直前に確認した plan を適用
```

`infra-apply` は保存済み plan をそのまま適用します。追加の確認入力はありません。
設定を編集したら必ず `infra-plan` をやり直してください。
通常、反映直後の `infra-plan` は `No changes` になります。
Firewall の変更はアプリの再デプロイなしで反映されます。
環境変数の値を変更したときは、その後にアプリを再デプロイしてください。

`web/` への初回移行では、Root Directory の plan / apply を行ってから、
移動を含む変更をコミット・push します。設定変更だけでは既存の公開済みデプロイは置き換わりません。
移行前のコミットには `web/` がないため、そのコミットを新規にデプロイし直さないでください。

## 秘密情報

次の値は `.enc.env` で管理します。

| 変数 | 用途 |
| --- | --- |
| `VERCEL_API_TOKEN` | Terraform が Vercel を操作する認証情報 |
| `OPENAI_API_KEY` | Vercel の同名環境変数に登録する値 |
| `DEVICE_TOKEN` | Vercel と本体で共有する認証情報 |
| `REALTIME_TOKEN_URL` | 本体・疎通検証が利用する設定済み本番 URL |

SOPS が復号した値は実行プロセスの環境変数として渡します。
OpenAI キーとデバイストークンは Terraform の `ephemeral` 変数と `value_wo` を使用し、
plan / state に秘密値を保存しません。`VERCEL_API_TOKEN` も Provider の環境変数から読みます。
`SSID` / `PASS` と `VERCEL_API_TOKEN` は Vercel の環境変数には登録しません。

Vercel の既存環境変数の対象は Production と Preview で、取り込み時に保持しています。
`project.auto.tfvars.json` にはリソース ID と対象環境だけを記録し、Git で管理します。
Vercel の API Route は引き続き Production でのみトークンを発行します。

秘密値を更新する場合は `.enc.env` を編集し、`variables.tf` の `secret_versions` の
該当番号を増やしてから plan / apply します。write-only の値は読み戻せないため、
番号を変更しなければ値の変更を検出できません。Vercel の GUI で秘密値を変更した場合も同様です。

## 初回取り込みと state

`imports.tf` に既存プロジェクトと2つの環境変数の import を定義しています。
同じリソースが state にあれば import は再実行されません。
プロジェクトと環境変数には `prevent_destroy` を設定しており、削除・再作成の plan を拒否します。

Firewall は作成済みで、`import_firewall = true` にしています。
state の復旧時も既存の Firewall を取り込みます。公開済み設定がないプロジェクトでのみ
`false` を指定します。既存のカスタムルールがある場合は、削除差分がないかを確認します。
このリソースは Firewall 設定全体を管理するため、以後のルール追加も Terraform で行います。

state はこの Mac の `infra/terraform.tfstate` に保存します。
state、バックアップ、保存済み plan、`.terraform/` は Git の対象外です。
state は暗号化されたバックアップの対象にしてください。
別の Mac / CI でも実行するようになったら、同じ state を共有できるバックエンドへ
`terraform init -migrate-state` で移行します。それまではこの Mac から実行します。

## 発行制限と実接続の検証

制限は Realtime のトークン発行と GPT-Live のセッション開始で共有し、60秒あたり6回、超過時は HTTP 429 です。`rate_limit_api_id = realtime-token` に対し、SDK が送る
`x-vercel-rate-limit-key` ごとに集計します。IP アドレスで全デバイスをまとめません。
集計はリージョン単位で、OpenAI の料金上限を保証するものではありません。

```sh
just infra-verify        # 認証なし401・認証あり200、短命トークンを1件発行
just infra-verify-limit  # 次の集計期間まで待機し、6回成功・7回目429を確認
```

本体と同じ本番 API を呼びます。音声は生成せず、取得した秘密値も表示しません。
制限テストは実際の発行枠を使うため、実行中は本体を操作せず、終了後は1分待ってください。

## 公式資料

- [Vercel Provider](https://registry.terraform.io/providers/vercel/vercel/5.16.0/docs)
- [Vercel のモノレポ構成](https://vercel.com/docs/monorepos)
- [Firewall resource](https://github.com/vercel/terraform-provider-vercel/blob/v5.16.0/docs/resources/firewall_config.md)
- [環境変数の write-only 設定](https://github.com/vercel/terraform-provider-vercel/blob/v5.16.0/docs/resources/project_environment_variable.md)
- [Terraform の秘密情報管理](https://developer.hashicorp.com/terraform/language/manage-sensitive-data)
- [Vercel Rate Limiting SDK](https://vercel.com/docs/vercel-firewall/vercel-waf/rate-limiting-sdk)

- [Functions のリージョン設定](https://vercel.com/docs/functions/configuring-functions/region)
- [Project resource の resource_config](https://github.com/vercel/terraform-provider-vercel/blob/v5.16.0/docs/resources/project.md#nested-schema-for-resource_config)
