#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <cmath>
#include <cstring>

constexpr uint32_t sampleRate = 24000;
constexpr size_t chunkSamples = sampleRate / 50;  // 20 ms
constexpr size_t maxSamples = sampleRate * 10;   // 10 seconds / 480 KB
constexpr uint32_t warmupMs = 1200;
constexpr uint32_t captureTimeoutMs = 1000;
constexpr uint32_t minimumPressMs = 150;

enum class State { Warming, Ready, Starting, Recording, Draining, Playing, Error };
State state = State::Error;
int16_t* audio = nullptr;
int16_t discard[chunkSamples];
size_t queuedSamples = 0;
uint32_t stateSince = 0;
uint32_t lastCaptureProgress = 0;
uint32_t pressedAt = 0;
uint32_t releasedAfterMs = 0;
uint32_t captureStartedAt = 0;
uint32_t captureElapsedMs = 0;
uint32_t captureQueueEmpty = 0;
bool exportNextRecording = false;
uint32_t exportArmedAt = 0;
size_t captureDmaFrames = 128;
const char* failure = "Not initialized";
constexpr const char* readyMessage =
    "Ready: hold the front button to record (max 10 s), or send t for a PCM speaker test.";
uint8_t codecFormatBefore = 0;
uint8_t codecFormatAfter = 0;
constexpr uint8_t codecRegisters[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                     0x06, 0x07, 0x08, 0x0A, 0x14, 0x16,
                                     0x17, 0x18, 0x19, 0x1C};
uint8_t codecValues[sizeof(codecRegisters)];

void reportCodec() {
  Serial.print("ES8311 capture registers:");
  for (size_t i = 0; i < sizeof(codecRegisters); ++i) {
    Serial.printf(" %02X=%02X", codecRegisters[i], codecValues[i]);
  }
  Serial.println();
}

bool configureMicFormat() {
  // VoiceS3R's ES8311 is on I2C1 SDA45/SCL0, as in M5Unified's board callback.
  // ES8311 User Guide: 0x0A bits 4:2 = 011 selects 16-bit output.
  // M5Unified 0.2.23 configures 16-bit I2S slots but leaves this register alone.
  constexpr uint8_t codecAddress = 0x18;
  constexpr uint8_t outputFormatRegister = 0x0A;
  constexpr uint8_t wordLengthMask = 0x1C;
  constexpr uint8_t wordLength16 = 0x0C;
  m5gfx::i2c::i2c_temporary_switcher_t bus(1, GPIO_NUM_45, GPIO_NUM_0);
  bool ok = M5.In_I2C.readRegister(codecAddress, outputFormatRegister,
                                 &codecFormatBefore, 1, 100000);
  if (ok) {
    const uint8_t desired = (codecFormatBefore & ~wordLengthMask) | wordLength16;
    ok = M5.In_I2C.writeRegister8(codecAddress, outputFormatRegister, desired, 100000)
        && M5.In_I2C.readRegister(codecAddress, outputFormatRegister,
                                 &codecFormatAfter, 1, 100000)
        && codecFormatAfter == desired;
  }
  for (size_t i = 0; ok && i < sizeof(codecRegisters); ++i) {
    ok = M5.In_I2C.readRegister(codecAddress, codecRegisters[i],
                               &codecValues[i], 1, 100000);
  }
  bus.restore();
  return ok;
}

void clearAudio() {
  if (audio) {
    memset(audio, 0, maxSamples * sizeof(int16_t));
  }
  memset(discard, 0, sizeof(discard));
  queuedSamples = 0;
  captureStartedAt = captureElapsedMs = captureQueueEmpty = 0;
}

void fail(const char* message) {
  // Stop both consumers before clearing memory they may still access.
  M5.Mic.end();
  M5.Speaker.end();
  clearAudio();
  failure = message;
  state = State::Error;
  Serial.printf("ERROR: %s; reset to retry.\n", failure);
}

void prepareMic() {
  M5.Speaker.end();
  M5.Mic.end();
  clearAudio();
  auto config = M5.Mic.config();
  config.sample_rate = sampleRate;
  config.dma_buf_len = captureDmaFrames;
  // Preserve the board's I2S input channel configuration (stereo on VoiceS3R).
  // record(..., false) independently requests mono output samples.
  M5.Mic.config(config);
  if (!M5.Mic.begin()) {
    fail("Microphone initialization failed");
    return;
  }
  // Apply after the library's codec initialization, before warm-up/capture.
  if (!configureMicFormat()) {
    fail("ES8311 microphone format write/readback failed");
    return;
  }
  Serial.printf("ES8311 ADC format: 0x%02X -> 0x%02X (16-bit output verified)\n",
                codecFormatBefore, codecFormatAfter);
  reportCodec();
  Serial.printf("Capture DMA: frames=%u, buffers=%u\n",
                static_cast<unsigned>(config.dma_buf_len),
                static_cast<unsigned>(config.dma_buf_count));
  state = State::Warming;
  stateSince = lastCaptureProgress = millis();
  Serial.println("Warming microphone...");
}

void reportRecordingLevels() {
  int32_t peak = 0;
  int32_t minimum = 32767, maximum = -32768;
  double sumSquares = 0;
  size_t nearLimit = 0;
  for (size_t i = 0; i < queuedSamples; ++i) {
    const int32_t sample = audio[i];
    const int32_t magnitude = sample < 0 ? -sample : sample;
    if (magnitude > peak) peak = magnitude;
    if (sample < minimum) minimum = sample;
    if (sample > maximum) maximum = sample;
    sumSquares += static_cast<double>(sample) * sample;
    // M5Unified clamps PCM16 at INT16_MIN+16 / INT16_MAX-16.
    if (sample <= -32752 || sample >= 32751) ++nearLimit;
  }
  Serial.printf("PCM: peak=%ld/32768, at_capture_limit=%u/%u (%.2f%%)\n",
                static_cast<long>(peak), static_cast<unsigned>(nearLimit),
                static_cast<unsigned>(queuedSamples),
                queuedSamples ? 100.0 * nearLimit / queuedSamples : 0.0);
  Serial.printf("Capture: wall_ms=%u, pcm_ms=%u, queue_empty=%u, min=%ld, max=%ld, rms=%.1f\n",
                static_cast<unsigned>(captureElapsedMs),
                static_cast<unsigned>(queuedSamples * 1000 / sampleRate),
                static_cast<unsigned>(captureQueueEmpty),
                static_cast<long>(minimum), static_cast<long>(maximum),
                queuedSamples ? std::sqrt(sumSquares / queuedSamples) : 0.0);
}

bool exportRecording() {
  // The capture task has stopped. Transfer exactly the buffer used by playRaw.
  const auto* bytes = reinterpret_cast<const uint8_t*>(audio);
  const size_t length = queuedSamples * sizeof(int16_t);
  uint32_t checksum = 2166136261u;  // FNV-1a: detect truncated/corrupt USB transfers.
  for (size_t i = 0; i < length; ++i) checksum = (checksum ^ bytes[i]) * 16777619u;
  Serial.printf("PCM_BEGIN %u %u 1 %08lX\n", static_cast<unsigned>(length),
                static_cast<unsigned>(sampleRate), static_cast<unsigned long>(checksum));
  size_t sent = 0;
  const uint32_t started = millis();
  while (sent < length && Serial && millis() - started < 15000) {
    const size_t remaining = length - sent;
    sent += Serial.write(bytes + sent, remaining < 512 ? remaining : 512);
    delay(1);
  }
  if (sent != length) return false;
  Serial.print("\nPCM_END\n");
  return true;
}

void startPlayback() {
  if (!M5.Speaker.begin()
      || !M5.Speaker.playRaw(audio, queuedSamples, sampleRate, false, 1, 0)) {
    fail("Playback initialization failed");
    return;
  }
  state = State::Playing;
  stateSince = millis();
  Serial.printf("Playing %u ms...\n", static_cast<unsigned>(queuedSamples * 1000 / sampleRate));
}

void playReference() {
  // Stop the outstanding idle capture before reusing either buffer.
  M5.Mic.end();
  M5.Speaker.end();
  clearAudio();
  queuedSamples = sampleRate * 2;
  constexpr size_t fadeSamples = sampleRate / 50;
  constexpr float twoPi = 6.28318530718f;
  for (size_t i = 0; i < queuedSamples; ++i) {
    float envelope = 1.0f;
    if (i < fadeSamples) envelope = static_cast<float>(i) / fadeSamples;
    else if (i >= queuedSamples - fadeSamples) {
      envelope = static_cast<float>(queuedSamples - 1 - i) / fadeSamples;
    }
    // Exactly 24 samples per cycle; amplitude comparable to the reported voice.
    audio[i] = static_cast<int16_t>(5000.0f * envelope
        * std::sin(twoPi * static_cast<float>(i % 24) / 24.0f));
  }
  Serial.println("Reference: 1000 Hz sine, 2000 ms, PCM peak=5000; microphone bypassed.");
  startPlayback();
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(100);
  auto config = M5.config();
  M5.begin(config);
  M5.Speaker.end();
  M5.Mic.end();
  audio = static_cast<int16_t*>(heap_caps_malloc(
      maxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!audio) {
    fail("Cannot allocate recording buffer in PSRAM");
    return;
  }
  M5.Speaker.setVolume(64);
  prepareMic();
}

void loop() {
  M5.update();
  const uint32_t now = millis();
  static bool monitorConnected = false;
  const bool connected = static_cast<bool>(Serial);
  if (connected && !monitorConnected) {
    Serial.printf("Atom VoiceS3R: Flash=%u, PSRAM=%u bytes; PCM16 mono 24000 Hz\n",
                  ESP.getFlashChipSize(), ESP.getPsramSize());
    // Configuration reads are made by this loop, which also owns begin/end.
    const auto micConfig = M5.Mic.config();
    Serial.printf("Mic: I2S stereo=%u, magnification=%u, oversampling=%u\n",
                  static_cast<unsigned>(micConfig.stereo),
                  static_cast<unsigned>(micConfig.magnification),
                  static_cast<unsigned>(micConfig.over_sampling));
    if (state != State::Error) {
      Serial.printf("ES8311 ADC format: 0x%02X -> 0x%02X (last microphone setup)\n",
                    codecFormatBefore, codecFormatAfter);
      reportCodec();
    }
    const auto speakerConfig = M5.Speaker.config();
    Serial.printf("Speaker: I2S rate=%u, stereo=%u, magnification=%u, volume=%u\n",
                  static_cast<unsigned>(speakerConfig.sample_rate),
                  static_cast<unsigned>(speakerConfig.stereo),
                  static_cast<unsigned>(speakerConfig.magnification),
                  static_cast<unsigned>(M5.Speaker.getVolume()));
    if (state == State::Ready) Serial.println(readyMessage);
    else if (state == State::Error) Serial.printf("ERROR: %s; reset to retry.\n", failure);
    else Serial.println("Audio busy; wait for Ready.");
  }
  monitorConnected = connected;

  if (exportNextRecording && (!connected || now - exportArmedAt > 60000)) {
    exportNextRecording = false;
    Serial.println("EXPORT CANCELLED");
  }
  if (Serial.available()) {
    const int command = Serial.read();
    if (command == '?') {
      if (state == State::Ready) {
        reportCodec();
        Serial.println(readyMessage);
      } else if (state == State::Error) Serial.printf("ERROR: %s\n", failure);
      else Serial.println("Audio busy; wait for Ready.");
    } else if ((command == '1' || command == '2') && state == State::Ready
               && !M5.BtnA.isPressed()) {
      // Controlled A/B experiment: only the DMA frame count changes.
      exportNextRecording = false;
      captureDmaFrames = command == '1' ? 128 : 256;
      prepareMic();
      if (state != State::Error) {
        Serial.printf("DMA CONFIGURED %u\n", static_cast<unsigned>(captureDmaFrames));
      }
      return; // prepareMic changed stateSince; do not use the earlier 'now'.
    } else if (command == 'd' && state == State::Ready && !M5.BtnA.isPressed()) {
      exportNextRecording = true;
      exportArmedAt = now;
      Serial.println("EXPORT ARMED: hold the button, then release within 10 s. Expires in 60 s.");
    } else if (command == 'c') {
      exportNextRecording = false;
      Serial.println("EXPORT CANCELLED");
    }
    if (command == 't') {
      if (state == State::Ready && !M5.BtnA.isPressed()) {
        playReference();
        return;
      }
      Serial.println("Speaker test ignored; release the button and wait for Ready.");
    }
  }

  switch (state) {
    case State::Warming:
    case State::Ready:
      if (state == State::Ready && M5.BtnA.wasPressed()) {
        pressedAt = stateSince = now;
        state = State::Starting;
        break;
      }
      // Drain idle audio into scratch RAM; never append it to the recording.
      if (!M5.Mic.isRecording()) {
        if (state == State::Warming && now - stateSince >= warmupMs
            && !M5.BtnA.isPressed()) {
          state = State::Ready;
          Serial.println(readyMessage);
        }
        memset(discard, 0, sizeof(discard));
        if (!M5.Mic.record(discard, chunkSamples, sampleRate, false)) {
          fail("Idle microphone capture failed");
          break;
        }
        lastCaptureProgress = now;
      } else if (now - lastCaptureProgress > captureTimeoutMs) {
        fail("Microphone capture timed out");
      }
      break;

    case State::Starting:
      // Let the outstanding idle chunk finish before changing destinations.
      if (!M5.Mic.isRecording()) {
        memset(discard, 0, sizeof(discard));
        if (!M5.BtnA.isPressed()) {
          state = State::Ready;
          break;
        }
        state = State::Recording;
        stateSince = lastCaptureProgress = now;
        Serial.println("Recording...");
      } else if (now - stateSince > captureTimeoutMs) {
        fail("Microphone did not finish idle capture");
      }
      break;

    case State::Recording:
      if (!M5.BtnA.isPressed() || queuedSamples == maxSamples) {
        releasedAfterMs = now - pressedAt;
        state = State::Draining;
        stateSince = now;
        Serial.println(queuedSamples == maxSamples ? "10 s limit reached." : "Button released.");
        break;
      }
      // Two contiguous requests keep capture running while the loop polls the button.
      if (M5.Mic.isRecording() < 2) {
        if (queuedSamples == 0) captureStartedAt = millis();
        else if (!M5.Mic.isRecording()) ++captureQueueEmpty;
        if (!M5.Mic.record(audio + queuedSamples, chunkSamples, sampleRate, false)) {
          fail("Recording request failed");
          break;
        }
        queuedSamples += chunkSamples;
        lastCaptureProgress = now;
      } else if (now - lastCaptureProgress > captureTimeoutMs) {
        fail("Recording timed out");
      }
      break;

    case State::Draining:
      // record() is asynchronous. Do not read or play pending samples.
      if (M5.Mic.isRecording()) {
        if (now - stateSince > captureTimeoutMs) fail("Final capture timed out");
        break;
      }
      captureElapsedMs = millis() - captureStartedAt;
      M5.Mic.end();
      if (releasedAfterMs < minimumPressMs || queuedSamples == 0) {
        Serial.println("Short press discarded.");
        prepareMic();
        break;
      }
      reportRecordingLevels();
      if (exportNextRecording) {
        exportNextRecording = false;
        if (!exportRecording()) Serial.println("\nEXPORT FAILED");
      }
      startPlayback();
      break;

    case State::Playing:
      if (!M5.Speaker.isPlaying()) {
        // end() joins the playback task before the recording buffer is cleared.
        Serial.println("Playback finished.");
        prepareMic();
      } else if (now - stateSince > 12000) {
        fail("Playback timed out");
      }
      break;

    case State::Error:
      break;
  }
  M5.delay(1);
}
