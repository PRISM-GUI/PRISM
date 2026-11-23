#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <prism/audio/backend.hpp>
#include <prism/audio/render_gate.hpp>

#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <thread>
#include <vector>

using prism::audio::PullFn;
using prism::audio::RenderGate;

namespace {

// A pull that counts its calls and stamps every byte of the block: the block says whether it ran.
PullFn stamping(std::atomic<int>& calls, std::byte value) {
  return [&calls, value](std::span<std::byte> block, int frames) {
    static_cast<void>(frames);
    ++calls;
    std::fill(block.begin(), block.end(), value);
  };
}

bool all_bytes(const std::span<std::byte> block, std::byte value) {
  return std::ranges::all_of(block, [value](std::byte byte) { return byte == value; });
}

} // namespace

TEST_CASE("call with nothing attached reports nothing to render") {
  RenderGate gate;
  CHECK_FALSE(gate.attached());

  std::vector<std::byte> block(64, std::byte{0x11});
  CHECK_FALSE(gate.call(block, 16));
  // The gate does not write silence: the caller knows its own silence, the gate only says who plays.
  CHECK(all_bytes(block, std::byte{0x11}));
}

TEST_CASE("an attached pull receives the block") {
  std::atomic<int> calls{0};
  auto pull = stamping(calls, std::byte{0x5a});
  RenderGate gate;

  gate.attach(&pull);
  CHECK(gate.attached());

  std::vector<std::byte> block(64);
  CHECK(gate.call(block, 16));
  CHECK(calls.load() == 1);
  CHECK(all_bytes(block, std::byte{0x5a}));
}

// Two threads and many attach/detach rounds: the renderer loops on the gate while the Output's
// thread hands the pull back and forth and finally destroys it right after detach() returns. The
// judge is ThreadSanitizer; the assertion only says the renderer was really rendering.
TEST_CASE("detach while a caller loops: after detach returns, the pull can be destroyed safely") {
  std::atomic<int> calls{0};
  auto pull = std::make_unique<PullFn>(stamping(calls, std::byte{0x33}));
  RenderGate gate;
  gate.attach(pull.get());

  std::atomic<bool> stop{false};
  std::atomic<int> rendered{0};
  std::thread renderer([&gate, &stop, &rendered] {
    std::vector<std::byte> block(256);
    while (!stop.load(std::memory_order_relaxed)) {
      if (gate.call(block, 64)) ++rendered;
    }
  });

  // The rounds below can all end before the renderer's thread is scheduled: wait for its first call.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (rendered.load() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();

  for (int round = 0; round < 200; ++round) {
    gate.detach();
    gate.attach(pull.get());
  }
  gate.detach();
  pull.reset(); // the renderer may still be looping: nothing may touch the old pull from here on

  const auto mark = rendered.load();
  for (int spin = 0; spin < 1000 && rendered.load() < mark + 2048; ++spin) std::this_thread::yield();
  stop = true;
  renderer.join();
  CHECK(rendered.load() > 0);
}

TEST_CASE("attached() follows attach and detach") {
  std::atomic<int> calls{0};
  auto pull = stamping(calls, std::byte{0x00});
  RenderGate gate;

  CHECK_FALSE(gate.attached());
  gate.attach(&pull);
  CHECK(gate.attached());
  gate.detach();
  CHECK_FALSE(gate.attached());
}
