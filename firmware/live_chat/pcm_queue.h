#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// One writer and one reader. Reset/erase only after both tasks have stopped.
// Power-of-two capacity also preserves indexing when uint32_t counters wrap.
template<size_t Capacity> struct PcmQueue {
  static_assert(Capacity && !(Capacity & (Capacity - 1)));
  int16_t *samples = nullptr;
  std::atomic<uint32_t> written{0}, consumed{0};
  bool push(const int16_t *source, size_t count) {
    const uint32_t w = written.load(std::memory_order_relaxed);
    const uint32_t r = consumed.load(std::memory_order_acquire);
    if (count > Capacity - uint32_t(w - r)) return false;
    for (size_t i = 0; i < count; ++i) samples[(w + i) & (Capacity - 1)] = source[i];
    written.store(w + count, std::memory_order_release);
    return true;
  }
  size_t available() const {
    return uint32_t(written.load(std::memory_order_acquire) - consumed.load(std::memory_order_relaxed));
  }
  size_t pop(int16_t *target, size_t count) {
    const uint32_t r = consumed.load(std::memory_order_relaxed);
    const size_t ready = uint32_t(written.load(std::memory_order_acquire) - r);
    if (count > ready) count = ready;
    for (size_t i = 0; i < count; ++i) target[i] = samples[(r + i) & (Capacity - 1)];
    consumed.store(r + count, std::memory_order_release);
    return count;
  }
  void reset() {
    if (samples) memset(samples, 0, Capacity * sizeof(int16_t));
    written = consumed = 0;
  }
};
