## 開発環境

リポジトリルートで `direnv allow` を実行するか、`nix develop` に入って作業します。
Node.js **24.21.0** と pnpm **10.34.0** は `nix/fixed-node.nix` で固定しています。
グローバルの Node.js / pnpm と混在させず、実行前に `node -v` / `pnpm -v` を確認してください。

```sh
pnpm install --frozen-lockfile
pnpm dev
```

pnpm workspace と lockfile はルートで管理します。既存の依存関係が参照しているストアは
`node_modules/.modules.yaml` の `storeDir` で確認できます。自動実行環境でそのストアにアクセスできない場合は、
権限と実行環境を直し、代替の `.pnpm-store/` をリポジトリ内に作成しないでください。

- `firmware/live_chat/`: 本体コード、I2S / AECドライバー、固定したArduinoボード定義。
- `firmware/live.py`: SOPSの設定をUSBで渡すツールとデバイストークンの初期化。
- `web/`: Next.js と認証付き `/api/live` WebSocket中継。
- `infra/`: Vercelプロジェクト・環境変数・FirewallのTerraform定義。
- `nix/`: 開発環境とpre-commitチェック。

## 初期設定

`.env.example` を参考に、`.enc.env` を SOPS で編集します。秘密値はGitやコマンド引数へ書きません。

```sh
sops edit --input-type dotenv --output-type dotenv .enc.env
just live-init
```

`live-init` は未設定の場合だけ `DEVICE_TOKEN` を生成し、SOPSで暗号化保存します。
既存の値は変更しません。必要な設定は以下です。

| 変数 | 用途 |
| --- | --- |
| `SSID`, `PASS` | 2.4 GHz Wi-Fiへの接続 |
| `DEVICE_TOKEN` | 本体とVercelで共有するデバイス認証情報 |
| `OPENAI_API_KEY` | VercelからOpenAIへ接続するAPIキー |
| `REALTIME_TOKEN_URL` | 接続先ホストの指定。既存形式 `https://your-project.vercel.app/api/realtime/token` を使用 |
| `VERCEL_API_TOKEN` | Terraform専用のVercel認証情報 |

Arduinoの依存バージョンは `firmware/live_chat/sketch.yaml` で固定します。
USBポートは `just boards` で確認します。ポートが現れない場合は、本体のリセットボタンを
長押ししてダウンロードモードに入れ、データ通信対応ケーブルで接続してください。
`.ino` のエディタ設定はプロジェクトの `.zed/settings.json` に置き、Arduino LSPを無効にしています。

## GPT-Live の連続会話

ボタンを1回押すと会話を開始し、もう1回押すと終了します。録音と再生を同時に行い、
スピーカーの音を Espressif AEC で除去してからマイク音声を送ります。
`gpt-live-1` が発話のタイミングと割り込みを扱い、知識・推論は `gpt-6-luna` に委ねます。
回答までの待ち時間を抑えるため、推論量は `reasoning.effort: "low"` に設定しています。

```text
本体 ─ WSS + DEVICE_TOKEN → Vercel /api/live ─ WSS + API キー → OpenAI GPT-Live
```

通常の API キーは Vercel 内でのみ使用します。Vercel は東京 `hnd1` で音声通信を中継します。
16 kHz / mono / PCM16 で双方向に通信し、レート変換は行いません。

### デプロイと実機確認

1. 変更を push して Vercel の **Production** にデプロイします。
2. `OPENAI_API_KEY` / `DEVICE_TOKEN` と Firewall ルールは [Terraform](../infra/README.md) で管理します。
   `realtime-token` という既存のルールIDで、接続開始を60秒あたり6回に制限します。
3. monitor を終了し、次を実行します。

```sh
just live-upload /dev/cu.usbmodem1101
just live-connect /dev/cu.usbmodem1101
just monitor /dev/cu.usbmodem1101
```

`live-connect` は `.enc.env` の既存 `REALTIME_TOKEN_URL` からホストを取り出し、
同じホストの `/api/live` を接続先にします。この変数名と旧パスは既存設定を維持するためのもので、
旧 `/api/realtime/token` への通信は行いません。Wi-Fi 接続・時刻同期に成功したら、SSID・パスワード・
接続先ホスト・`DEVICE_TOKEN` を本体の NVS にまとめて保存します。`本体への設定保存: OK` が完了の目印です。
初回と設定変更時だけ `live-connect` を実行してください。失敗した設定では以前の保存内容を上書きしません。

次回からは電源投入・リセット時に保存済み設定を読み、Wi-Fi 接続と時刻同期を行います。
テザリングがまだ有効でない場合は、Wi-Fi 接続を最大30秒待ち、失敗後30秒待って再試行します。
時刻同期は最大20秒待ちます。待機中のWi-Fi切断も再接続します。
接続中・再試行中も USB から `live-connect` で設定し直せます。
設定の送信や起動だけではマイクや OpenAI 接続を開始しません。
会話中の切断では会話を終了し、Wi-Fi 復旧後も会話の再開にはボタン操作が必要です。

起動時のログは `CONFIG_LOADED` → `CONNECTING` → `OK WIFI` → `OK TIME` → `LIVE_READY` です。
`WIFI_RETRY_WAIT` は再試行待ち、`LIVE_CONFIG_REQUIRED` は設定が必要な状態です。
`FAIL CONFIG_STORAGE` は保存領域の読み書きまたはデータ検証の失敗です。`live-connect` で再設定してください。
通常の書き込み（`EraseFlash=none`）では設定を保持しますが、フラッシュ全消去では失われます。

NVS はこのファームウェアでは暗号化していません。`.enc.env` の SOPS 暗号化とは別の保存領域であり、
本体のフラッシュを読み出せる人には Wi-Fi パスワードと `DEVICE_TOKEN` を取得される可能性があります。
通常の OpenAI API キーは本体へ送りません。
保存方式: [Espressif Preferences / NVS](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/preferences.html)。

### Unit Glass2 の状態表示

Atom／TailBATの電源を切り、MacのUSBも外してから、TailBATのGrove端子とUnit Glass2を付属ケーブルで接続します。
Glass2自体に電源スイッチはなく、Groveの5Vから給電されます。配線はSDA=GPIO2、SCL=GPIO1です。
再度USBを接続し、`just live-upload /dev/cu.usbmodem1101` で書き込んでください。保存済みWi-Fi設定は維持します。

| 画面 | 状態 |
| --- | --- |
| `SETUP` | USBから `live-connect` で初期設定が必要 |
| `WIFI...` / `TIME SYNC` | Wi-Fi接続／時刻同期中 |
| `WIFI WAIT` | 接続失敗後の再試行待ち |
| `READY` | ボタンで会話を開始できる状態 |
| `STARTING` | 音声処理とサーバー接続の準備中 |
| `LIVE` | 会話中。話しかけるか、ボタンで終了 |
| `STOPPING` | 会話の終了処理中 |
| `ERROR` | 下のログにエラーコードを表示 |

表示は欧文等幅の X11 Fixed 6×10 に統一し、状態・操作案内・空行・直近3行の主要イベントを配置します。
フォントは [U8g2配布のパブリックドメインデータ](https://github.com/olikraus/u8g2/blob/master/tools/font/build/single_font_files/u8g2_font_6x10_tr.c) を同梱しています。
日本語の会話本文、SSID、パスワード、トークンは表示しません。
細かな音声診断・トークン数は `just monitor` で確認できます。
エラーは終了後の `LIVE_READY` で消さず、次の接続・会話開始まで残します。

M5GFX 0.2.30の公式ドライバーを使用し、I²Cアドレスは0x3C、0x3Dの順で検出します。
USBログの `DISPLAY_READY 0x3C` または `0x3D` が認識成功、`DISPLAY_NOT_FOUND` は未検出です。
未接続でも音声機能は動作します。Glass2は起動時に検出するため、配線を変更したら本体を再起動してください。
コーデックのI²C0と表示用I²C1を分け、画面はネットワークより低い優先度の別タスクで、変更時のみ最大4回/秒更新します。

公式仕様: [Unit Glass2](https://docs.m5stack.com/en/unit/Glass2%20Unit)、
[Atom VoiceS3Rのピン配置](https://docs.m5stack.com/en/core/Atom_EchoS3R)、
[M5UnitGLASS2ドライバー](https://github.com/m5stack/M5GFX/blob/0.2.30/src/M5UnitGLASS2.h)。

### 会話の操作と確認

- ボタンを1回押し、`LIVE_LISTENING` を確認してから日本語で話しかけます。
- 回答中にも話しかけ、相手の発話へ切り替わるかを確認します。保持する再生音声と通信により遅延は残ります。
- 再度ボタンを押すと録音・再生を停止し、`LIVE_SESSION_CLOSED` → `LIVE_READY` で終了します。
  monitor の `c` + Enter でも終了できます。
- 接続開始から最大4分で自動終了します。Vercel Function は最大300秒に設定し、終了通知を待つ余裕を確保します。
  もう1回押すと新規会話になり、前の会話は引き継ぎません。会話セッションはエラー時にも自動再開しません。

**開始から終了まで API 利用料金が発生します。** `LIVE_USAGE_SECONDS` は最終通知の会話秒数、
`LIVE_BACKEND_TOKENS` は委任先モデルの応答ごとのトークン数です。両者は別に課金されます。
Vercel の中継にも Function の実行時間・転送量が発生します。
`LIVE_USAGE_UNCONFIRMED` は正常終了の最終通知が届かなかったことを表し、無料だったという意味ではありません。

音声・文字起こし・認証情報をアプリのログに保存しません。本体の接続設定は上記の NVS に保存します。`store: false` で
Live セッションの保存を無効にします（OpenAI のサービス側の保持方針とは別です）。
録音/再生キューには上限を設け、終了後に両タスクが停止してから消去します。

`LIVE_AUDIO` は終了時の診断値です。`rx_overflows=0` と、通常の32 ms処理枠を下回る
`max_aec_us` を確認します。`playback_gaps` は末尾や無音区間のキュー切れも数えるため、
それだけで音切れと断定せず実際の聞こえ方と合わせて判断します。
`gain_clipped` が多い場合は、AEC 後の入力ゲイン（現在8倍）の見直しが必要です。
`FAIL AUDIO_OVERRUN` / `MIC_BACKPRESSURE` / `PLAYBACK_BACKPRESSURE` は音声処理や通信が追いつかなかったため終了した状態です。

`LIVE_WS_HTTP` が404なら Live ルートがまだデプロイされていません。401は認証情報、429は接続回数、
503は環境変数・Production 環境・Firewallを確認します。101の後の `FAIL LIVE_API` / `RELAY_STOP` は
上流接続やセッション処理の失敗です。秘密情報や上流エラー本文は本体へ転送しません。

ローカルの `next dev` は Vercel の WebSocket upgrade を提供しません。公式ヘルパーの開発には
Vercel CLI 54.14.2以降の `vercel dev` が必要ですが、このプロジェクトの認証入口は
Firewall を必須としているため Production 以外を拒否します。ローカルでは中継の単体テストを使います。

```sh
pnpm test
pnpm typecheck
pnpm build
python3 -m unittest discover -s firmware -p 'test_*.py'
just live-build
```

回帰テストは認証・通信制限・切断と終了処理、USB経由の設定、設定保存・復元、音声キュー、Wi-Fi・時刻同期を対象にします。
実機では設定保存後の電源断・再起動、テザリングを後から有効にした場合の復旧、会話、割り込み、終了・再開を確認します。

公式仕様: [Live WebSocket](https://developers.openai.com/api/docs/guides/voice-websockets?api=live)、
[Live のプロンプト](https://developers.openai.com/api/docs/guides/live-prompting)、
[Vercel WebSocket](https://vercel.com/docs/functions/websockets)、
[Vercel upgrade API](https://vercel.com/docs/functions/functions-api-reference/vercel-functions-package#experimental_upgradewebsocket)。
