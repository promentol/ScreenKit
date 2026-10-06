// Copyright (c) ScreenKit contributors. MIT.
//
// The gamepads the W3C Gamepad API sees: the state behind
// `navigator.getGamepads()`.
//
// Only the main thread talks to SDL about gamepads. InputRouter opens a pad on
// SDL_EVENT_GAMEPAD_ADDED and hands it to `connect`, closes it after
// `disconnect`, and calls `poll` once a frame, before the frame runs; the JS
// thread reads a copy of the result under the lock (`__screenkit.gamepads`,
// bindings/Gamepads.h). The one exception is rumble, which the binding sends
// from the JS thread under SDL_LockJoysticks, looking the pad up by its
// SDL_JoystickID.
//
// Gamepads belong to the process, not to a runtime, so one registry serves
// every runtime there is.
//
// Every pad is exposed with the `standard` mapping:
//
//   buttons  0-3 South East West North    4-5 shoulders    6-7 triggers (0..1)
//            8 Back   9 Start   10-11 stick clicks   12-15 D-pad up down left right
//            16 Guide
//   axes     left X, left Y, right X, right Y, each -1..1, up and left negative
//
// **The claim.** Until an app asks for the gamepads, InputRouter turns their
// buttons and left stick into DOM keys, which is how a TV UI navigates. The
// first `navigator.getGamepads()` sets `claimed` for the rest of the process,
// and from then on the app reads the pads itself and InputRouter sends no keys
// for them.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL_gamepad.h>

namespace screenkit {

inline constexpr int kStandardGamepadButtons = 17;
inline constexpr int kStandardGamepadAxes = 4;

/// One slot of the snapshot, as the JS thread reads it.
struct GamepadSlot {
  /// One per connection, never reused; 0 is an empty slot.
  std::uint32_t serial = 0;
  SDL_JoystickID instance = 0;
  /// Chrome's format: `<name> (STANDARD GAMEPAD Vendor: vvvv Product: pppp)`.
  std::string id;
  /// SDL reports rumble for this pad.
  bool rumble = false;
  /// Wall-clock milliseconds since the epoch of the last change, or of the
  /// connection when nothing has changed since. Moves forward on every change.
  double timestampMs = 0;
  std::array<double, kStandardGamepadButtons> buttons{};
  std::array<double, kStandardGamepadAxes> axes{};
};

/// What a connect or disconnect tells the page.
struct GamepadConnection {
  int index = -1;
  std::uint32_t serial = 0;
  std::string id;
  bool rumble = false;
};

class GamepadRegistry {
 public:
  /// The process's registry.
  static GamepadRegistry& shared();

  GamepadRegistry() = default;
  GamepadRegistry(const GamepadRegistry&) = delete;
  GamepadRegistry& operator=(const GamepadRegistry&) = delete;

  /// An opened pad takes the lowest free slot and keeps it while connected.
  /// Connecting a pad that is already here (a second router) returns the same
  /// connection and counts the reference. Main thread only.
  bool connect(SDL_Gamepad* pad, GamepadConnection& out);

  /// The pad left: its slot is free at once, whoever else still holds it open.
  /// False for a pad that was never connected. The caller closes the pad after
  /// this returns. Main thread only.
  bool disconnect(SDL_JoystickID instance, GamepadConnection& out);

  /// Read every connected pad's state from SDL, stamping the ones that changed.
  /// Main thread only, once a frame.
  void poll();

  /// Copy the snapshot into `out`, reusing its storage: one entry per slot up to
  /// the highest occupied one. `claim` sets the claim flag (see the header).
  /// Any thread.
  std::size_t read(std::vector<GamepadSlot>& out, bool claim);

  /// The app has called `navigator.getGamepads()`. Any thread.
  bool claimed() const { return claimed_.load(std::memory_order_relaxed); }

  /// A pad's state in the standard mapping. Main thread only.
  static void readStandard(SDL_Gamepad* pad, std::array<double, kStandardGamepadButtons>& buttons,
                           std::array<double, kStandardGamepadAxes>& axes);

  /// `id` as Chrome writes it.
  static std::string standardId(const char* name, std::uint16_t vendor, std::uint16_t product);

 private:
  /// Main-thread bookkeeping for a pad a router has opened.
  struct Open {
    SDL_Gamepad* pad = nullptr;
    SDL_JoystickID instance = 0;
    int refs = 0;
    bool slotted = false;
    GamepadConnection connection;
  };

  std::vector<Open> open_;         // main thread only
  std::uint32_t nextSerial_ = 1;   // main thread only

  mutable std::mutex mutex_;
  std::vector<GamepadSlot> slots_;  // guarded by mutex_
  std::atomic<bool> claimed_{false};
};

}  // namespace screenkit
