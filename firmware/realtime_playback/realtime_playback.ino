#include <M5Unified.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <esp_transport_ssl.h>
#include <esp_transport_ws.h>
#include <mbedtls/base64.h>
#include <mbedtls/platform_util.h>

constexpr size_t sampleRate = 24000;
constexpr size_t startupBytes = sampleRate * 2 * 200 / 1000;
constexpr size_t audioCapacity = startupBytes * 2 + sampleRate * 2 * 20;
constexpr size_t messageCapacity = 128 * 1024;
constexpr size_t captureChunkSamples = sampleRate / 50; // 20 ms
constexpr size_t captureMaxSamples = sampleRate * 10;
constexpr size_t captureMinSamples = sampleRate * 150 / 1000;
constexpr uint32_t micWarmupMs = 1200;
constexpr uint32_t captureTimeoutMs = 1000;
enum class CaptureState { Off, Warming, Ready, Starting, Recording, Draining, Captured };
CaptureState captureState = CaptureState::Off;
int16_t* recording = nullptr;
int16_t discard[captureChunkSamples] = {};
size_t recordedSamples = 0; // Includes queued samples until Draining completes.
uint32_t captureStateAt = 0;
uint32_t captureProgressAt = 0;
uint32_t pressedAt = 0;
bool shortPress = false;
uint8_t* audio = nullptr;
char* message = nullptr;
size_t audioBytes = startupBytes;
char input[1536] = {};
size_t inputLength = 0;
bool inputOverflow = false;
uint32_t lastInputAt = 0;
char tokenUrl[512] = {};
char deviceToken[65] = {};
bool ready = false;

// The network task owns TLS/WebSocket. Only loop() touches M5 audio devices.
// Buffers are append-only within a turn; release/acquire publishes completed PCM.
enum class NetworkState { Off, Connecting, Ready, Busy, Done, Failed };
enum class Command { None, Voice, Text, Release };
std::atomic<NetworkState> networkState{NetworkState::Off};
std::atomic<Command> command{Command::None};
std::atomic<bool> networkEnabled{false};
std::atomic<bool> abortTurn{false};
std::atomic<size_t> capturedSamples{0};
std::atomic<size_t> availableAudio{startupBytes};
std::atomic<int> inputFinished{0}; // 0=recording, 1=release/commit, 2=discard
std::atomic<uint32_t> releasedAt{0};
bool workerCreated = false;
bool turnActive = false;
bool voiceTurn = true;
bool speakerStarted = false;
bool underrun = false;
size_t queuedAudio = 0;
uint32_t playbackAt = 0;
uint32_t underruns = 0;

bool stopNetwork() {
  networkEnabled = false;
  abortTurn = true;
  const auto started = millis();
  while (networkState.load() != NetworkState::Off) {
    if (millis() - started > 35000) return fail("NETWORK_STOP_TIMEOUT");
    delay(1);
  }
  return true;
}

const char* jsonString(cJSON* object, const char* key) {
  auto item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) ? item->valuestring : "";
}

bool fail(const char* code) {
  Serial.printf("FAIL %s\n", code);
  return false;
}

void clearRecording() {
  if (recording) mbedtls_platform_zeroize(recording, captureMaxSamples * sizeof(int16_t));
  mbedtls_platform_zeroize(discard, sizeof(discard));
  recordedSamples = 0;
}

void captureFailure(const char* code) {
  // end() stops the writer before its buffers are cleared.
  M5.Mic.end();
  captureState = CaptureState::Off;
  // A network send may still reference recording. Clear only after worker exits.
  if (turnActive) abortTurn = true;
  else clearRecording();
  fail(code);
}

bool prepareMic() {
  M5.Speaker.end();
  M5.Mic.end();
  captureState = CaptureState::Off;
  clearRecording();
  auto config = M5.Mic.config();
  config.sample_rate = sampleRate;
  config.dma_buf_len = 128;
  // Keep the board's stereo I2S input; record(..., false) produces mono PCM.
  M5.Mic.config(config);
  if (!M5.Mic.begin()) {
    captureFailure("MIC_INIT");
    return false;
  }
  // Match the existing recording test: ES8311 ADC output word length = 16 bits.
  m5gfx::i2c::i2c_temporary_switcher_t bus(1, GPIO_NUM_45, GPIO_NUM_0);
  uint8_t before = 0, after = 0;
  bool ok = M5.In_I2C.readRegister(0x18, 0x0A, &before, 1, 100000);
  const uint8_t desired = (before & ~0x1C) | 0x0C;
  if (ok) ok = M5.In_I2C.writeRegister8(0x18, 0x0A, desired, 100000)
               && M5.In_I2C.readRegister(0x18, 0x0A, &after, 1, 100000) && after == desired;
  bus.restore();
  if (!ok) {
    captureFailure("MIC_FORMAT");
    return false;
  }
  captureState = CaptureState::Warming;
  captureStateAt = captureProgressAt = millis();
  Serial.println("MIC_WARMING");
  return true;
}

bool validTokenUrl(const char* url) {
  if (strncmp(url, "https://", 8) || strlen(url) >= sizeof(tokenUrl)) return false;
  const char* path = strchr(url + 8, '/');
  if (!path || path == url + 8 || strcmp(path, "/api/realtime/token")) return false;
  for (const char* p = url + 8; p < path; ++p) {
    if (!(isalnum(static_cast<unsigned char>(*p)) || *p == '.' || *p == '-')) return false;
  }
  return true;
}

bool syncTime() {
  esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTime(0, 0, "time.apple.com", "pool.ntp.org");
  const auto started = millis();
  while (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
    if (millis() - started >= 20000) return fail("TIME");
    delay(10);
  }
  return true;
}

bool configure() {
  ready = false;
  M5.Mic.end();
  M5.Speaker.end();
  captureState = CaptureState::Off;
  if (!stopNetwork()) return false;
  turnActive = speakerStarted = false;
  command = Command::None;
  abortTurn = false;
  clearRecording();
  auto doc = cJSON_ParseWithOpts(input, nullptr, true);
  const char* ssid = jsonString(doc, "ssid");
  const char* password = jsonString(doc, "password");
  const char* url = jsonString(doc, "token_url");
  const char* token = jsonString(doc, "device_token");
  bool valid = strlen(ssid) >= 1 && strlen(ssid) <= 32
               && strlen(password) >= 8 && strlen(password) <= 63
               && validTokenUrl(url) && strlen(token) == 64;
  for (const char* p = token; valid && *p; ++p) {
    valid = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f');
  }
  if (valid) {
    strcpy(tokenUrl, url);
    strcpy(deviceToken, token);
    WiFi.disconnect();
    WiFi.begin(ssid, password);
  }
  for (const char* key : {"ssid", "password", "device_token"}) {
    auto item = cJSON_GetObjectItemCaseSensitive(doc, key);
    if (cJSON_IsString(item)) mbedtls_platform_zeroize(item->valuestring, strlen(item->valuestring));
  }
  cJSON_Delete(doc);
  mbedtls_platform_zeroize(input, sizeof(input));
  if (!valid) return fail("CONFIG");
  Serial.println("CONNECTING");
  const auto started = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - started >= 30000) return fail("WIFI");
    delay(10);
  }
  Serial.println("OK WIFI");
  if (!syncTime()) return false;
  Serial.println("OK TIME");
  ready = true;
  networkEnabled = true;
  networkState = NetworkState::Connecting;
  return prepareMic(); // Ready requires both the microphone and the session.
}

bool getClientSecret(char* secret, size_t secretCapacity, char* model, size_t modelCapacity) {
  char authorization[80];
  snprintf(authorization, sizeof(authorization), "Bearer %s", deviceToken);
  esp_http_client_config_t config = {};
  config.url = tokenUrl;
  config.method = HTTP_METHOD_POST;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 15000;
  config.disable_auto_redirect = true;
  auto client = esp_http_client_init(&config);
  if (!client) {
    mbedtls_platform_zeroize(authorization, sizeof(authorization));
    return fail("TOKEN_INIT");
  }
  esp_http_client_set_header(client, "Authorization", authorization);
  const auto opened = esp_http_client_open(client, 0);
  const auto headers = opened == ESP_OK ? esp_http_client_fetch_headers(client) : -1;
  const int status = esp_http_client_get_status_code(client);
  Serial.printf("TOKEN_HTTP %d\n", status);
  char body[2048] = {};
  size_t length = 0;
  bool complete = false;
  if (opened == ESP_OK && headers >= 0 && status == 200) {
    while (length < sizeof(body) - 1) {
      const int n = esp_http_client_read(client, body + length, sizeof(body) - 1 - length);
      if (n < 0) break;
      if (n == 0) {
        complete = esp_http_client_is_complete_data_received(client);
        break;
      }
      length += n;
    }
  }
  esp_http_client_cleanup(client);
  mbedtls_platform_zeroize(authorization, sizeof(authorization));
  if (!complete) {
    mbedtls_platform_zeroize(body, sizeof(body));
    return fail("TOKEN");
  }
  auto doc = cJSON_ParseWithOpts(body, nullptr, true);
  const char* value = jsonString(doc, "value");
  const char* modelValue = jsonString(doc, "model");
  auto expires = cJSON_GetObjectItemCaseSensitive(doc, "expires_at");
  bool valid = !strncmp(value, "ek_", 3) && strlen(value) > 3 && strlen(value) < secretCapacity
               && strlen(modelValue) > 0 && strlen(modelValue) < modelCapacity
               && cJSON_IsNumber(expires) && expires->valuedouble > time(nullptr) + 2;
  for (const char* p = value; valid && *p; ++p) {
    valid = isalnum(static_cast<unsigned char>(*p)) || *p == '_' || *p == '-';
  }
  for (const char* p = modelValue; valid && *p; ++p) {
    valid = isalnum(static_cast<unsigned char>(*p)) || *p == '.' || *p == '-';
  }
  if (valid) {
    strcpy(secret, value);
    strcpy(model, modelValue);
  }
  auto secretItem = cJSON_GetObjectItemCaseSensitive(doc, "value");
  if (cJSON_IsString(secretItem)) mbedtls_platform_zeroize(secretItem->valuestring, strlen(secretItem->valuestring));
  cJSON_Delete(doc);
  mbedtls_platform_zeroize(body, sizeof(body));
  return valid || fail("TOKEN_FORMAT");
}

bool sendEvent(esp_transport_handle_t socket, const char* json) {
  // ESP-IDF masks outgoing payloads in place, so use a writable copy.
  char* copy = strdup(json);
  if (!copy) return fail("MEMORY");
  const size_t length = strlen(copy);
  const int sent = esp_transport_ws_send_raw(socket,
      static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN),
      copy, length, 5000);
  mbedtls_platform_zeroize(copy, length);
  free(copy);
  return sent == static_cast<int>(length) || fail("WS_SEND");
}

bool sendPcm(esp_transport_handle_t socket, size_t offset, size_t samples) {
  constexpr size_t pcmChunkSamples = sampleRate / 10;
  constexpr char prefix[] = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
  constexpr size_t prefixLength = sizeof(prefix) - 1;
  constexpr size_t encodedCapacity = (pcmChunkSamples * 2 + 2) / 3 * 4 + 1;
  char event[prefixLength + encodedCapacity + 3];
  size_t encodedLength = 0;
  memcpy(event, prefix, prefixLength);
  bool ok = samples && samples <= pcmChunkSamples
            && mbedtls_base64_encode(reinterpret_cast<uint8_t*>(event + prefixLength), encodedCapacity,
                 &encodedLength, reinterpret_cast<const uint8_t*>(recording + offset), samples * 2) == 0;
  if (ok) {
    memcpy(event + prefixLength + encodedLength, "\"}", 3);
    ok = sendEvent(socket, event);
  } else fail("AUDIO_ENCODE");
  mbedtls_platform_zeroize(event, sizeof(event));
  return ok;
}

// Missing or unfamiliar usage fields must not silently produce a zero estimate.
double tokenCount(cJSON* object, const char* key) {
  auto value = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsNumber(value) && value->valuedouble >= 0
         && value->valuedouble == floor(value->valuedouble) ? value->valuedouble : -1;
}

void logUsage(cJSON* response, const char* model) {
  auto usage = cJSON_GetObjectItemCaseSensitive(response, "usage");
  auto in = cJSON_GetObjectItemCaseSensitive(usage, "input_token_details");
  auto out = cJSON_GetObjectItemCaseSensitive(usage, "output_token_details");
  auto cache = cJSON_GetObjectItemCaseSensitive(in, "cached_tokens_details");
  const double totalIn = tokenCount(usage, "input_tokens"), totalOut = tokenCount(usage, "output_tokens");
  const double textIn = tokenCount(in, "text_tokens"), audioIn = tokenCount(in, "audio_tokens");
  const double textOut = tokenCount(out, "text_tokens"), audioOut = tokenCount(out, "audio_tokens");
  const double cached = tokenCount(in, "cached_tokens");
  const double cachedText = cached == 0 ? 0 : tokenCount(cache, "text_tokens");
  const double cachedAudio = cached == 0 ? 0 : tokenCount(cache, "audio_tokens");
  Serial.printf("USAGE input=%.0f output=%.0f text_in=%.0f audio_in=%.0f cached_text=%.0f cached_audio=%.0f text_out=%.0f audio_out=%.0f\n",
                totalIn, totalOut, textIn, audioIn, cachedText, cachedAudio, textOut, audioOut);
  if (strcmp(model, "gpt-realtime-2.1") || totalIn < 0 || totalOut < 0 || textIn < 0 || audioIn < 0
      || textOut < 0 || audioOut < 0 || cached < 0 || cachedText < 0 || cachedAudio < 0
      || cachedText > textIn || cachedAudio > audioIn || cachedText + cachedAudio != cached
      || textIn + audioIn != totalIn || textOut + audioOut != totalOut) {
    Serial.println("ESTIMATED_USD unavailable");
    return;
  }
  // USD per million tokens, verified 2026-09-24. Includes prior conversation input.
  // https://developers.openai.com/api/docs/pricing
  const double usd = ((textIn - cachedText) * 4 + (audioIn - cachedAudio) * 32
                      + cached * 0.40 + textOut * 24 + audioOut * 64) / 1000000;
  Serial.printf("ESTIMATED_USD %.6f pricing=2026-09-24\n", usd);
}

bool sessionLoop(esp_transport_handle_t socket, const char* model) {
  enum class Stage { Configure, Idle, Upload, Commit, Respond, Clear, Finished };
  Stage stage = Stage::Configure;
  size_t messageLength = 0, frameRead = 0, sentSamples = 0;
  bool fragmentStarted = false, configured = false, audioDone = false;
  const auto connectedAt = millis();
  auto progressAt = connectedAt, pingAt = connectedAt;
  while (networkEnabled.load() && !abortTurn.load()) {
    if (WiFi.status() != WL_CONNECTED) return fail("WIFI_LOST");
    // Reopen explicitly with r before the API's 60 minute session limit.
    if (stage == Stage::Idle && millis() - connectedAt >= 55UL * 60 * 1000) {
      auto expected = NetworkState::Ready;
      if (networkState.compare_exchange_strong(expected, NetworkState::Connecting)) return fail("SESSION_EXPIRED");
    }
    const auto next = command.exchange(Command::None);
    if (next == Command::Voice || next == Command::Text) {
      if (stage != Stage::Idle) return fail("TURN_STATE");
      audioBytes = startupBytes;
      sentSamples = 0;
      audioDone = false;
      progressAt = millis();
      if (next == Command::Voice) stage = Stage::Upload;
      else {
        if (!sendEvent(socket, R"({"type":"conversation.item.create","item":{"type":"message","role":"user","content":[{"type":"input_text","text":"日本語で「こんにちは。音声接続のテストに成功しました。」とだけ話してください。"}]}})")
            || !sendEvent(socket, R"({"type":"response.create"})")) return false;
        stage = Stage::Respond;
      }
    } else if (next == Command::Release) {
      if (stage != Stage::Finished) return fail("TURN_STATE");
      stage = Stage::Idle;
      networkState = NetworkState::Ready;
    }
    if (stage == Stage::Upload) {
      // inputFinished is published AFTER the last completed-sample count.
      const int finished = inputFinished.load();
      const size_t count = capturedSamples.load();
      if (finished == 2) {
        if (!sendEvent(socket, R"({"type":"input_audio_buffer.clear"})")) return false;
        stage = Stage::Clear;
        progressAt = millis();
      } else if (count > sentSamples && (count - sentSamples >= sampleRate / 10 || finished == 1)) {
        const size_t n = std::min(count - sentSamples, sampleRate / 10);
        if (!sendPcm(socket, sentSamples, n)) return false;
        sentSamples += n;
        progressAt = millis();
      } else if (finished == 1 && sentSamples == count) {
        if (count < captureMinSamples || !sendEvent(socket, R"({"type":"input_audio_buffer.commit"})")) return fail("INPUT_COMMIT");
        Serial.printf("RELEASE_TO_COMMIT_MS %lu\n", static_cast<unsigned long>(millis() - releasedAt.load()));
        stage = Stage::Commit;
        progressAt = millis();
      }
      // Holding at the 10 s cap is allowed; commit still requires release.
      if (finished == 1 && millis() - releasedAt.load() >= 20000) return fail("AUDIO_SEND_TIMEOUT");
    }
    if (stage != Stage::Idle && stage != Stage::Finished && stage != Stage::Upload
        && millis() - progressAt >= 45000) return fail("RESPONSE_TIMEOUT");
    if (millis() - pingAt >= 20000) {
      char payload = 0;
      if (esp_transport_ws_send_raw(socket,
          static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_PING | WS_TRANSPORT_OPCODES_FIN),
          &payload, 1, 1000) != 1) return fail("WS_PING");
      pingAt = millis();
    }
    // WS header/payload timeouts can discard a partial frame in ESP-IDF. Poll
    // briefly to keep uploading, but allow a full second once a read starts.
    // During Configure, read directly to drain any HTTP-upgrade buffered data;
    // ws_poll_read only examines the parent TLS transport, not that WS buffer.
    if (stage != Stage::Configure) {
      const int pending = esp_transport_poll_read(socket, 10);
      if (pending < 0) return fail("WS_READ");
      if (!pending) { delay(1); continue; }
    }
    char chunk[2048];
    const int n = esp_transport_read(socket, chunk, sizeof(chunk), 1000);
    if (n < 0) return fail("WS_READ");
    if (!n) {
      if (frameRead) return fail("WS_READ");
      delay(1);
      continue;
    }
    const auto opcode = esp_transport_ws_get_read_opcode(socket);
    const int frameSize = esp_transport_ws_get_read_payload_len(socket);
    if (frameSize < 0 || frameSize > static_cast<int>(messageCapacity)) return fail("EVENT_SIZE");
    if (!frameRead) {
      if (opcode == WS_TRANSPORT_OPCODES_TEXT && !fragmentStarted) fragmentStarted = true;
      else if (opcode != WS_TRANSPORT_OPCODES_CONT || !fragmentStarted) return fail("WS_FRAME");
    }
    if (messageLength + n > messageCapacity) return fail("EVENT_SIZE");
    memcpy(message + messageLength, chunk, n);
    messageLength += n;
    frameRead += n;
    if (frameRead < static_cast<size_t>(frameSize)) continue;
    if (frameRead != static_cast<size_t>(frameSize)) return fail("WS_FRAME");
    frameRead = 0;
    if (!esp_transport_ws_get_fin_flag(socket)) continue;
    fragmentStarted = false;
    message[messageLength] = '\0';
    auto event = cJSON_ParseWithOpts(message, nullptr, true);
    mbedtls_platform_zeroize(message, messageLength);
    messageLength = 0;
    if (!event) return fail("EVENT_JSON");
    const char* type = jsonString(event, "type");
    bool ok = true;
    if (!strcmp(type, "session.created") && !configured) {
      configured = true;
      ok = sendEvent(socket, R"({"type":"session.update","session":{"type":"realtime","output_modalities":["audio"],"max_output_tokens":256,"truncation":{"type":"retention_ratio","retention_ratio":0.8,"token_limits":{"post_instructions":8000}},"audio":{"input":{"format":{"type":"audio/pcm","rate":24000},"turn_detection":null},"output":{"format":{"type":"audio/pcm","rate":24000},"voice":"marin"}}}})");
    } else if (!strcmp(type, "session.updated") && configured && stage == Stage::Configure) {
      auto session = cJSON_GetObjectItemCaseSensitive(event, "session");
      auto config = cJSON_GetObjectItemCaseSensitive(session, "audio");
      auto output = cJSON_GetObjectItemCaseSensitive(config, "output");
      auto format = cJSON_GetObjectItemCaseSensitive(output, "format");
      auto rate = cJSON_GetObjectItemCaseSensitive(format, "rate");
      auto in = cJSON_GetObjectItemCaseSensitive(config, "input");
      auto inputFormat = cJSON_GetObjectItemCaseSensitive(in, "format");
      auto inputRate = cJSON_GetObjectItemCaseSensitive(inputFormat, "rate");
      ok = !strcmp(jsonString(format, "type"), "audio/pcm") && cJSON_IsNumber(rate) && rate->valueint == sampleRate
           && !strcmp(jsonString(inputFormat, "type"), "audio/pcm") && cJSON_IsNumber(inputRate)
           && inputRate->valueint == sampleRate && cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(in, "turn_detection"));
      if (ok) {
        Serial.printf("SESSION_READY_MS %lu\n", static_cast<unsigned long>(millis() - connectedAt));
        Serial.println("OK SESSION");
        stage = Stage::Idle;
        networkState = NetworkState::Ready;
      } else fail("AUDIO_FORMAT");
    } else if (!strcmp(type, "input_audio_buffer.cleared") && stage == Stage::Clear) {
      stage = Stage::Finished;
      networkState = NetworkState::Done;
    } else if (!strcmp(type, "input_audio_buffer.committed") && stage == Stage::Commit) {
      Serial.println("OK INPUT_AUDIO");
      ok = sendEvent(socket, R"({"type":"response.create"})");
      stage = Stage::Respond;
      progressAt = millis();
    } else if (!strcmp(type, "response.output_audio.delta")) {
      const char* delta = jsonString(event, "delta");
      size_t decoded = 0;
      ok = stage == Stage::Respond && !audioDone && strlen(delta) > 0
           && mbedtls_base64_decode(audio + audioBytes, audioCapacity - startupBytes - audioBytes, &decoded,
                                    reinterpret_cast<const uint8_t*>(delta), strlen(delta)) == 0
           && decoded > 0 && decoded % 2 == 0;
      if (ok) {
        if (audioBytes == startupBytes) Serial.printf("RELEASE_TO_FIRST_AUDIO_MS %lu\n", static_cast<unsigned long>(millis() - releasedAt.load()));
        audioBytes += decoded;
        availableAudio.store(audioBytes);
      } else fail("AUDIO_DATA");
    } else if (!strcmp(type, "response.output_audio.done")) {
      audioDone = true;
    } else if (!strcmp(type, "response.done")) {
      auto response = cJSON_GetObjectItemCaseSensitive(event, "response");
      logUsage(response, model);
      ok = stage == Stage::Respond && audioDone && audioBytes > startupBytes
           && !strcmp(jsonString(response, "status"), "completed");
      if (ok) {
        Serial.printf("RESPONSE_DONE_MS %lu\n", static_cast<unsigned long>(millis() - releasedAt.load()));
        // isPlaying() can release PCM before the I2S queue reaches the speaker.
        // A silent tail keeps end() from clipping the last spoken syllable.
        memset(audio + audioBytes, 0, startupBytes);
        audioBytes += startupBytes;
        availableAudio = audioBytes;
        stage = Stage::Finished;
        networkState = NetworkState::Done;
      } else fail("RESPONSE_INCOMPLETE");
    } else if (!strcmp(type, "error")) ok = fail("REALTIME_API");
    cJSON_Delete(event);
    if (!ok) return false;
    delay(1);
  }
  return true;
}

void networkTask(void*) {
  for (;;) {
    if (networkState.load() == NetworkState::Off) { delay(10); continue; }
    if (networkState.load() == NetworkState::Failed) {
      if (!networkEnabled.load()) networkState = NetworkState::Off;
      delay(10);
      continue;
    }
    char secret[512] = {}, model[64] = {};
    const auto tokenAt = millis();
    bool ok = getClientSecret(secret, sizeof(secret), model, sizeof(model));
    Serial.printf("TOKEN_MS %lu\n", static_cast<unsigned long>(millis() - tokenAt));
    auto tls = ok ? esp_transport_ssl_init() : nullptr;
    auto socket = tls ? esp_transport_ws_init(tls) : nullptr;
    if (ok && !socket) ok = fail("MEMORY");
    if (ok && networkEnabled.load()) {
      esp_transport_ssl_crt_bundle_attach(tls, esp_crt_bundle_attach);
      char path[128], authorization[528];
      snprintf(path, sizeof(path), "/v1/realtime?model=%s", model);
      snprintf(authorization, sizeof(authorization), "Bearer %s", secret);
      esp_transport_ws_config_t config = {};
      config.ws_path = path;
      config.auth = authorization;
      config.propagate_control_frames = false;
      const bool setupOk = esp_transport_ws_set_config(socket, &config) == ESP_OK;
      mbedtls_platform_zeroize(authorization, sizeof(authorization));
      mbedtls_platform_zeroize(secret, sizeof(secret));
      const auto wsAt = millis();
      const int connected = setupOk ? esp_transport_connect(socket, "api.openai.com", 443, 15000) : -1;
      Serial.printf("WS_HTTP %d\nWS_CONNECT_MS %lu\n", esp_transport_ws_get_upgrade_request_status(socket), static_cast<unsigned long>(millis() - wsAt));
      esp_transport_ws_set_auth(socket, nullptr);
      if (connected == 0) ok = sessionLoop(socket, model);
      else ok = fail("WS_CONNECT");
    }
    mbedtls_platform_zeroize(secret, sizeof(secret));
    if (socket) { esp_transport_close(socket); esp_transport_destroy(socket); }
    if (tls) esp_transport_destroy(tls);
    mbedtls_platform_zeroize(message, messageCapacity + 1);
    // No buffer access occurs after this publication, until a new session starts.
    networkState = networkEnabled.load() ? NetworkState::Failed : NetworkState::Off;
    if (networkEnabled.load()) Serial.println("RECONNECT_REQUIRED");
  }
}

bool beginTurn(bool voiceInput) {
  auto expected = NetworkState::Ready;
  if (!networkState.compare_exchange_strong(expected, NetworkState::Busy)) return false;
  capturedSamples = 0;
  availableAudio = startupBytes;
  inputFinished = 0;
  releasedAt = voiceInput ? 0 : millis();
  queuedAudio = 0;
  underruns = 0;
  speakerStarted = underrun = false;
  voiceTurn = voiceInput;
  turnActive = true;
  memset(audio, 0, startupBytes);
  command = voiceInput ? Command::Voice : Command::Text;
  return true;
}

void updatePlayback() {
  const auto state = networkState.load();
  if (state == NetworkState::Failed) {
    if (ready || turnActive) {
      M5.Mic.end();
      M5.Speaker.end();
      captureState = CaptureState::Off;
      clearRecording();
      mbedtls_platform_zeroize(audio, audioCapacity);
      ready = turnActive = speakerStarted = false;
    }
    return;
  }
  if (!turnActive || abortTurn.load()) return;
  const size_t produced = availableAudio.load();
  const bool finished = state == NetworkState::Done;
  constexpr size_t playbackChunkBytes = sampleRate * 2 / 10; // 100 ms
  constexpr size_t prebufferBytes = sampleRate * 2 * 240 / 1000;
  if (!speakerStarted && produced > startupBytes
      && (produced >= startupBytes + prebufferBytes || finished)) {
    // Capture has drained and ended before inputFinished permits response.create.
    if (!M5.Speaker.begin()) { fail("PLAYBACK"); abortTurn = true; return; }
    speakerStarted = true;
    playbackAt = millis();
    Serial.printf("RELEASE_TO_PLAYBACK_MS %lu\n", static_cast<unsigned long>(playbackAt - releasedAt.load()));
    Serial.println("PLAYING");
  }
  if (speakerStarted) {
    if (millis() - playbackAt > 50000) { fail("PLAYBACK_TIMEOUT"); abortTurn = true; return; }
    const auto occupied = M5.Speaker.isPlaying(0);
    if (!occupied && queuedAudio > 0 && !finished && !underrun) { ++underruns; underrun = true; }
    while (M5.Speaker.isPlaying(0) < 2 && queuedAudio < produced) {
      const size_t n = std::min(playbackChunkBytes, produced - queuedAudio);
      if (n < playbackChunkBytes && !finished) break;
      if (!M5.Speaker.playRaw(reinterpret_cast<int16_t*>(audio + queuedAudio), n / 2, sampleRate, false, 1, 0)) {
        fail("PLAYBACK"); abortTurn = true; return;
      }
      queuedAudio += n;
      underrun = false;
    }
  }
  if (finished && ((!speakerStarted && produced == startupBytes)
      || (speakerStarted && queuedAudio == produced && !M5.Speaker.isPlaying()))) {
    M5.Speaker.end();
    Serial.printf("PLAYBACK_UNDERRUNS %lu\n", static_cast<unsigned long>(underruns));
    if (produced > startupBytes) Serial.println(voiceTurn ? "REALTIME_TURN_DONE" : "REALTIME_TEST_DONE");
    // Worker is Finished and M5 has released all buffers; now they can be erased.
    clearRecording();
    mbedtls_platform_zeroize(audio, audioCapacity);
    turnActive = speakerStarted = false;
    prepareMic();
    command = Command::Release;
  }
}

void updateCapture() {
  if (!ready || captureState == CaptureState::Off) return;
  const auto now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    ready = false;
    captureFailure("WIFI_LOST");
    return;
  }
  switch (captureState) {
    case CaptureState::Warming:
    case CaptureState::Ready:
      if (captureState == CaptureState::Ready && M5.BtnA.wasPressed()) {
        if (!beginTurn(true)) break;
        pressedAt = captureStateAt = now;
        shortPress = false;
        captureState = CaptureState::Starting;
        break;
      }
      // Keep the codec warm, but never retain or send idle microphone audio.
      if (!M5.Mic.isRecording()) {
        mbedtls_platform_zeroize(discard, sizeof(discard));
        if (captureState == CaptureState::Warming && now - captureStateAt >= micWarmupMs
            && !M5.BtnA.isPressed() && networkState.load() == NetworkState::Ready) {
          captureState = CaptureState::Ready;
          Serial.println("REALTIME_READY");
        }
        if (!M5.Mic.record(discard, captureChunkSamples, sampleRate, false)) {
          captureFailure("MIC_CAPTURE");
          break;
        }
        captureProgressAt = now;
      } else if (now - captureProgressAt > captureTimeoutMs) captureFailure("MIC_TIMEOUT");
      break;
    case CaptureState::Starting:
      // The idle request owns discard until it completes.
      if (!M5.Mic.isRecording()) {
        mbedtls_platform_zeroize(discard, sizeof(discard));
        if (!M5.BtnA.isPressed()) {
          Serial.println("RECORDING_DISCARDED SHORT");
          M5.Mic.end();
          inputFinished = 2;
          captureState = CaptureState::Off;
          break;
        }
        captureState = CaptureState::Recording;
        captureProgressAt = now;
        Serial.println("RECORDING");
      } else if (now - captureStateAt > captureTimeoutMs) captureFailure("MIC_TIMEOUT");
      break;
    case CaptureState::Recording:
      if (!M5.BtnA.isPressed() || recordedSamples == captureMaxSamples) {
        shortPress = now - pressedAt < 150;
        if (!M5.BtnA.isPressed()) releasedAt = now;
        captureState = CaptureState::Draining;
        captureStateAt = now;
        Serial.println(recordedSamples == captureMaxSamples ? "RECORDING_LIMIT RELEASE_BUTTON" : "BUTTON_RELEASED");
        break;
      }
      // Only completed requests can be read by the network task. No buffer is
      // reused until both tasks finish the turn; pending requests remain private.
      if (now - pressedAt >= 150) {
        capturedSamples = recordedSamples - M5.Mic.isRecording() * captureChunkSamples;
      }
      // Keep two adjacent requests queued while polling the physical button.
      if (M5.Mic.isRecording() < 2) {
        if (!M5.Mic.record(recording + recordedSamples, captureChunkSamples, sampleRate, false)) {
          captureFailure("MIC_CAPTURE");
          break;
        }
        recordedSamples += captureChunkSamples;
        captureProgressAt = now;
      } else if (now - captureProgressAt > captureTimeoutMs) captureFailure("MIC_TIMEOUT");
      break;
    case CaptureState::Draining:
      // Never encode queued data before the background writer has finished.
      if (M5.Mic.isRecording()) {
        if (now - captureStateAt > captureTimeoutMs) captureFailure("MIC_TIMEOUT");
        break;
      }
      M5.Mic.end();
      if (shortPress || recordedSamples < captureMinSamples) {
        Serial.println("RECORDING_DISCARDED SHORT");
        inputFinished = 2;
        captureState = CaptureState::Off;
      } else {
        capturedSamples = recordedSamples;
        Serial.printf("RECORDED_MS %u\n", static_cast<unsigned>(recordedSamples * 1000 / sampleRate));
        captureState = CaptureState::Captured;
      }
      break;
    case CaptureState::Captured:
      // Upload can continue at the cap, but generation requires physical release.
      if (!M5.BtnA.isPressed()) {
        if (!releasedAt.load()) releasedAt = now;
        inputFinished = 1;
        captureState = CaptureState::Off;
        Serial.println("REQUESTING");
      }
      break;
    case CaptureState::Off:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(100);
  // The upstream WS transport can log the authorization header on write failure.
  // Keep our numeric/stage diagnostics, but prevent raw headers being printed.
  esp_log_level_set("transport_ws", ESP_LOG_NONE);
  esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);
  auto config = M5.config();
  M5.begin(config);
  M5.Mic.end();
  M5.Speaker.end();
  M5.Speaker.setVolume(64);
  audio = static_cast<uint8_t*>(ps_malloc(audioCapacity));
  recording = static_cast<int16_t*>(ps_malloc(captureMaxSamples * sizeof(int16_t)));
  message = static_cast<char*>(ps_malloc(messageCapacity + 1));
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  if (audio && recording && message) {
    workerCreated = xTaskCreate(networkTask, "realtime-network", 24576, nullptr, 1, nullptr) == pdPASS;
  }
}

void loop() {
  M5.update();
  while (Serial.available()) {
    const char value = Serial.read();
    lastInputAt = millis();
    if (value == '\n') {
      input[inputLength] = '\0';
      if (!audio || !message || !recording || !workerCreated) fail("MEMORY");
      else if (inputOverflow) fail("CONFIG");
      else if (!strcmp(input, "?")) Serial.println("AVS3R_REALTIME_READY 3");
      else if (!strcmp(input, "p")) {
        if (ready && !M5.BtnA.isPressed()
            && captureState == CaptureState::Ready && beginTurn(false)) {
          M5.Mic.end();
          captureState = CaptureState::Off;
        }
        else fail("BUSY");
      }
      else if (!strcmp(input, "r")) {
        if (turnActive || M5.BtnA.isPressed()) fail("BUSY");
        else if (!tokenUrl[0] || WiFi.status() != WL_CONNECTED) fail("WIFI_LOST");
        else if (stopNetwork()) {
          Serial.println("SESSION_HISTORY_RESET");
          command = Command::None;
          abortTurn = false;
          ready = true;
          networkEnabled = true;
          networkState = NetworkState::Connecting;
          prepareMic();
        }
      }
      else if (inputLength) configure();
      mbedtls_platform_zeroize(input, sizeof(input));
      inputLength = 0;
      inputOverflow = false;
    } else if (!inputOverflow) {
      if (inputLength < sizeof(input) - 1 && value != '\0') input[inputLength++] = value;
      else inputOverflow = true;
    }
  }
  if ((inputLength || inputOverflow) && millis() - lastInputAt >= 5000) {
    mbedtls_platform_zeroize(input, sizeof(input));
    inputLength = 0;
    inputOverflow = false;
    fail("CONFIG_TIMEOUT");
  }
  updateCapture();
  updatePlayback();
  M5.delay(1);
}
