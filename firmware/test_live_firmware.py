from pathlib import Path
import subprocess
import tempfile
import unittest


class LiveFirmwareTests(unittest.TestCase):
    def test_pcm_queue_boundaries_wraparound_and_concurrent_order(self):
        source = Path(__file__).parent / "tests/pcm_queue.cpp"
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "queue"
            subprocess.run(["c++", "-std=c++17", "-pthread", "-O2", str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_saved_configuration_and_nonblocking_connection(self):
        root = Path(__file__).parent
        sketch = (root / "live_chat/live_chat.ino").read_text()
        declarations = sketch[sketch.index("DeviceConfig settings"):sketch.index("char input[")]
        functions = sketch[sketch.index("void beginConnection()"):sketch.index("void configure()")]
        preferences = r"""
#pragma once
#include <cstddef>
#include <cstring>
#include <vector>
inline std::vector<unsigned char> stored;
inline bool openOk = true, writeOk = true, readOk = true;
inline unsigned writes = 0;
class Preferences {
public:
  bool begin(const char *) { return openOk; }
  void end() {}
  bool isKey(const char *) { return !stored.empty(); }
  size_t getBytesLength(const char *) { return stored.size(); }
  size_t getBytes(const char *, void *target, size_t size) {
    if (!readOk || size != stored.size()) return 0;
    memcpy(target, stored.data(), size); return size;
  }
  size_t putBytes(const char *, const void *source, size_t size) {
    ++writes;
    if (!writeOk) return 0;
    const auto *bytes = static_cast<const unsigned char *>(source);
    stored.assign(bytes, bytes + size); return size;
  }
};
"""
        harness = r"""
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include "device_config.h"
constexpr int WL_CONNECTED = 3;
constexpr int SNTP_SYNC_STATUS_RESET = 0, SNTP_SYNC_STATUS_COMPLETED = 1;
uint32_t now = 0;
uint32_t millis() { return now; }
bool configured = false, running = false, timeComplete = false;
unsigned syncStarts = 0, consumed = 0, afterSuccess = 0;
void esp_sntp_stop() {}
void esp_sntp_set_sync_status(int) { timeComplete = false; consumed = afterSuccess = 0; }
void configTime(int, int, const char *, const char *) { ++syncStarts; }
int esp_sntp_get_sync_status() {
  if (consumed) ++afterSuccess;
  if (!timeComplete) return SNTP_SYNC_STATUS_RESET;
  timeComplete = false; ++consumed;
  return SNTP_SYNC_STATUS_COMPLETED;
}
struct {
  int statusCode = 0;
  unsigned starts = 0;
  std::string ssid, password;
  int status() { return statusCode; }
  void disconnect() { statusCode = 0; }
  void begin(const char *s, const char *p) { ++starts; ssid = s; password = p; }
} WiFi;
struct {
  std::vector<std::string> logs;
  void println(const char *line) { logs.emplace_back(line); }
  void printf(const char *, const char *code) { logs.emplace_back(std::string("FAIL ") + code); }
} Serial;
void liveLog(const char *line) { Serial.println(line); }
void liveLogf(const char *format, const char *code) { Serial.printf(format, code); }
// DECLARATIONS
// FUNCTIONS
int main() {
  // Empty storage does not fabricate credentials; a saved record survives a new instance.
  DeviceConfig first = {};
  assert(first.load() == 0);
  first.version = 1;
  strcpy(first.ssid, "phone"); strcpy(first.password, "test-password");
  strcpy(first.host, "device.example"); memset(first.token, 'a', 64);
  assert(first.valid() && first.save());
  DeviceConfig restored = {};
  assert(restored.load() == 1 && !memcmp(&first, &restored, sizeof(first)));
  const auto original = stored;
  // Reject damaged, truncated, wrong-version and unterminated stored credentials.
  stored.pop_back(); assert(restored.load() == -1); stored = original;
  stored[0] = 9; assert(restored.load() == -1); stored = original;
  auto invalid = first; memset(invalid.host, 'x', sizeof(invalid.host));
  assert(!invalid.valid() && !invalid.save());
  invalid = first; memset(invalid.ssid, 'x', sizeof(invalid.ssid)); assert(!invalid.valid());
  invalid = first; invalid.password[0] = '\x01'; assert(!invalid.valid());
  invalid = first; invalid.token[0] = 'z'; assert(!invalid.valid());
  readOk = false; assert(restored.load() == -1); readOk = true;
  openOk = false; assert(restored.load() == -1 && !first.save()); openOk = true;

  // A boot with an unavailable hotspot times out, then retries without saving or recording.
  assert(settings.load() == 1);
  retrySavedConnection = true;
  const unsigned initialWrites = writes;
  beginConnection();
  for (now = 0; now < 30000; now += 10) updateConnection();
  assert(connectionState == ConnectionState::Wifi);
  updateConnection();
  assert(connectionState == ConnectionState::Retry && !configured && !running);
  now += 29999; updateConnection(); assert(WiFi.starts == 1);
  ++now; updateConnection(); assert(WiFi.starts == 2);
  WiFi.statusCode = WL_CONNECTED; updateConnection();
  assert(connectionState == ConnectionState::Time);
  timeComplete = true; updateConnection(); updateConnection();
  assert(configured && connectionState == ConnectionState::Ready);
  assert(consumed == 1 && afterSuccess == 0 && !running && writes == initialWrites);
  assert(WiFi.ssid == "phone" && WiFi.password == "test-password");

  // Existing flash settings survive unsuccessful provisioning (Wi-Fi and SNTP).
  settings = first; strcpy(settings.ssid, "replacement");
  retrySavedConnection = false; saveAfterConnect = true;
  beginConnection(); now += 30000; updateConnection();
  assert(connectionState == ConnectionState::Failed && stored == original && !saveAfterConnect);
  saveAfterConnect = true; beginConnection();
  WiFi.statusCode = WL_CONNECTED; updateConnection();
  now += 20000; updateConnection();
  assert(connectionState == ConnectionState::Failed && stored == original && !configured);

  // Failed flash commit never reports a saved/ready configuration.
  saveAfterConnect = true; beginConnection();
  WiFi.statusCode = WL_CONNECTED; updateConnection();
  writeOk = false; timeComplete = true; updateConnection();
  assert(!configured && connectionState == ConnectionState::Failed && stored == original);
  assert(Serial.logs.back() == "FAIL CONFIG_STORAGE"); writeOk = true;

  // Successful provisioning commits once, and the replacement loads on reboot.
  saveAfterConnect = true; beginConnection();
  WiFi.statusCode = WL_CONNECTED; updateConnection();
  const unsigned beforeSave = writes;
  timeComplete = true; updateConnection(); updateConnection();
  assert(configured && writes == beforeSave + 1 && !running);
  assert(restored.load() == 1 && !strcmp(restored.ssid, "replacement"));
  assert(Serial.logs[Serial.logs.size() - 2] == "OK CONFIG_SAVED");

  // A live conversation owns Wi-Fi until its tasks have stopped.
  running = true; WiFi.statusCode = 0; updateConnection();
  assert(connectionState == ConnectionState::Ready);
  running = false; updateConnection(); assert(connectionState == ConnectionState::Retry);
  now += 30000; updateConnection(); assert(connectionState == ConnectionState::Wifi && !running);

  // Unsigned timeout arithmetic works across millis() rollover.
  now = UINT32_MAX - 100; beginConnection(); now += 30000; updateConnection();
  assert(connectionState == ConnectionState::Retry);
  now += 30000; updateConnection(); WiFi.statusCode = WL_CONNECTED; updateConnection();
  now += 20000; updateConnection(); assert(connectionState == ConnectionState::Retry);
  for (const auto &line : Serial.logs) {
    assert(line.find("test-password") == std::string::npos);
    assert(line.find(first.token) == std::string::npos);
  }
}
"""
        harness = harness.replace("// DECLARATIONS", declarations).replace("// FUNCTIONS", functions)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "connection.cpp"
            binary = Path(directory) / "connection"
            (Path(directory) / "Preferences.h").write_text(preferences)
            source.write_text(harness)
            subprocess.run(["c++", "-std=c++17", "-I", directory, "-I", str(root / "live_chat"),
                            str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_display_preserves_errors_and_bounds_public_logs(self):
        header = (Path(__file__).parent / "live_chat/status_display.h").read_text()
        model = header[header.index("struct DisplayState {"):header.index("namespace statusDisplay {")]
        harness = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
// MODEL
int main() {
  DisplayState view;
  view.record("CONNECTING"); assert(!strcmp(view.title, "WIFI..."));
  view.record("OK WIFI"); assert(!strcmp(view.title, "TIME SYNC"));
  view.record("LIVE_READY"); assert(!strcmp(view.title, "READY"));
  const auto revision = view.revision;
  view.record("AVS3R_LIVE_READY 2");
  view.record("arbitrary private server text");
  assert(view.revision == revision);
  view.record("LIVE_STARTING"); view.record("LIVE_LISTENING");
  assert(!strcmp(view.title, "LIVE"));
  view.record("LIVE_STOPPING"); assert(!strcmp(view.title, "STOPPING"));
  view.record("FAIL PLAYBACK_BACKPRESSURE");
  view.record("LIVE_READY");
  assert(!strcmp(view.title, "ERROR") && view.failed);
  // The complete error fits in two bounded rows, rather than losing its suffix.
  char error[43];
  snprintf(error, sizeof(error), "%s%s", view.lines[0], view.lines[1]);
  assert(!strcmp(error, "FAIL PLAYBACK_BACKPRESSURE"));
  for (const auto &line : view.lines) assert(strlen(line) <= 21);
  view.record("LIVE_STARTING"); assert(!view.failed);
  view.record("FAIL WIFI"); view.record("WIFI_RETRY_WAIT");
  assert(!strcmp(view.title, "WIFI WAIT"));
  view.record("CONNECTING"); view.record("OK WIFI"); view.record("LIVE_READY");
  assert(!view.failed && !strcmp(view.title, "READY"));
  view.record("LIVE_CONFIG_REQUIRED"); assert(!strcmp(view.title, "SETUP"));
}
""".replace("// MODEL", model)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "display.cpp"
            binary = Path(directory) / "display"
            source.write_text(harness)
            subprocess.run(["c++", "-std=c++17", str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
