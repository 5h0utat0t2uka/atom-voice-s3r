#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s_std.h>

// Hardware-verified ES8311 / full-duplex I2S configuration.
constexpr uint32_t rate = 16000;
constexpr size_t framesPerBlock = 160;
constexpr size_t dmaBlocks = 6;
constexpr int amplifierPin = 18;
constexpr int buttonPin = 41;
i2s_chan_handle_t tx = nullptr, rx = nullptr;
bool txEnabled = false, rxEnabled = false, wireStarted = false;
portMUX_TYPE counterMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t rxOverflows = 0;

bool IRAM_ATTR receiveOverflow(i2s_chan_handle_t, i2s_event_data_t *, void *) {
  portENTER_CRITICAL_ISR(&counterMux);
  ++rxOverflows;
  portEXIT_CRITICAL_ISR(&counterMux);
  return false;
}

bool writeCodec(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(0x18);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readCodec(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(0x18);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(uint8_t(0x18), size_t(1)) != 1) return false;
  value = Wire.read();
  return true;
}

bool initCodec() {
  wireStarted = Wire.begin(45, 0, 100000);
  if (!wireStarted) return false;
  Wire.setTimeOut(100);
  if (!writeCodec(0x00, 0x1F)) return false;
  delay(20);
  // Fixed 16 kHz / MCLK 4.096 MHz / 16-bit Philips I2S, codec slave.
  // Clock coefficients and power-up order follow Espressif's ES8311 driver:
  // https://github.com/espressif/esp-bsp/blob/master/components/es8311/es8311.c
  // Both ADC and DAC clocks/power remain enabled throughout the recording.
  const uint8_t settings[][2] = {
    {0x00, 0x00}, {0x00, 0x80},
    {0x01, 0x3F},  // All clocks enabled, external MCLK (not BCLK).
    {0x02, 0x00}, {0x03, 0x10}, {0x04, 0x10}, {0x05, 0x00},
    {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xFF},
    {0x09, 0x0C}, {0x0A, 0x0C},  // DAC and ADC: 16-bit, left slot.
    {0x0D, 0x01}, {0x0E, 0x02}, {0x12, 0x00}, {0x13, 0x10},
    {0x1C, 0x6A}, {0x37, 0x08},
    {0x14, 0x1A},  // Analog microphone, PGA setting used by Espressif's driver.
    {0x16, 0x00}, {0x17, 0xBF},  // No additional ADC gain, digital 0 dB.
    {0x31, 0x00}, {0x32, 0xBF},  // DAC unmuted, digital 0 dB.
  };
  for (const auto &setting : settings) {
    if (!writeCodec(setting[0], setting[1])) return false;
  }
  const uint8_t verify[][2] = {{0x00, 0x80}, {0x01, 0x3F}, {0x09, 0x0C}, {0x0A, 0x0C},
                             {0x0D, 0x01}, {0x0E, 0x02}, {0x12, 0x00}, {0x17, 0xBF}, {0x32, 0xBF}};
  for (const auto &setting : verify) {
    uint8_t value = 0;
    if (!readCodec(setting[0], value) || value != setting[1]) return false;
  }
  return true;
}

bool initAudio() {
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  channel.dma_desc_num = dmaBlocks;
  channel.dma_frame_num = framesPerBlock;
  channel.auto_clear = true;
  if (i2s_new_channel(&channel, &tx, &rx) != ESP_OK) return false;
  i2s_std_config_t config = {};
  config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  config.gpio_cfg.mclk = GPIO_NUM_11;
  config.gpio_cfg.bclk = GPIO_NUM_17;
  config.gpio_cfg.ws = GPIO_NUM_3;
  config.gpio_cfg.dout = GPIO_NUM_48;
  config.gpio_cfg.din = GPIO_NUM_4;
  if (i2s_channel_init_std_mode(tx, &config) != ESP_OK
      || i2s_channel_init_std_mode(rx, &config) != ESP_OK) return false;
  i2s_event_callbacks_t callbacks = {};
  callbacks.on_recv_q_ovf = receiveOverflow;
  if (i2s_channel_register_event_callback(rx, &callbacks, nullptr) != ESP_OK) return false;
  int16_t silence[framesPerBlock * 2] = {};
  for (size_t i = 0; i < dmaBlocks; ++i) {
    size_t loaded = 0;
    if (i2s_channel_preload_data(tx, silence, sizeof(silence), &loaded) != ESP_OK
        || loaded != sizeof(silence)) return false;
  }
  return true;
}

void stopAudio() {
  digitalWrite(amplifierPin, LOW);
  if (txEnabled) i2s_channel_disable(tx);
  if (rxEnabled) i2s_channel_disable(rx);
  if (tx) i2s_del_channel(tx);
  if (rx) i2s_del_channel(rx);
  tx = rx = nullptr;
  txEnabled = rxEnabled = false;
  if (wireStarted) {
    writeCodec(0x0D, 0xFC);
    writeCodec(0x00, 0x00);
    Wire.end();
    wireStarted = false;
  }
}

