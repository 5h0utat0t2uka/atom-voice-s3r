#include <M5Unified.h>
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
constexpr size_t audioCapacity = startupBytes + sampleRate * 2 * 20;
constexpr size_t messageCapacity = 128 * 1024;
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

const char* jsonString(cJSON* object, const char* key) {
  auto item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) ? item->valuestring : "";
}

bool fail(const char* code) {
  Serial.printf("FAIL %s\n", code);
  return false;
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
  Serial.println("REALTIME_READY");
  return true;
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
  if (!client) return fail("TOKEN_INIT");
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
  free(copy);
  return sent == static_cast<int>(length) || fail("WS_SEND");
}

bool receiveAudio(esp_transport_handle_t socket) {
  size_t messageLength = 0;
  size_t frameRead = 0;
  bool fragmentStarted = false;
  bool configured = false;
  bool responseRequested = false;
  bool audioDone = false;
  const auto started = millis();
  while (millis() - started < 45000) {
    if (WiFi.status() != WL_CONNECTED) return fail("WIFI_LOST");
    char chunk[2048];
    int n = esp_transport_read(socket, chunk, sizeof(chunk), 1000);
    if (n < 0) return fail("WS_READ");
    if (!n) continue; // Includes control frames handled by ESP-IDF.
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
    messageLength = 0;
    if (!event) return fail("EVENT_JSON");
    const char* type = jsonString(event, "type");
    bool ok = true;
    bool finished = false;
    if (!strcmp(type, "session.created") && !configured) {
      configured = true;
      Serial.println("OK SESSION");
      ok = sendEvent(socket, R"({"type":"session.update","session":{"type":"realtime","output_modalities":["audio"],"max_output_tokens":256,"audio":{"input":{"turn_detection":null},"output":{"format":{"type":"audio/pcm","rate":24000},"voice":"marin"}}}})");
    } else if (!strcmp(type, "session.updated") && configured && !responseRequested) {
      auto session = cJSON_GetObjectItemCaseSensitive(event, "session");
      auto config = cJSON_GetObjectItemCaseSensitive(session, "audio");
      auto output = cJSON_GetObjectItemCaseSensitive(config, "output");
      auto format = cJSON_GetObjectItemCaseSensitive(output, "format");
      auto rate = cJSON_GetObjectItemCaseSensitive(format, "rate");
      ok = !strcmp(jsonString(format, "type"), "audio/pcm") && cJSON_IsNumber(rate) && rate->valueint == sampleRate;
      if (!ok) fail("AUDIO_FORMAT");
      if (ok) ok = sendEvent(socket, R"({"type":"conversation.item.create","item":{"type":"message","role":"user","content":[{"type":"input_text","text":"日本語で「こんにちは。音声接続のテストに成功しました。」とだけ話してください。"}]}})");
      if (ok) ok = sendEvent(socket, R"({"type":"response.create","response":{"output_modalities":["audio"],"max_output_tokens":256}})");
      responseRequested = ok;
    } else if (!strcmp(type, "response.output_audio.delta")) {
      const char* delta = jsonString(event, "delta");
      size_t decoded = 0;
      ok = responseRequested && !audioDone && strlen(delta) > 0
           && mbedtls_base64_decode(audio + audioBytes, audioCapacity - audioBytes, &decoded,
                                    reinterpret_cast<const uint8_t*>(delta), strlen(delta)) == 0
           && decoded > 0 && decoded % 2 == 0;
      if (ok) audioBytes += decoded;
      else fail("AUDIO_DATA");
    } else if (!strcmp(type, "response.output_audio.done")) {
      audioDone = true;
    } else if (!strcmp(type, "response.done")) {
      auto response = cJSON_GetObjectItemCaseSensitive(event, "response");
      ok = responseRequested && audioDone && audioBytes > startupBytes
           && !strcmp(jsonString(response, "status"), "completed");
      if (!ok) fail("RESPONSE_INCOMPLETE");
      finished = true;
    } else if (!strcmp(type, "error")) {
      // Server error messages can contain request data; log only the failure stage.
      ok = fail("REALTIME_API");
    }
    cJSON_Delete(event);
    if (!ok) return false;
    if (finished) return true;
    delay(1);
  }
  return fail("RESPONSE_TIMEOUT");
}

bool requestAudio() {
  char secret[512] = {};
  char model[64] = {};
  if (!getClientSecret(secret, sizeof(secret), model, sizeof(model))) return false;
  Serial.println("OK TOKEN");
  auto tls = esp_transport_ssl_init();
  auto socket = tls ? esp_transport_ws_init(tls) : nullptr;
  if (!socket) {
    if (tls) esp_transport_destroy(tls);
    mbedtls_platform_zeroize(secret, sizeof(secret));
    return fail("MEMORY");
  }
  esp_transport_ssl_crt_bundle_attach(tls, esp_crt_bundle_attach);
  char path[128];
  char authorization[528];
  snprintf(path, sizeof(path), "/v1/realtime?model=%s", model);
  snprintf(authorization, sizeof(authorization), "Bearer %s", secret);
  esp_transport_ws_config_t config = {};
  config.ws_path = path;
  config.auth = authorization;
  config.propagate_control_frames = false;
  const bool setupOk = esp_transport_ws_set_config(socket, &config) == ESP_OK;
  mbedtls_platform_zeroize(secret, sizeof(secret));
  mbedtls_platform_zeroize(authorization, sizeof(authorization));
  const int connected = setupOk ? esp_transport_connect(socket, "api.openai.com", 443, 15000) : -1;
  Serial.printf("WS_HTTP %d\n", esp_transport_ws_get_upgrade_request_status(socket));
  esp_transport_ws_set_auth(socket, nullptr);
  bool ok = connected == 0;
  if (ok) ok = receiveAudio(socket);
  else fail("WS_CONNECT");
  if (connected == 0) {
    char closeCode[2] = {3, static_cast<char>(232)}; // 1000, normal closure
    esp_transport_ws_send_raw(socket,
        static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_CLOSE | WS_TRANSPORT_OPCODES_FIN),
        closeCode, sizeof(closeCode), 1000);
  }
  esp_transport_close(socket);
  esp_transport_destroy(socket);
  esp_transport_destroy(tls);
  return ok;
}

void runTest() {
  audioBytes = startupBytes;
  memset(audio, 0, startupBytes);
  if (WiFi.status() != WL_CONNECTED) {
    ready = false;
    fail("WIFI_LOST");
    return;
  }
  Serial.println("REQUESTING");
  if (requestAudio()) {
    Serial.printf("AUDIO_BYTES %u\n", static_cast<unsigned>(audioBytes - startupBytes));
    if (!M5.Speaker.begin() || !M5.Speaker.playRaw(reinterpret_cast<int16_t*>(audio), audioBytes / 2,
                                                sampleRate, false, 1, 0)) {
      fail("PLAYBACK");
    } else {
      Serial.println("PLAYING");
      const auto started = millis();
      const uint32_t timeout = audioBytes * 1000 / (sampleRate * 2) + 3000;
      while (M5.Speaker.isPlaying() && millis() - started < timeout) delay(1);
      if (M5.Speaker.isPlaying()) fail("PLAYBACK_TIMEOUT");
      else Serial.println("REALTIME_TEST_DONE");
    }
  }
  M5.Speaker.end();
  mbedtls_platform_zeroize(audio, audioCapacity);
  mbedtls_platform_zeroize(message, messageCapacity + 1);
  // Refresh the button state so presses during the test do not trigger another paid request.
  M5.update();
  while (M5.BtnA.isPressed()) { M5.delay(1); M5.update(); }
  while (Serial.available()) Serial.read();
  Serial.println("REALTIME_READY");
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
  message = static_cast<char*>(ps_malloc(messageCapacity + 1));
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
}

void loop() {
  M5.update();
  if (ready && audio && message && M5.BtnA.wasPressed()) runTest();
  while (Serial.available()) {
    const char value = Serial.read();
    lastInputAt = millis();
    if (value == '\n') {
      input[inputLength] = '\0';
      if (!audio || !message) fail("MEMORY");
      else if (inputOverflow) fail("CONFIG");
      else if (!strcmp(input, "?")) Serial.println("AVS3R_REALTIME_READY 1");
      else if (!strcmp(input, "p") && ready) runTest();
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
  M5.delay(1);
}
