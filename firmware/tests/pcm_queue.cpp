#include "../live_chat/pcm_queue.h"
#include <cassert>
#include <thread>
#include <vector>

int main() {
  PcmQueue<1024> queue;
  int16_t storage[1024] = {};
  queue.samples = storage;
  int16_t block[1024], out[1024];
  for (int i = 0; i < 1024; ++i) block[i] = i;
  assert(queue.push(block, 1024));
  assert(!queue.push(block, 1));
  assert(queue.pop(out, 512) == 512);
  assert(queue.push(block, 512));
  assert(queue.pop(out, 1024) == 1024);
  for (int i = 0; i < 512; ++i) assert(out[i] == i + 512 && out[i + 512] == i);
  queue.reset();
  // Wrap both the ring index and uint32_t sequence number under concurrency.
  queue.written = queue.consumed = UINT32_MAX - 511;
  constexpr int count = 500000;
  std::thread producer([&] {
    for (int i = 0; i < count; ++i) {
      const int16_t sample = int16_t(i % 32000);
      while (!queue.push(&sample, 1)) std::this_thread::yield();
    }
  });
  for (int i = 0; i < count; ++i) {
    int16_t sample;
    while (!queue.pop(&sample, 1)) std::this_thread::yield();
    assert(sample == i % 32000);
  }
  producer.join();
  assert(queue.available() == 0);
  queue.reset();
  for (auto sample : storage) assert(sample == 0);
}
