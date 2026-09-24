# Atom VoiceS3R 動作確認

## Realtime のボタン録音・音声応答

`just realtime-upload` で使うスケッチは `realtime_playback/realtime_playback.ino` です。
前面ボタンを押している間に録音・送信し、離すと回答を生成して受信中から再生します。
接続と会話履歴を維持し、待機中の `r` + Enter で再接続・履歴リセットができます。
書き込み・接続設定・確認ログは [プロジェクト README](../README.md#realtime-のボタン録音音声応答) を参照してください。
以下の録音単体・固定音声・Wi-Fi の各テストは、切り分け用の独立したスケッチです。

## Zed での編集

リポジトリのルートを Zed で開き、そのまま `.ino` を編集します。
専用ウィンドウや起動コマンドは不要です。

ルートの `.zed/settings.json` で `.ino` を Arduino として認識させ、
Arduino の LSP を無効にしています。Arduino 拡張による構文ハイライトは利用できますが、
LSP による補完・ホバー・定義参照・診断は行いません。
コンパイルエラーは `just build` で確認します。他言語の LSP 設定には影響しません。
グローバルな Zed 設定の変更は不要です。

設定の根拠: [Zed の言語別 LSP 無効化](https://zed.dev/docs/configuring-languages#enabling-or-disabling-language-servers)。

## ビルドと書き込み

`sketch.yaml` がボードパッケージとライブラリを固定し、初回ビルド時に Arduino CLI が不足する依存物を自動取得
```sh
just build
just boards
just upload /dev/cu.usbmodem1101
just monitor /dev/cu.usbmodem1101
```

- ポート名は `just boards` の結果に合わせる
- 書き込み用ポートが表示されない場合は、USB 接続したまま側面のリセットボタンを2秒から3秒ほど長押しし、緑色 LED が点灯・点滅したら離してダウンロードモードにする
- 書き込み後に起動しない場合は、モニターを終了して USB を抜き、ボタンを押さずに再接続する
- 再起動後にポート名が変わる場合は `just boards` で確認する

## AI が生成した日本語音声の再生テスト

録音ノイズの調査は保留し、マイクを起動しない再生専用スケッチで確認します。
Mac で OpenAI Speech API に短い固定文を送り、生成音声をファームウェアに含めて
USB で書き込みます。通常の録音テストとは別のスケッチです。

`.enc.env` に SOPS で暗号化した `OPENAI_API_KEY` を設定し、プロジェクトの
Nix 環境で次を実行します。キーは `sops exec-env` で生成処理に渡し、平文の
環境ファイルやファームウェアには保存しません。生成には API 利用料金が発生します。

```sh
just speech-generate
afplay build/speech/speech.wav
just boards
# just monitor は終了してから書き込みます。
just speech-upload /dev/cu.usbmodem1101
just monitor /dev/cu.usbmodem1101
```

書き込み後、必要なら USB を再接続し、前面ボタンを押すと日本語音声を再生します。
再生終了後に再度押せば、API を呼ばずに同じ音声を聞けます。
モニターから `p` を送っても再生できます。再生中の押下・要求は受け付けません。

- モデル: `gpt-4o-mini-tts`、声: `marin`。
- 文: 「こんにちは。これはAIが生成した日本語の音声です。」
- 音声: 24 kHz / mono / signed PCM16 little-endian、最大10秒。
- PCM の音量補正・フィルター・サンプルレート変換は行いません。
  スピーカー音量は録音テストと同じ `64` です。
- 本体でのみ冒頭が欠ける現象への対策として、起動直後に200 msの無音 PCM を流し、
  同じ再生チャンネルに元の音声を続けて送ります。単なる待ち時間ではなく、
  I²S のクロックを動かしてから発話を始めます。元の PCM・WAV は変更しません。
  200 ms は実機で効果を確認するための初期値であり、メーカー保証の起動時間ではありません。
- `build/speech/` に PCM・WAV・生成条件・ビルド用スケッチを保存します（Git 管理対象外）。
  ボード設定と依存ライブラリは既存の `sketch.yaml` からコピーして揃えます。
- `just speech-generate` は保存済み PCM があれば再生成しません。
  再生成するときは `sops exec-env .enc.env 'python3 firmware/speech.py generate --force'` を実行します。
- 本体には音声を含むファームウェアが Flash に残ります。録音テストに戻すには
  通常の `just upload <port>` を使います。

Mac の WAV と本体で、声の明瞭さ・速度・高さ・音割れを聞き比べてください。
このテストは保存音声の再生品質を確認するものです。Realtime API の通信や
音声チャンクを受信しながらの再生は、別の段階で検証します。

根拠: [OpenAI Text to speech](https://developers.openai.com/api/docs/guides/text-to-speech)。

## Wi-Fi / HTTPS 疎通テスト

`.enc.env` に `SSID` と `PASS` を SOPS で設定します。`SSID` は UTF-8 で1〜32バイト、
`PASS` は8〜63文字の ASCII 文字です。空白を含む値もそのまま使います。
iPhone のテザリングを使う場合は「インターネット共有（Personal Hotspot）」で
「ほかの人の接続を許可（Allow Others to Join）」をオンにし、接続まで画面を開いておきます。
接続できず「互換性を優先（Maximize Compatibility）」が表示される場合はオンにします。
Apple はこの設定の対象を iPhone 12 以降としています。既に接続できている場合は変更不要です。

```sh
# monitor / capture を終了してから実行
just wifi-upload /dev/cu.usbmodem1101
just wifi-check /dev/cu.usbmodem1101
```

`wifi-upload` は接続情報を含まない専用スケッチを書き込みます。
`wifi-check` は `sops exec-env` で `SSID` / `PASS` を読み、専用スケッチの応答を
確認してから USB 経由で送信します。`OPENAI_API_KEY` は送信しません。
接続情報を生成ファイル・ファームウェア・ログ・本体の不揮発メモリには保存せず、
本体の RAM に保持します。再起動したら `wifi-check` を再実行してください。
これは接続検証用であり、複数 Wi-Fi の保存・自動再接続・設定画面はまだ実装していません。

テストは Wi-Fi 接続・IP アドレス取得、`www.espressif.com` の名前解決、
NTP による時刻同期、`https://www.espressif.com/` への HEAD リクエストの順に進みます。
HTTPS はボードパッケージの CA バンドルを使い、サーバー証明書・ホスト名を検証します。
リダイレクトは追わず、HTTP 200〜399 を到達成功とします。
OpenAI API の呼び出しは行わず、API 認証・WebSocket 接続の確認は別の段階で行います。

- `FAIL WIFI`: SSID / PASS、iPhone の共有設定、2.4 GHz の到達性を確認。
  直前の `DISCONNECT_REASON` は ESP-IDF の切断理由コード（201: AP が見つからない、
  202: 認証失敗、204: ハンドシェイクのタイムアウト）。0 は理由未取得です。
- `FAIL DNS` / `FAIL TIME`: モバイル回線の状態、DNS / NTP の利用可否を確認。
- `FAIL HTTPS` / `FAIL HTTP_STATUS`: TLS 接続または HTTP 応答の問題。検証を無効化せず調査。
- `WIFI_CHECK_DONE` 相当の「疎通テスト完了」が表示されれば成功。

音声再生テストに戻すときは `just speech-upload <port>` を使います。

根拠: [Arduino ESP32 Wi-Fi API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html)、
[ESP HTTP Client](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/protocols/esp_http_client.html)、
[Apple のインターネット共有手順](https://support.apple.com/ja-jp/119837)。

## 録音テストの動作

- シリアルモニター接続時に Flash / PSRAM 容量と音声形式を表示
- `Ready` の表示後、前面ボタンを押し続けて話し、離すと録音した音声を再生
- 再生後はマイクを再準備し、次の `Ready` から新しい押下を受け付け

- 音声形式：24 kHz、モノラル、符号付き PCM16
  I²S 入力のチャンネル設定は M5Unified のボード初期値を維持し、`record(..., false)` でモノラルの録音データを受け取る
- ES8311 の ADC 送信ワード長も16ビットに設定し、M5Unified 0.2.23 のVoiceS3R 初期化はレジスタ `0x0A` を設定しないため、`Mic.begin()` 後にビット `[4:2]` を `011` に更新し、読み戻して一致を確認します。他のビットは維持
- `ES8311 ADC format: 0x00 -> 0x0C` なら24ビットの初期値から変更されたことを示し、ソフトリセットや再録音では、既に設定済みの `0x0C -> 0x0C` になる場合もある
  読み書き・一致確認に失敗した場合は停止します。
- 最大録音時間：10 秒。上限に達すると自動で録音終了・再生します。
- 150 ms 未満の押下は破棄します。準備中・再生中の押下は受け付けません。
- PSRAM に 480,000 バイトを確保し、再生終了・エラー時に消去します。
  Flash への保存とネットワーク送信は行いません。
- ES8311 起動直後の無音を避けるため、マイク開始後は 1.2 秒以上準備します。
  待機中もマイクから取得しますが、20 ms の一時バッファに上書きし、録音には含めません。
- 押下後は進行中の待機チャンクを完了してから録音するため、通常は最大約 20 ms と
  ループ処理分の開始遅延があります。解放時は進行中の録音を完了させるため、
  通常は最大約 40 ms 分の末尾音声を含みます。実際の遅延は実機で確認してください。
- 初期化失敗や音声処理のタイムアウトは `ERROR` として表示し、音声処理を停止します。

## 実機確認
1. `Ready` の後、ボタンを押しながら 2〜3 秒話し、離すと自分の声が再生されること。
2. 語頭・語尾の欠落、音の速度や高さ、音割れを確認すること。
3. 録音・再生を繰り返し、毎回 `Ready` に戻ること。
4. 短い押下が破棄されること、10 秒の上限で停止すること。
5. 再生中に押しても割り込まず、押し続けても勝手に次の録音が始まらないこと。

再生前に `PCM: peak=..., at_capture_limit=...` を表示します。
`at_capture_limit` は M5Unified の PCM16 出力の飽和値に達したサンプル数と割合です。
割合が高い場合は録音側のクリッピングが疑われます。0% でもコーデック内部の
歪みやスピーカー側の音割れまで否定できません。音割れがある場合は、この行と
起動時の `Mic: ...` の設定値を確認してください。

### ノイズの切り分け
`just monitor <port>` で `Ready` を待ち、ボタンを押さずに `t` を入力して Enter を押します。
マイクを停止し、生成した 1 kHz の正弦波を 2 秒間再生します。録音音声と同じ
24 kHz / PCM16 / モノラルのバッファ、`playRaw`、音量設定を使用します。
振幅は今回の録音ログに近い 5000 にし、両端を 20 ms でフェードさせています。
音量が小さく、判定できない場合はその旨を記録してください。

- テスト音にも持続的なザラザラ・バリバリがある場合：マイクを通らないため、
  再生処理・コーデックの出力設定・スピーカー側を優先して調べます。
- テスト音が明瞭で録音再生だけが歪む場合：録音データを取得して、入力側を調べます。
  このテストだけで再生側の全条件を検証できるわけではありません。

準備中・録音中・再生中の `t` は受け付けません。終了後は通常の `Ready` に戻ります。
これは原因を分けるための診断機能で、ノイズの修正完了を意味しません。

### 録音データの保存と確認

実機ではテスト音は明瞭で、ADC の送信ビット幅を16ビットに揃えても録音音声の
ノイズはほぼ改善しませんでした。ビット幅だけを原因とは判断できません。
録音ノイズの調査は保留中です。以下は調査を再開する場合の手順で、現在の Realtime 対話には不要です。

```sh
just upload /dev/cu.usbmodem1101
# monitor は終了してから実行。Python 3 標準ライブラリのみを使用。
just capture /dev/cu.usbmodem1101
```

`EXPORT ARMED` と案内が表示されたら、前面ボタンを押し続け、最初の約1秒は
静かにし、その後2〜3秒話してから離してください。再生に使うモノラル PCM と
診断ログを `build/diagnostics/capture-*/capture.wav` / `capture.txt` に保存します。
Mac で WAV を再生し、本体で聞いたノイズが含まれるか確認してください。
音量の自動補正・フィルター・サンプルレート変換は行いません。

- 保存は `just capture` で要求した次の録音1回だけです。通常の録音では送信しません。
- USB 経由でローカルに保存し、ネットワーク送信しません。保存先は Git 管理対象外で、
  ディレクトリは所有者のみアクセス可、ファイルは所有者のみ読み書き可です。
- 転送はバイト数とチェックサムで検証し、不完全なデータを WAV として保存しません。
- 要求は60秒で解除され、Ctrl+C でも解除を試みます。通常の再生終了後に本体の録音を消去します。
- `Capture: wall_ms` は最初の録音要求から最後の完了確認までの実測時間、
  `pcm_ms` はサンプル数と指定レートから計算した時間です。DMA の先行蓄積や
  ループの遅延があるので厳密には一致しませんが、大きな比率の違いを調べられます。
- `queue_empty` は途中の追加要求前に録音キューが空と観測された回数です。
  0でもすべての DMA オーバーラン・取りこぼしを否定できません。
- ES8311 のレジスタ値と PCM の最小値・最大値・RMS もログに記録します。

収集スクリプトの検証: `python3 -m unittest discover -s firmware -p 'test_*.py'`

### DMA 周期との関係を確認する比較

2026-09-24 の `capture-fy6vu6ou`（TV）と `capture-smxu8e29`（本人）の解析では、
冒頭の静かな区間に約187.9 Hzとその高調波がありました。実測時間はそれぞれ
9999 / 4463 ms、PCM の時間は10000 / 4460 ms、`queue_empty` は両方0です。
大幅なレート不一致や20 msチャンク境界に集中する段差は確認できていません。
上記の録音ファイル・解析結果は不要ファイルの整理で削除済みです。再検証する場合は新しく録音してください。

DMA の既定長128フレームでは完了頻度が `24000 / 128 = 187.5 Hz` となるため、
調査を再開する際はノイズとの関係を比較できます。これは周波数の近さに基づく仮説で、DMA の不具合や
電源経由の干渉と確定したわけではありません。

```sh
just upload /dev/cu.usbmodem1101
just capture /dev/cu.usbmodem1101 128
# 本体の再生が終了してから、同じ環境・距離・声で再度録音
just capture /dev/cu.usbmodem1101 256
```

両方とも案内後にボタンを押し、約2秒静かにしてから2〜3秒話します。
DMA 長以外の音声形式・ゲイン・フィルター・20 ms録音チャンクは同じです。
収集スクリプトは DMA の設定完了とマイクの準備完了を待ってから録音保存を要求します。
選択した DMA 長は再起動まで保持し、通常の `just capture <port>` では128に戻します。
この変更は比較実験であり、ノイズ対策として256を採用するものではありません。

通常は `just upload` を使用します。`just upload-erase <port>` は
フラッシュ内の保存データも全消去してから書き込む明示的な操作です。
ビルド出力は Git 管理対象外の `build/firmware/` に保存します。

## Ref
- [M5Stack: Atom VoiceS3R のビルド・書き込み](https://docs.m5stack.com/en/arduino/atom_echos3r/program)
- [M5Stack: ボードマネージャー](https://docs.m5stack.com/en/arduino/arduino_board)
- [Arduino CLI: ビルドプロファイル](https://docs.arduino.cc/arduino-cli/sketch-project-file/)
- [M5Stack: マイクと録音・再生の切り替え](https://docs.m5stack.com/en/arduino/atom_echos3r/mic)
- [M5Stack: スピーカーと PCM 再生](https://docs.m5stack.com/en/arduino/atom_echos3r/speaker)
- [Everest Semiconductor: ES8311 User Guide、p.12 の ADC 出力ワード長（Waveshare 配布）](https://files.waveshare.com/wiki/common/ES8311.user.Guide.pdf)
