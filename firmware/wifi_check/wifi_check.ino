#include <Arduino.h>
#include <atomic>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_sntp.h>
#include <mbedtls/platform_util.h>

char input[512] = {};
size_t inputLength = 0;
bool overflowed = false;
uint32_t lastInputAt = 0;
std::atomic<unsigned> disconnectReason{0};

void checkInternet() {
  IPAddress address;
  if (!WiFi.hostByName("www.espressif.com", address)) {
    Serial.println("FAIL DNS");
    return;
  }
  Serial.println("OK DNS");

  esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTime(0, 0, "time.apple.com", "pool.ntp.org");
  const uint32_t startedAt = millis();
  while (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
    if (millis() - startedAt >= 20000) {
      Serial.println("FAIL TIME");
      return;
    }
    delay(10);
  }
  Serial.println("OK TIME");

  // Use the CA bundle shipped with the pinned board package. Verify both
  // certificate trust and hostname; never fall back to insecure TLS.
  esp_http_client_config_t config = {};
  config.url = "https://www.espressif.com/";
  config.method = HTTP_METHOD_HEAD;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 15000;
  config.disable_auto_redirect = true;
  auto client = esp_http_client_init(&config);
  if (!client) {
    Serial.println("FAIL HTTPS_INIT");
    return;
  }
  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (result != ESP_OK) {
    Serial.println("FAIL HTTPS");
    return;
  }
  Serial.printf("HTTP %d\n", status);
  if (status < 200 || status >= 400) {
    Serial.println("FAIL HTTP_STATUS");
    return;
  }
  Serial.println("OK HTTPS");
  Serial.println("WIFI_CHECK_DONE");
}

void connectFromInput() {
  auto document = cJSON_ParseWithOpts(input, nullptr, true);
  auto ssid = cJSON_GetObjectItemCaseSensitive(document, "ssid");
  auto password = cJSON_GetObjectItemCaseSensitive(document, "password");
  const bool valid = cJSON_IsString(ssid) && cJSON_IsString(password)
                    && strlen(ssid->valuestring) >= 1 && strlen(ssid->valuestring) <= 32
                    && strlen(password->valuestring) >= 8 && strlen(password->valuestring) <= 63;
  if (valid) {
    WiFi.disconnect();
    disconnectReason.store(0);
    WiFi.begin(ssid->valuestring, password->valuestring);
  }
  // WiFi.begin copies the configuration into the driver, configured for RAM.
  if (cJSON_IsString(ssid)) {
    mbedtls_platform_zeroize(ssid->valuestring, strlen(ssid->valuestring));
  }
  if (cJSON_IsString(password)) {
    mbedtls_platform_zeroize(password->valuestring, strlen(password->valuestring));
  }
  cJSON_Delete(document);
  mbedtls_platform_zeroize(input, sizeof(input));
  if (!valid) {
    Serial.println("FAIL CONFIG");
    return;
  }
  Serial.println("CONNECTING");
  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startedAt >= 30000) {
      Serial.printf("DISCONNECT_REASON %u\n", disconnectReason.load());
      Serial.println("FAIL WIFI");
      WiFi.disconnect();
      return;
    }
    delay(10);
  }
  Serial.println("OK WIFI");
  Serial.printf("RSSI %d\n", static_cast<int>(WiFi.RSSI()));
  checkInternet();
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(100);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    disconnectReason.store(info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
}

void loop() {
  while (Serial.available()) {
    const char value = Serial.read();
    lastInputAt = millis();
    if (value == '\n') {
      input[inputLength] = '\0';
      if (overflowed) {
        Serial.println("FAIL CONFIG");
      } else if (!strcmp(input, "?")) {
        Serial.println("AVS3R_WIFI_READY 1");
      } else if (inputLength) {
        connectFromInput();
      }
      mbedtls_platform_zeroize(input, sizeof(input));
      inputLength = 0;
      overflowed = false;
    } else if (!overflowed) {
      if (inputLength < sizeof(input) - 1 && value != '\0') {
        input[inputLength++] = value;
      } else {
        overflowed = true;
      }
    }
  }
  if ((inputLength || overflowed) && millis() - lastInputAt >= 5000) {
    mbedtls_platform_zeroize(input, sizeof(input));
    inputLength = 0;
    overflowed = false;
    Serial.println("FAIL CONFIG_TIMEOUT");
  }
  delay(1);
}
