#pragma once

#include <M5UnitGLASS2.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "terminal_font.h"

// Only fixed application events enter this view; USB input and server text do not.
struct DisplayState {
  const char *title = "BOOT";
  const char *hint = "Please wait";
  char lines[3][22] = {};
  bool failed = false;
  uint32_t revision = 1;

  void append(const char *text) {
    // 21 ASCII columns at 6 px; wrap once so long error codes remain readable.
    for (size_t offset = 0; text[offset] && offset < 42; offset += 21) {
      memcpy(lines[0], lines[1], sizeof(lines[0]));
      memcpy(lines[1], lines[2], sizeof(lines[1]));
      snprintf(lines[2], sizeof(lines[2]), "%.21s", text + offset);
      if (strlen(text + offset) <= 21) break;
    }
    ++revision;
  }

  void record(const char *event) {
    if (!strcmp(event, "CONFIG_ACCEPTED")) { failed = false; return; }
    if (!strcmp(event, "CONNECTING")) {
      failed = false; title = "WIFI..."; hint = "Connecting";
    } else if (!strcmp(event, "OK WIFI")) {
      title = "TIME SYNC"; hint = "Please wait";
    } else if (!strcmp(event, "LIVE_READY")) {
      if (!failed) { title = "READY"; hint = "Press to talk"; }
    } else if (!strcmp(event, "LIVE_STARTING")) {
      failed = false; title = "STARTING"; hint = "Press to cancel";
    } else if (!strcmp(event, "LIVE_LISTENING")) {
      title = "LIVE"; hint = "Talk / press to stop";
    } else if (!strcmp(event, "LIVE_STOPPING")) {
      if (!failed) { title = "STOPPING"; hint = "Please wait"; }
    } else if (!strcmp(event, "WIFI_RETRY_WAIT")) {
      title = "WIFI WAIT"; hint = "Retry in 30 sec";
    } else if (!strcmp(event, "LIVE_CONFIG_REQUIRED")) {
      title = "SETUP"; hint = "USB: live-connect";
    } else if (!strncmp(event, "FAIL ", 5)) {
      failed = true; title = "ERROR"; hint = "See log below";
    } else if (strcmp(event, "CONFIG_LOADED") && strcmp(event, "OK TIME")
        && strcmp(event, "OK CONFIG_SAVED") && strcmp(event, "LIVE_SESSION_CLOSED")
        && strcmp(event, "LIVE_USAGE_UNCONFIRMED")
        && strncmp(event, "LIVE_WS_HTTP ", 13) && strncmp(event, "LIVE_USAGE_SECONDS ", 19)) {
      return;
    }
    append(!strncmp(event, "LIVE_", 5) ? event + 5 : event);
  }
};

namespace statusDisplay {
// ES8311 uses Wire / I2C0 on GPIO45/0. Glass2 owns I2C1 on the Grove GPIO2/1.
M5UnitGLASS2 screen(2, 1, 400000, 1, 0x3C);
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
DisplayState state;

void record(const char *event) {
  portENTER_CRITICAL(&mutex);
  state.record(event);
  portEXIT_CRITICAL(&mutex);
}

void task(void *) {
  // M5GFX init checks for a panel response. Try both documented addresses.
  int address = 0;
  for (int candidate : {0x3C, 0x3D}) {
    if (screen.init(2, 1, 400000, 1, candidate)) { address = candidate; break; }
  }
  if (!address) {
    Serial.println("DISPLAY_NOT_FOUND");
    vTaskDelete(nullptr);
    return;
  }
  Serial.printf("DISPLAY_READY 0x%02X\n", address);
  screen.setTextWrap(false);
  screen.setFont(&terminal6x10);
  screen.setTextSize(1);
  screen.setTextColor(0xFFFF, 0);
  uint32_t drawn = 0;
  for (;;) {
    DisplayState snapshot;
    portENTER_CRITICAL(&mutex);
    snapshot = state;
    portEXIT_CRITICAL(&mutex);
    if (snapshot.revision != drawn) {
      // Six 10-pixel rows fit the 64-pixel panel, including one blank row.
      screen.startWrite();
      screen.fillScreen(0);
      screen.setCursor(0, 0);
      screen.print(snapshot.title);
      screen.setCursor(0, 10);
      screen.print(snapshot.hint);
      for (int row = 0; row < 3; ++row) {
        screen.setCursor(0, 30 + row * 10);
        screen.print(snapshot.lines[row]);
      }
      screen.endWrite();
      drawn = snapshot.revision;
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

void begin() {
  // Lower priority than networking; all display I/O stays off the audio core.
  if (xTaskCreatePinnedToCore(task, "glass2", 4096, nullptr, 1, nullptr, 0) != pdPASS) {
    Serial.println("DISPLAY_TASK_FAILED");
  }
}
} // namespace statusDisplay

void liveLog(const char *line) {
  statusDisplay::record(line);
  Serial.println(line);
}

void liveLogf(const char *format, ...) {
  char line[192];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  line[strcspn(line, "\r\n")] = 0;
  liveLog(line);
}
