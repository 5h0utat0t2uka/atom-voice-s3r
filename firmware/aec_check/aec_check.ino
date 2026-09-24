#include <Arduino.h>
#include <Wire.h>
#include <atomic>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_aec.h>
#include "generated_audio.h"

// Offline acoustic echo cancellation experiment. No M5Unified audio drivers.
// Wiring: https://docs.m5stack.com/en/core/Atom_EchoS3R
#include "audio_hardware.h"
constexpr size_t speechFrames = sizeof(speechAudio) / sizeof(speechAudio[0]);
constexpr size_t speechBlocks = (speechFrames + framesPerBlock - 1) / framesPerBlock;
constexpr size_t quietBlocks = 200;
constexpr size_t firstSpeechBlock = quietBlocks;
constexpr size_t secondSpeechBlock = quietBlocks + speechBlocks + quietBlocks;
constexpr size_t nominalCaptureBlocks = secondSpeechBlock + speechBlocks + quietBlocks;
constexpr size_t maxCaptureFrames = rate * 27;
// Three channels: raw microphone, delayed playback reference, AEC output.
constexpr size_t captureBytes = maxCaptureFrames * 6;
static_assert(speechFrames > 0 && speechFrames <= rate * 10, "Speech must fit within 10 seconds");

int16_t *recording = nullptr;
std::atomic<bool> completed{false};
std::atomic<bool> cancelled{false};
bool running = false;
bool exportRequested = false;
const char *failure = nullptr;
uint32_t elapsedMs = 0;
uint32_t maxBlockUs = 0;
uint32_t maxAecUs = 0;
uint32_t aecCalls = 0;
uint64_t aecTotalUs = 0;
size_t warmBlocks = 0;
size_t captureBlocks = 0;
size_t aecFrames = 0;
size_t captured = 0;

void audioTask(void *) {
  aec_handle_t *aec = aec_create(rate, 4, 1, AEC_MODE_FD_HIGH_PERF);
  int16_t *aecMic = nullptr, *aecReference = nullptr, *aecOutput = nullptr;
  if (!aec) failure = "AEC_CREATE";
  if (!failure) {
    aec_set_nlp_level(aec, AEC_NLP_LEVEL_NORMAL);
    const int chunk = aec_get_chunksize(aec);
    if (chunk <= 0 || chunk > 2048) failure = "AEC_CHUNK_SIZE";
    else {
      aecFrames = size_t(chunk);
      // Round both periods to a multiple of I2S and AEC frame sizes.
      size_t boundary = framesPerBlock;
      while (boundary % aecFrames) boundary += framesPerBlock;
      warmBlocks = ((rate * 12 / 10 + boundary - 1) / boundary) * boundary / framesPerBlock;
      captureBlocks = ((nominalCaptureBlocks * framesPerBlock + boundary - 1) / boundary) * boundary / framesPerBlock;
      if (captureBlocks * framesPerBlock > maxCaptureFrames) failure = "CAPTURE_SIZE";
      aecMic = static_cast<int16_t *>(heap_caps_aligned_calloc(16, aecFrames, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      aecReference = static_cast<int16_t *>(heap_caps_aligned_calloc(16, aecFrames, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      aecOutput = static_cast<int16_t *>(heap_caps_aligned_calloc(16, aecFrames, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      if (!aecMic || !aecReference || !aecOutput) failure = "AEC_BUFFERS";
    }
  }
  if (!failure && !initAudio()) failure = "I2S_INIT";
  if (!failure && !initCodec()) failure = "CODEC_INIT_OR_READBACK";
  if (!failure) {
    Serial.printf("AEC_CONFIG rate=%u chunk=%u warmup_ms=%u first_ms=%u second_ms=%u speech_ms=%u reference_delay_ms=%u\n",
                  unsigned(rate), unsigned(aecFrames), unsigned(warmBlocks * 10),
                  unsigned(firstSpeechBlock * 10 + dmaBlocks * 10),
                  unsigned(secondSpeechBlock * 10 + dmaBlocks * 10),
                  unsigned(speechBlocks * 10), unsigned(dmaBlocks * 10));
    if (i2s_channel_enable(rx) != ESP_OK) failure = "I2S_RX_ENABLE";
    else rxEnabled = true;
    if (!failure) {
      if (i2s_channel_enable(tx) != ESP_OK) failure = "I2S_TX_ENABLE";
      else txEnabled = true;
    }
  }
  if (!failure) {
    digitalWrite(amplifierPin, HIGH);
    // Both DMA timelines start with silence. A completed TX descriptor is
    // refilled for playback one complete DMA ring later. Delay the software
    // reference by that ring, not by an empirically guessed acoustic delay.
    int16_t referenceHistory[dmaBlocks][framesPerBlock] = {};
    int16_t output[framesPerBlock * 2] = {};
    int16_t input[framesPerBlock * 2];
    size_t filled = 0;
    size_t processedFrames = 0;
    uint32_t started = 0;
    for (size_t block = 0; block < warmBlocks + captureBlocks; ++block) {
      if (cancelled.load()) { failure = "CANCELLED"; break; }
      if (block == warmBlocks) {
        started = millis();
        portENTER_CRITICAL(&counterMux);
        rxOverflows = 0;
        portEXIT_CRITICAL(&counterMux);
      }
      for (size_t frame = 0; frame < framesPerBlock; ++frame) {
        const int32_t position = int32_t(block * framesPerBlock + frame) - int32_t(warmBlocks * framesPerBlock);
        const int32_t first = position - int32_t(firstSpeechBlock * framesPerBlock);
        const int32_t second = position - int32_t(secondSpeechBlock * framesPerBlock);
        const int16_t sample = first >= 0 && size_t(first) < speechFrames ? speechAudio[first]
                               : second >= 0 && size_t(second) < speechFrames ? speechAudio[second] : 0;
        output[frame * 2] = output[frame * 2 + 1] = sample;
      }
      size_t written = 0, received = 0;
      const uint32_t before = micros();
      if (i2s_channel_write(tx, output, sizeof(output), &written, 250) != ESP_OK
          || written != sizeof(output)) { failure = "I2S_WRITE"; break; }
      if (i2s_channel_read(rx, input, sizeof(input), &received, 250) != ESP_OK
          || received != sizeof(input)) { failure = "I2S_READ"; break; }
      for (size_t frame = 0; frame < framesPerBlock; ++frame) {
        aecMic[filled] = input[frame * 2];
        aecReference[filled] = referenceHistory[block % dmaBlocks][frame];
        if (block >= warmBlocks) {
          const size_t index = (block - warmBlocks) * framesPerBlock + frame;
          recording[index * 3] = aecMic[filled];
          recording[index * 3 + 1] = aecReference[filled];
        }
        referenceHistory[block % dmaBlocks][frame] = output[frame * 2];
        if (++filled == aecFrames) {
          const uint32_t aecStarted = micros();
          aec_process(aec, aecMic, aecReference, aecOutput);
          const uint32_t aecUs = micros() - aecStarted;
          if (aecUs > maxAecUs) maxAecUs = aecUs;
          aecTotalUs += aecUs;
          ++aecCalls;
          for (size_t i = 0; i < aecFrames; ++i) {
            const int32_t index = int32_t(processedFrames + i) - int32_t(warmBlocks * framesPerBlock);
            if (index >= 0) {
              recording[size_t(index) * 3 + 2] = aecOutput[i];
              captured += 6;
            }
          }
          processedFrames += aecFrames;
          filled = 0;
        }
      }
      if (block >= warmBlocks) {
        const uint32_t duration = micros() - before;
        if (duration > maxBlockUs) maxBlockUs = duration;
        elapsedMs = millis() - started;
      }
    }
  }
  stopAudio();
  if (aec) aec_destroy(aec);
  for (auto *buffer : {aecMic, aecReference, aecOutput}) {
    if (buffer) { memset(buffer, 0, aecFrames * 2); free(buffer); }
  }
  completed.store(true);
  vTaskDelete(nullptr);
}

void ready() {
  Serial.println("AEC_READY: button/p = local test; d = test and USB export; c = cancel.");
}

void startTest(bool exportAudio) {
  recording = static_cast<int16_t *>(heap_caps_malloc(captureBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!recording) { Serial.println("ERROR: PSRAM_ALLOC"); return; }
  running = true;
  exportRequested = exportAudio;
  failure = nullptr;
  captured = elapsedMs = maxBlockUs = maxAecUs = aecCalls = aecTotalUs = 0;
  warmBlocks = captureBlocks = aecFrames = 0;
  cancelled.store(false);
  completed.store(false);
  Serial.println("AEC_RECORDING: stay quiet for the FIRST playback; speak over the SECOND playback.");
  if (xTaskCreatePinnedToCore(audioTask, "aec", 12288, nullptr, 4, nullptr, 1) != pdPASS) {
    failure = "TASK_CREATE";
    completed.store(true);
  }
}

void exportAudio() {
  const auto *bytes = reinterpret_cast<const uint8_t *>(recording);
  uint32_t digest = 2166136261u;
  for (size_t i = 0; i < captured; ++i) digest = (digest ^ bytes[i]) * 16777619u;
  Serial.printf("AEC_PCM_BEGIN %u %u 3 %08lX\n", unsigned(captured), unsigned(rate),
                static_cast<unsigned long>(digest));
  size_t sent = 0;
  const uint32_t started = millis();
  while (sent < captured && Serial && millis() - started < 15000) {
    const size_t remaining = captured - sent;
    sent += Serial.write(bytes + sent, remaining < 512 ? remaining : 512);
    delay(1);
  }
  Serial.print("\nAEC_PCM_END\n");
}

void finishTest() {
  uint32_t overflows;
  portENTER_CRITICAL(&counterMux);
  overflows = rxOverflows;
  portEXIT_CRITICAL(&counterMux);
  Serial.printf("AEC_STATS frames=%u expected=%u wall_ms=%u pcm_ms=%u rx_overflows=%u max_block_us=%u max_aec_us=%u mean_aec_us=%u\n",
                unsigned(captured / 6), unsigned(captureBlocks * framesPerBlock),
                unsigned(elapsedMs), unsigned(captured / 6 * 1000 / rate), unsigned(overflows), unsigned(maxBlockUs),
                unsigned(maxAecUs), unsigned(aecCalls ? aecTotalUs / aecCalls : 0));
  if (failure) Serial.printf("ERROR: %s\n", failure);
  else {
    // This reports software transport completion, not an acoustic quality verdict.
    Serial.println(overflows ? "AEC_WARNING: RX queue overflow; recording may contain gaps." : "AEC_IO_DONE");
    if (exportRequested && Serial) exportAudio();
  }
  memset(recording, 0, captureBytes);
  free(recording);
  recording = nullptr;
  running = false;
  ready();
}

void setup() {
  pinMode(amplifierPin, OUTPUT);
  digitalWrite(amplifierPin, LOW);
  pinMode(buttonPin, INPUT_PULLUP);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(100);
}

void loop() {
  static bool connectedBefore = false;
  static bool buttonBefore = false;
  static bool buttonRaw = false;
  static uint32_t buttonChanged = 0;
  const bool connected = static_cast<bool>(Serial);
  if (connected && !connectedBefore) {
    Serial.println("Atom VoiceS3R AEC check v1: offline, PCM16 16000 Hz, MCLK=4096000 Hz.");
    if (!running) ready();
  }
  connectedBefore = connected;
  const bool pressed = digitalRead(buttonPin) == LOW;
  if (pressed != buttonRaw) { buttonRaw = pressed; buttonChanged = millis(); }
  if (millis() - buttonChanged >= 30 && pressed != buttonBefore) {
    buttonBefore = pressed;
    if (pressed && !running) startTest(false);
  }
  if (Serial.available()) {
    const int command = Serial.read();
    if (command == 'c') cancelled.store(true);
    if (command == '?' && !running) ready();
    if ((command == 'p' || command == 'd') && !running) startTest(command == 'd');
  }
  if (running && completed.load()) finishTest();
  delay(1);
}
