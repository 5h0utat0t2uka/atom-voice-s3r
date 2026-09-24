#include <M5Unified.h>
#include "generated_audio.h"

constexpr uint32_t sampleRate = 24000;
constexpr size_t sampleCount = sizeof(speechAudio) / sizeof(speechAudio[0]);
// Clock the codec with silence before sending the first speech sample.
// 200 ms is a starting value to verify on the device, not a specified hardware minimum.
constexpr uint32_t startupSilenceMs = 200;
constexpr size_t startupSampleCount = sampleRate * startupSilenceMs / 1000;
const int16_t startupSilence[startupSampleCount] = {};
constexpr uint32_t playbackTimeoutMs = startupSilenceMs + sampleCount * 1000 / sampleRate + 3000;
static_assert(sampleCount > 0 && sampleCount <= sampleRate * 10, "Speech must fit within 10 seconds");
bool playing = false;
bool failed = false;
uint32_t startedAt = 0;

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(100);
  auto config = M5.config();
  // M5Unified 0.2.23 also gates VoiceS3R speaker configuration on internal_mic.
  // Keep the default configuration, then stop the microphone before playback.
  M5.begin(config);
  M5.Mic.end();
  M5.Speaker.end();
  M5.Speaker.setVolume(64);
}

void loop() {
  M5.update();
  static bool wasConnected = false;
  const bool connected = static_cast<bool>(Serial);
  if (connected && !wasConnected) {
    Serial.printf("AI Japanese speech test: PCM16 mono %u Hz, %u ms, volume=%u\n",
                  static_cast<unsigned>(sampleRate),
                  static_cast<unsigned>(sampleCount * 1000 / sampleRate),
                  static_cast<unsigned>(M5.Speaker.getVolume()));
    Serial.println("Audio is AI-generated. Press the front button to play; send p to play over USB.");
  }
  wasConnected = connected;

  const bool serialPlay = Serial.available() && Serial.read() == 'p';
  if (!playing && !failed && (M5.BtnA.wasPressed() || serialPlay)) {
    // Queue both buffers on the same channel so I2S keeps running between them.
    if (!M5.Speaker.begin()
        || !M5.Speaker.playRaw(startupSilence, startupSampleCount, sampleRate, false, 1, 0)
        || !M5.Speaker.playRaw(speechAudio, sampleCount, sampleRate, false, 1, 0)) {
      M5.Speaker.end();
      failed = true;
      Serial.println("ERROR: playback initialization failed; reset to retry.");
    } else {
      startedAt = millis();
      playing = true;
      Serial.printf("Playing AI-generated Japanese speech (startup silence: %u ms)...\n",
                    static_cast<unsigned>(startupSilenceMs));
    }
  }
  if (playing) {
    if (!M5.Speaker.isPlaying()) {
      M5.Speaker.end();
      playing = false;
      Serial.println("Playback finished. Press the front button to replay.");
    } else if (millis() - startedAt > playbackTimeoutMs) {
      M5.Speaker.end();
      playing = false;
      failed = true;
      Serial.println("ERROR: playback timed out; reset to retry.");
    }
  }
  M5.delay(1);
}
