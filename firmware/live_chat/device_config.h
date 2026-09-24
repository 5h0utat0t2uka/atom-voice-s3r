#pragma once

#include <Preferences.h>
#include <cstdint>
#include <cstring>

// One versioned NVS value keeps credentials and their destination together.
// NVS is not encrypted by this firmware; never store the OpenAI API key here.
struct DeviceConfig {
  uint32_t version;
  char ssid[33], password[64], host[256], token[65];

  bool valid() const {
    if (version != 1 || !ssid[0] || strnlen(ssid, sizeof(ssid)) > 32
        || strnlen(password, sizeof(password)) < 8 || strnlen(password, sizeof(password)) > 63
        || !host[0] || strnlen(host, sizeof(host)) >= sizeof(host)
        || strnlen(token, sizeof(token)) != 64) return false;
    for (const char *p = password; *p; ++p) if (*p < 32 || *p > 126) return false;
    for (const char *p = host; *p; ++p) {
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
            || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) return false;
    }
    for (const char *p = token; *p; ++p) {
      if (!((*p >= 'a' && *p <= 'f') || (*p >= '0' && *p <= '9'))) return false;
    }
    return true;
  }

  // 0: first use, 1: valid saved settings, -1: unreadable/invalid settings.
  int load() {
    Preferences storage;
    if (!storage.begin("avs3r-live")) return -1;
    int result = 0;
    if (storage.isKey("config")) {
      result = storage.getBytesLength("config") == sizeof(*this)
          && storage.getBytes("config", this, sizeof(*this)) == sizeof(*this)
          && valid() ? 1 : -1;
    }
    storage.end();
    return result;
  }

  bool save() const {
    if (!valid()) return false;
    Preferences storage;
    if (!storage.begin("avs3r-live")) return false;
    const bool saved = storage.putBytes("config", this, sizeof(*this)) == sizeof(*this);
    storage.end();
    return saved;
  }
};
