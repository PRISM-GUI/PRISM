#pragma once
#include <prism/audio/backend.hpp>

#include <atomic>
#include <cstddef>
#include <span>
#include <thread>

namespace prism::audio {

// Which pull function a long-lived device should call, when the Output that owns it comes and goes.
// The device's own thread calls call(); the Output's thread calls attach() and detach(). Both sides
// store a pointer and read the other side's flag (store pull / load inside versus store inside /
// load pull), so every atomic here keeps the default seq_cst: this is a Dekker pair, and with
// acquire/release each side may see the other's stale value and both proceed.
class RenderGate {
 public:
  // The device's thread calls `pull` from now on. `pull` stays the caller's: it must outlive the
  // detach that takes it out again.
  void attach(PullFn* pull) { pull_.store(pull); }

  // Clears the pointer, then waits until no call is inside: after this returns the caller may
  // destroy the pull. The wait is one block long, because a pull never blocks.
  void detach()
  {
    pull_.store(nullptr);
    while (inside_.load()) std::this_thread::yield();
  }

  // One block for the device's thread. False means nothing is attached and the device writes
  // silence itself; a true return says the pull filled the block.
  bool call(std::span<std::byte> block, int frames)
  {
    inside_.store(true);
    auto* pull = pull_.load();
    const bool rendered = pull != nullptr;
    if (rendered) (*pull)(block, frames);
    inside_.store(false);
    return rendered;
  }

  // For a device thread that waits for something to play from.
  bool attached() const { return pull_.load() != nullptr; }

 private:
  std::atomic<PullFn*> pull_{nullptr};
  std::atomic<bool> inside_{false};
};

} // namespace prism::audio
