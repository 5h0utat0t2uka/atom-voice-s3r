# Atom VoiceS3R
![Atom VoiceS3R](./docs/C008-C-atom_sound_page_01.png)

Atom VoiceS3R と OpenAI Realtime API を使う音声チャットボットの開発用プロジェクトです。
Arduino の操作には Nix / direnv / just を使います。機器の基本動作と音声テストは
[firmware/README.md](firmware/README.md) を参照してください。

## Realtime の音声受信・再生テスト

今回は本体のボタン操作で、固定の日本語テキストを Realtime API に送ります。
応答音声を PSRAM に受信し終えてから再生します。マイク入力・受信中の再生・会話履歴の継続は
次の段階です。1回のテストごとに新しいセッションを作り、応答完了後に接続を閉じます。

```text
本体 ─ HTTPS + DEVICE_TOKEN → Vercel /api/realtime/token
                                      └ OpenAI の短命トークンを発行
本体 ─ WSS + 短命トークン → OpenAI Realtime API
```

### 1. デバイス認証情報

`.enc.env` に `OPENAI_API_KEY`、`SSID`、`PASS` を SOPS で保存し、次を実行します。

```sh
just realtime-init
```

`DEVICE_TOKEN` を生成して `.enc.env` に暗号化保存します。既存の値は変更しません。
値はログやコマンド引数に出しません。

### 2. Vercel の設定

このリポジトリを Vercel にインポートし、Framework Preset を Next.js、Root Directory を
リポジトリルートにします。Production のサーバー環境変数に次の2つを設定します。
`NEXT_PUBLIC_` は付けません。Vercel は SOPS ファイルを自動復号しません。

| 環境変数 | 値 |
| --- | --- |
| `OPENAI_API_KEY` | `.enc.env` と同じ OpenAI API キー |
| `DEVICE_TOKEN` | `realtime-init` が生成した64桁の値 |

`SSID`、`PASS` は Vercel には登録しません。
System Environment Variables の公開を有効にします
（`VERCEL` / `VERCEL_ENV` / `VERCEL_PROJECT_PRODUCTION_URL` を利用）。

Vercel の Firewall → Configure → New Rule で、次を設定して Publish します。

- 条件: `@vercel/firewall`
- Rate limit ID: `realtime-token`
- 発行頻度: 60秒あたり6回
- 上限超過時: Deny

SDK で認証済みデバイスごとに制限します。ルールがない・確認できない場合は API が503を返し、
トークンを発行しません。ローカルの `next dev` や Preview でも発行せず503にします。
Firewall のカウンターはリージョン単位です。OpenAI の料金上限を保証する仕組みではありません。

本体からログイン画面なしでアクセスできる Production URL を使います。Deployment Protection が
有効な Preview URL は、このテストでは使いません。環境変数を変更したら再デプロイしてください。

### 3. 本体へ設定・実行

デプロイ先が確定したら、SOPS で `.enc.env` に次を追加します。

```dotenv
REALTIME_TOKEN_URL=https://your-project.vercel.app/api/realtime/token
```

```sh
# monitor を終了してから実行
just realtime-upload /dev/cu.usbmodem1101
just realtime-connect /dev/cu.usbmodem1101
just monitor /dev/cu.usbmodem1101
```

`REALTIME_READY` の後、前面ボタンを押すか、モニターから `p` + Enter を送ります。
**毎回 API 利用料金が発生します。** 失敗時の自動再試行はしません。
`REALTIME_TEST_DONE` の表示と実際の音声で結果を確認してください。

- 接続情報は USB から RAM に渡します。再起動後は `realtime-connect` を再実行します。
- 通常の OpenAI API キーは本体に送りません。短命トークンは接続時に取得します。
- HTTPS / WSS ともにボードパッケージの CA バンドルで証明書・ホスト名を検証します。
- 短命トークンの接続開始期限は60秒です。期限は接続済みセッションの終了期限ではなく、
  期限内には複数セッションを作成できます。セッション設定もクライアントが変更可能です。
- モデルは `gpt-realtime-2.1`、声は `marin`、PCM16 / mono / 24 kHz。
  出力は256トークンを上限に要求し、本体では最大20秒の音声・128 KiBのイベントを受け付けます。
  上限超過・不完全な応答では再生せず失敗を報告します。
- 冒頭には200 msの無音を付けて再生します。受信音声は終了時に消去します。
- WebSocket は同梱 ESP-IDF の TCP transport を使います。追加の Arduino ライブラリは不要です。

`TOKEN_HTTP 401` はデバイス認証の不一致、429は発行頻度超過、503はサーバー設定・Firewallを確認します。
502は OpenAI 側の発行失敗または不正な応答です。`WS_HTTP` が101以外なら WebSocket 接続に失敗しています。
応答本文・認証ヘッダー・音声の文字起こしはログに表示しません。

### 検証

```sh
pnpm test
pnpm exec tsc --noEmit
pnpm build
python3 -m unittest discover -s firmware -p 'test_*.py'
just realtime-build
```

単体テストは OpenAI API を呼びません。本番の発行制限と機器での音声受信はデプロイ後に確認します。

### 公式資料

- [OpenAI Realtime WebSocket](https://developers.openai.com/api/docs/guides/voice-websockets?api=realtime)
- [OpenAI client secrets](https://developers.openai.com/api/reference/resources/realtime/subresources/client_secrets/methods/create)
- [OpenAI Realtime conversations](https://developers.openai.com/api/docs/guides/realtime-conversations)
- [Vercel Rate Limiting SDK](https://vercel.com/docs/vercel-firewall/vercel-waf/rate-limiting-sdk)
- [Vercel System environment variables](https://vercel.com/docs/environment-variables/system-environment-variables)
