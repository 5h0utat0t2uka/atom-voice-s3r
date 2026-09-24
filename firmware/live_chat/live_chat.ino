#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <algorithm>
#include <cJSON.h>
#include <esp_aec.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <esp_transport_ssl.h>
#include <esp_transport_ws.h>
#include <mbedtls/platform_util.h>
#include "audio_hardware.h"
#include "pcm_queue.h"

// Network owns WebSocket on core 0; I2S/AEC stays on core 1 without network waits.
PcmQueue<4096> microphone;
PcmQueue<8192> playback;
std::atomic<bool> stopRequested{false}, audioReady{false}, streaming{false};
std::atomic<bool> audioDone{true}, networkDone{true};
std::atomic<const char *> failure{nullptr};
uint32_t audioBlocks = 0, maxAecUs = 0, playbackGaps = 0, gainClipped = 0;
char host[256] = {}, deviceToken[65] = {};
char input[1536] = {};
size_t inputLength = 0;
bool inputOverflow = false, configured = false, running = false;
uint32_t lastInputAt = 0;

void setFailure(const char *code) {
  const char *empty = nullptr;
  failure.compare_exchange_strong(empty, code);
  stopRequested = true;
}

const char *jsonString(cJSON *object, const char *key) {
  auto value = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(value) ? value->valuestring : "";
}

void audioTask(void *) {
  auto *aec = aec_create(rate, 4, 1, AEC_MODE_FD_HIGH_PERF);
  int16_t *mic = nullptr, *reference = nullptr, *clean = nullptr;
  size_t chunk = 0;
  if (!aec) setFailure("AEC_CREATE");
  if (aec) {
    aec_set_nlp_level(aec, AEC_NLP_LEVEL_NORMAL);
    const int frames = aec_get_chunksize(aec);
    if (frames != 512) setFailure("AEC_CHUNK");
    else {
      chunk = size_t(frames);
      mic = static_cast<int16_t *>(heap_caps_aligned_calloc(16, chunk, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      reference = static_cast<int16_t *>(heap_caps_aligned_calloc(16, chunk, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      clean = static_cast<int16_t *>(heap_caps_aligned_calloc(16, chunk, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      if (!mic || !reference || !clean) setFailure("AEC_MEMORY");
    }
  }
  if (!stopRequested && (!initAudio() || !initCodec())) setFailure("AUDIO_INIT");
  if (!stopRequested) {
    if (i2s_channel_enable(rx) != ESP_OK) setFailure("I2S_RX_ENABLE");
    else rxEnabled = true;
    if (!stopRequested) {
      if (i2s_channel_enable(tx) != ESP_OK) setFailure("I2S_TX_ENABLE");
      else txEnabled = true;
    }
  }
  if (!stopRequested) {
    digitalWrite(amplifierPin, HIGH);
    int16_t history[dmaBlocks][framesPerBlock] = {};
    int16_t output[framesPerBlock * 2] = {}, captured[framesPerBlock * 2];
    int16_t mono[framesPerBlock] = {};
    size_t filled = 0;
    bool playing = false;
    uint32_t queuedAt = 0;
    // 128 blocks = 1280 ms, exactly 40 AEC chunks, matching the verified test.
    for (size_t block = 0; !stopRequested.load(); ++block) {
      if (block == 128) audioReady = true;
      const size_t available = playback.available();
      if (available && !queuedAt) queuedAt = millis();
      if (!playing && available && (available >= rate / 10 || millis() - queuedAt >= 100)) playing = true;
      memset(mono, 0, sizeof(mono));
      if (playing) {
        const size_t count = playback.pop(mono, framesPerBlock);
        if (count < framesPerBlock) { playing = false; queuedAt = 0; ++playbackGaps; }
      }
      for (size_t i = 0; i < framesPerBlock; ++i) output[2 * i] = output[2 * i + 1] = mono[i];
      size_t written = 0, received = 0;
      const auto before = micros();
      if (i2s_channel_write(tx, output, sizeof(output), &written, 250) != ESP_OK || written != sizeof(output)
          || i2s_channel_read(rx, captured, sizeof(captured), &received, 250) != ESP_OK || received != sizeof(captured)) {
        setFailure("I2S_IO"); break;
      }
      for (size_t i = 0; i < framesPerBlock; ++i) {
        mic[filled] = captured[2 * i];
        reference[filled] = history[block % dmaBlocks][i];
        history[block % dmaBlocks][i] = mono[i];
        if (++filled == chunk) {
          const auto started = micros();
          aec_process(aec, mic, reference, clean);
          maxAecUs = std::max(maxAecUs, uint32_t(micros() - started));
          // Same +18 dB gain as the successful listening comparison, applied
          // after cancellation (not to the playback reference or analog ADC).
          if (streaming.load()) {
            for (size_t j = 0; j < chunk; ++j) {
              const int value = int(clean[j]) * 8;
              if (value > 32767 || value < -32768) ++gainClipped;
              clean[j] = std::max(-32768, std::min(32767, value));
            }
            if (!microphone.push(clean, chunk)) setFailure("MIC_BACKPRESSURE");
          }
          filled = 0;
        }
      }
      ++audioBlocks;
      uint32_t overflow;
      portENTER_CRITICAL(&counterMux);
      overflow = rxOverflows;
      portEXIT_CRITICAL(&counterMux);
      // A missed DMA timeline invalidates the software AEC reference. Stop
      // explicitly instead of continuing with misaligned audio.
      if (overflow || micros() - before >= dmaBlocks * 10000) setFailure("AUDIO_OVERRUN");
    }
    mbedtls_platform_zeroize(captured, sizeof(captured));
    mbedtls_platform_zeroize(history, sizeof(history));
  }
  stopAudio();
  if (aec) aec_destroy(aec);
  for (auto *buffer : {mic, reference, clean}) {
    if (buffer) { mbedtls_platform_zeroize(buffer, chunk * 2); free(buffer); }
  }
  audioDone = true;
  vTaskDelete(nullptr);
}

bool sendFrame(esp_transport_handle_t socket, ws_transport_opcodes_t opcode, void *data, size_t size) {
  // ESP-IDF masks the writable payload in place; callers pass disposable copies.
  return esp_transport_ws_send_raw(socket, static_cast<ws_transport_opcodes_t>(opcode | WS_TRANSPORT_OPCODES_FIN),
                                  static_cast<char *>(data), size, 1000) == int(size);
}

void networkTask(void *) {
  while (!audioReady && !stopRequested) delay(1);
  esp_transport_handle_t tls = nullptr, socket = nullptr;
  if (!stopRequested) {
    tls = esp_transport_ssl_init();
    if (tls) socket = esp_transport_ws_init(tls);
    if (!tls || !socket) setFailure("WS_MEMORY");
  }
  if (!stopRequested) {
    esp_transport_ssl_crt_bundle_attach(tls, esp_crt_bundle_attach);
    char authorization[80];
    snprintf(authorization, sizeof(authorization), "Bearer %s", deviceToken);
    esp_transport_ws_config_t config = {};
    config.ws_path = "/api/live";
    config.auth = authorization;
    config.propagate_control_frames = false;
    const bool setupOk = esp_transport_ws_set_config(socket, &config) == ESP_OK;
    mbedtls_platform_zeroize(authorization, sizeof(authorization));
    const int result = setupOk ? esp_transport_connect(socket, host, 443, 15000) : -1;
    Serial.printf("LIVE_WS_HTTP %d\n", esp_transport_ws_get_upgrade_request_status(socket));
    esp_transport_ws_set_auth(socket, nullptr);
    if (result < 0) setFailure("WS_CONNECT");
    else {
      bool ready = false, closing = false, finalized = false;
      uint32_t closedAt = 0, connectedAt = millis(), pingAt = connectedAt;
      size_t length = 0, frameRead = 0;
      bool fragmented = false;
      int messageOpcode = 0;
      alignas(4) char message[2049] = {};
      while (true) {
        if (WiFi.status() != WL_CONNECTED) { setFailure("WIFI_LOST"); break; }
        if (!closing && (stopRequested || millis() - connectedAt > 240000)) {
          stopRequested = true;
          streaming = false;
          char close[] = "{\"type\":\"close\"}";
          if (!sendFrame(socket, WS_TRANSPORT_OPCODES_TEXT, close, sizeof(close) - 1)) { setFailure("WS_SEND"); break; }
          closing = true; closedAt = millis();
          Serial.println("LIVE_STOPPING");
        }
        if (closing && millis() - closedAt > 17000) { setFailure("CLOSE_TIMEOUT"); break; }
        if (!ready && !closing && millis() - connectedAt > 20000) { setFailure("SESSION_TIMEOUT"); break; }
        if (ready && !closing && microphone.available() >= 512) {
          int16_t pcm[512];
          microphone.pop(pcm, 512);
          const bool sent = sendFrame(socket, WS_TRANSPORT_OPCODES_BINARY, pcm, sizeof(pcm));
          mbedtls_platform_zeroize(pcm, sizeof(pcm));
          if (!sent) { setFailure("WS_SEND"); break; }
        }
        if (millis() - pingAt >= 20000) {
          char ping = 0;
          if (!sendFrame(socket, WS_TRANSPORT_OPCODES_PING, &ping, 1)) { setFailure("WS_PING"); break; }
          pingAt = millis();
        }
        // Before ready, drain any data buffered alongside the HTTP upgrade.
        if (ready) {
          const int pending = esp_transport_poll_read(socket, 1);
          if (pending < 0) { setFailure("WS_READ"); break; }
          if (!pending) { delay(1); continue; }
        }
        char part[1024];
        const int count = esp_transport_read(socket, part, sizeof(part), 1000);
        if (count < 0) { setFailure("WS_READ"); break; }
        if (!count) {
          if (frameRead) { setFailure("WS_FRAME_TIMEOUT"); break; }
          delay(1); continue;
        }
        const int opcode = esp_transport_ws_get_read_opcode(socket);
        const int frameSize = esp_transport_ws_get_read_payload_len(socket);
        if (frameSize < 0 || frameSize > 2048 || length + count > 2048) { setFailure("WS_SIZE"); break; }
        if (!frameRead) {
          if (!fragmented && (opcode == WS_TRANSPORT_OPCODES_TEXT || opcode == WS_TRANSPORT_OPCODES_BINARY)) {
            fragmented = true; messageOpcode = opcode;
          } else if (!fragmented || opcode != WS_TRANSPORT_OPCODES_CONT) { setFailure("WS_FRAME"); break; }
        }
        memcpy(message + length, part, count);
        length += count; frameRead += count;
        if (frameRead < size_t(frameSize)) continue;
        if (frameRead != size_t(frameSize)) { setFailure("WS_FRAME"); break; }
        frameRead = 0;
        if (!esp_transport_ws_get_fin_flag(socket)) continue;
        fragmented = false;
        if (messageOpcode == WS_TRANSPORT_OPCODES_BINARY) {
          if (!ready || length % 2) { setFailure("AUDIO_FORMAT"); break; }
          if (!closing && !playback.push(reinterpret_cast<int16_t *>(message), length / 2)) {
            setFailure("PLAYBACK_BACKPRESSURE");
          }
        } else {
          message[length] = 0;
          auto *event = cJSON_ParseWithOpts(message, nullptr, true);
          const char *type = jsonString(event, "type");
          if (!strcmp(type, "ready")) {
            auto format = cJSON_GetObjectItemCaseSensitive(event, "rate");
            if (ready || !cJSON_IsNumber(format) || format->valueint != rate) setFailure("SESSION_FORMAT");
            else {
              ready = true;
              if (!closing) { streaming = true; Serial.println("LIVE_LISTENING"); }
            }
          } else if (!strcmp(type, "stopping")) {
            stopRequested = true; streaming = false;
            if (!closing) { closing = true; closedAt = millis(); Serial.println("LIVE_STOPPING"); }
            // Only print fixed protocol values, never arbitrary server text.
            if (strcmp(jsonString(event, "reason"), "button") && strcmp(jsonString(event, "reason"), "time_limit")) {
              setFailure("RELAY_STOP");
            }
          } else if (!strcmp(type, "closed")) {
            finalized = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(event, "finalized"));
            auto seconds = cJSON_GetObjectItemCaseSensitive(event, "seconds");
            if (finalized && cJSON_IsNumber(seconds) && seconds->valuedouble >= 0) {
              Serial.printf("LIVE_USAGE_SECONDS %.3f\n", seconds->valuedouble);
            }
            cJSON_Delete(event);
            break;
          } else if (!strcmp(type, "backend_usage")) {
            auto in = cJSON_GetObjectItemCaseSensitive(event, "input_tokens");
            auto out = cJSON_GetObjectItemCaseSensitive(event, "output_tokens");
            auto cached = cJSON_GetObjectItemCaseSensitive(event, "cached_tokens");
            Serial.printf("LIVE_BACKEND_TOKENS input=%.0f output=%.0f cached=%.0f\n",
                cJSON_IsNumber(in) ? in->valuedouble : -1, cJSON_IsNumber(out) ? out->valuedouble : -1,
                cJSON_IsNumber(cached) ? cached->valuedouble : -1);
          } else if (!strcmp(type, "error")) setFailure("LIVE_API");
          else setFailure("RELAY_PROTOCOL");
          cJSON_Delete(event);
        }
        mbedtls_platform_zeroize(message, sizeof(message));
        length = 0;
      }
      Serial.println(finalized ? "LIVE_SESSION_CLOSED" : "LIVE_USAGE_UNCONFIRMED");
      mbedtls_platform_zeroize(message, sizeof(message));
    }
  }
  streaming = false;
  stopRequested = true;
  if (socket) { esp_transport_close(socket); esp_transport_destroy(socket); }
  if (tls) esp_transport_destroy(tls);
  networkDone = true;
  vTaskDelete(nullptr);
}

void configure() {
  configured = false;
  auto *doc = cJSON_ParseWithOpts(input, nullptr, true);
  const char *ssid = jsonString(doc, "ssid"), *password = jsonString(doc, "password");
  const char *server = jsonString(doc, "live_host"), *token = jsonString(doc, "device_token");
  bool valid = strlen(ssid) >= 1 && strlen(ssid) <= 32 && strlen(password) >= 8 && strlen(password) <= 63
      && strlen(server) >= 1 && strlen(server) < sizeof(host) && strlen(token) == 64;
  for (const char *p = server; valid && *p; ++p) valid = isalnum(static_cast<unsigned char>(*p)) || *p == '.' || *p == '-';
  for (const char *p = token; valid && *p; ++p) valid = (*p >= 'a' && *p <= 'f') || (*p >= '0' && *p <= '9');
  mbedtls_platform_zeroize(deviceToken, sizeof(deviceToken));
  if (valid) {
    strcpy(host, server); strcpy(deviceToken, token);
    WiFi.disconnect(); WiFi.begin(ssid, password);
  }
  for (const char *key : {"ssid", "password", "device_token"}) {
    auto value = cJSON_GetObjectItemCaseSensitive(doc, key);
    if (cJSON_IsString(value)) mbedtls_platform_zeroize(value->valuestring, strlen(value->valuestring));
  }
  cJSON_Delete(doc);
  mbedtls_platform_zeroize(input, sizeof(input));
  if (!valid) { Serial.println("FAIL CONFIG"); return; }
  Serial.println("CONNECTING");
  const auto started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 30000) delay(10);
  if (WiFi.status() != WL_CONNECTED) { Serial.println("FAIL WIFI"); return; }
  Serial.println("OK WIFI");
  esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTime(0, 0, "time.apple.com", "pool.ntp.org");
  const auto syncAt = millis();
  while (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - syncAt < 20000) delay(10);
  if (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) { Serial.println("FAIL TIME"); return; }
  Serial.println("OK TIME");
  configured = true;
  Serial.println("LIVE_READY");
}

void startSession() {
  if (!configured) { Serial.println("LIVE_CONFIG_REQUIRED"); return; }
  if (WiFi.status() != WL_CONNECTED) { Serial.println("FAIL WIFI_LOST"); return; }
  microphone.reset(); playback.reset();
  failure = nullptr;
  stopRequested = streaming = audioReady = false;
  audioBlocks = maxAecUs = playbackGaps = gainClipped = 0;
  rxOverflows = 0;
  running = true; audioDone = networkDone = false;
  Serial.println("LIVE_STARTING");
  if (xTaskCreatePinnedToCore(audioTask, "live-aec", 12288, nullptr, 4, nullptr, 1) != pdPASS) {
    setFailure("AUDIO_TASK"); audioDone = networkDone = true; return;
  }
  if (xTaskCreatePinnedToCore(networkTask, "live-ws", 16384, nullptr, 2, nullptr, 0) != pdPASS) {
    setFailure("NETWORK_TASK"); networkDone = true;
  }
}

void setup() {
  pinMode(amplifierPin, OUTPUT); digitalWrite(amplifierPin, LOW);
  pinMode(buttonPin, INPUT_PULLUP);
  Serial.begin(115200); Serial.setTxTimeoutMs(100);
  esp_log_level_set("transport_ws", ESP_LOG_NONE);
  esp_log_level_set("transport_base", ESP_LOG_NONE);
  WiFi.persistent(false); WiFi.mode(WIFI_STA); WiFi.setSleep(false);
  microphone.samples = static_cast<int16_t *>(heap_caps_calloc(4096, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  playback.samples = static_cast<int16_t *>(heap_caps_calloc(8192, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void loop() {
  static bool connectedBefore = false, buttonBefore = false, buttonRaw = false;
  static uint32_t changedAt = 0;
  const bool connected = static_cast<bool>(Serial);
  if (connected && !connectedBefore) Serial.println("AVS3R_LIVE_READY 1");
  connectedBefore = connected;
  const bool pressed = digitalRead(buttonPin) == LOW;
  if (pressed != buttonRaw) { buttonRaw = pressed; changedAt = millis(); }
  if (pressed != buttonBefore && millis() - changedAt >= 30) {
    buttonBefore = pressed;
    if (pressed) {
      if (running) { streaming = false; stopRequested = true; }
      else if (!microphone.samples || !playback.samples) Serial.println("FAIL MEMORY");
      else startSession();
    }
  }
  while (Serial.available()) {
    const char ch = Serial.read();
    lastInputAt = millis();
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (!inputOverflow && inputLength) {
        input[inputLength] = 0;
        if (!strcmp(input, "?")) Serial.println("AVS3R_LIVE_READY 1");
        else if (!strcmp(input, "c")) { streaming = false; stopRequested = true; }
        else if (running) Serial.println("FAIL BUSY");
        else if (!microphone.samples || !playback.samples) Serial.println("FAIL MEMORY");
        else configure();
      }
      mbedtls_platform_zeroize(input, sizeof(input)); inputLength = 0; inputOverflow = false;
    } else if (inputLength < sizeof(input) - 1 && !inputOverflow) input[inputLength++] = ch;
    else inputOverflow = true;
  }
  if ((inputLength || inputOverflow) && millis() - lastInputAt > 5000) {
    mbedtls_platform_zeroize(input, sizeof(input)); inputLength = 0; inputOverflow = false;
    Serial.println("FAIL CONFIG_TIMEOUT");
  }
  if (running && audioDone && networkDone) {
    Serial.printf("LIVE_AUDIO blocks=%lu max_aec_us=%lu playback_gaps=%lu gain_clipped=%lu rx_overflows=%lu\n",
                  static_cast<unsigned long>(audioBlocks), static_cast<unsigned long>(maxAecUs),
                  static_cast<unsigned long>(playbackGaps), static_cast<unsigned long>(gainClipped),
                  static_cast<unsigned long>(rxOverflows));
    if (failure.load()) Serial.printf("FAIL %s\n", failure.load());
    microphone.reset(); playback.reset(); running = false;
    Serial.println("LIVE_READY");
  }
  delay(1);
}
