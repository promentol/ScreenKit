// Copyright (c) ScreenKit contributors. MIT.
//
// Remote, keyboard and gamepad input: SDL events in, DOM keydown / keyup out.
//
// One path for every platform, because SDL already normalises the hardware:
//
//   tvOS Siri Remote     SDL_HINT_TV_REMOTE_AS_JOYSTICK=0 makes UIKit presses
//   Android TV remote    keyboard events: arrows, Return (select / D-pad centre),
//                        Escape (tvOS Menu), AC Back (Android Back), media keys
//   macOS / Linux        a keyboard is a keyboard
//   game controllers     SDL gamepad events -- the usual input on embedded Linux
//                        (Batocera) and a common one on Apple TV and Android
//
// Each becomes one `KeyEvent` shaped like a DOM KeyboardEvent -- `key`, `code`
// and the legacy `keyCode` TV frameworks still key on (Blits: 37-40 arrows,
// 13 enter, 8 back) -- and is dispatched in JS by the DOM shim's
// `__screenkitKey` hook. Mapping lives here because it is a table over SDL's
// enums; the DOM event itself is built in JS, where the rest of the DOM is.
//
// Gamepads also reach the page as the W3C Gamepad API. The router keeps the
// process's GamepadRegistry (core/src/input/Gamepads.h) current -- a pad it
// opens is connected there, and `tick` polls every pad's state -- and fires
// `gamepadconnected` / `gamepaddisconnected` through the shim's
// `__screenkitGamepad` hook. Once the app calls `navigator.getGamepads()` the
// gamepads are its own: from then on gamepad buttons and sticks make no keys,
// and the keys they were holding are released. Keyboards and remotes are
// unaffected.
//
// A host's whole integration is three calls:
//
//   InputRouter::configureHints();                  // before SDL_Init
//   SDL_Init(SDL_INIT_VIDEO | InputRouter::requiredSubsystems());
//   InputRouter input(runtime);
//   ... for each event:  if (input.handleEvent(event)) continue;
//   ... once per frame:  input.tick(SDL_GetTicksNS());
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>

namespace screenkit {

class FocusTarget;
class Runtime;
struct GamepadConnection;

/// A key press as the DOM describes it.
struct KeyEvent {
  bool down = true;       // keydown or keyup
  bool repeat = false;
  std::string key;        // UI Events `key`: "ArrowUp", "Enter", "a", "A"
  std::string code;       // UI Events `code`: "ArrowUp", "KeyA", "BrowserBack"
  int keyCode = 0;        // legacy `keyCode` / `which`
  unsigned modifiers = 0; // kShift | kControl | kAlt | kMeta

  static constexpr unsigned kShift = 1;
  static constexpr unsigned kControl = 2;
  static constexpr unsigned kAlt = 4;
  static constexpr unsigned kMeta = 8;
};

/// The "back" every platform's remote has -- Android Back, the tvOS Menu
/// button, a gamepad's B -- as one event. `key` and `code` are the UI Events
/// names; `keyCode` 8 is the TV convention Lightning and Blits map to back.
KeyEvent backKeyEvent(bool down);

/// Keyboard (and remote-as-keyboard) event -> DOM key. False only for a
/// scancode that names no key at all.
bool keyEventFromKeyboard(const SDL_KeyboardEvent& event, KeyEvent& out);

/// Gamepad button -> DOM key. False for buttons that mean nothing to a TV UI.
bool keyEventFromGamepadButton(SDL_GamepadButton button, bool down, KeyEvent& out);

/// Routes SDL input events to a runtime's JS thread. Main thread only.
class InputRouter {
 public:
  explicit InputRouter(std::shared_ptr<Runtime> runtime);
  ~InputRouter();

  InputRouter(const InputRouter&) = delete;
  InputRouter& operator=(const InputRouter&) = delete;

  /// Input follows focus (`Architecture.md` 5.1): keys go to the focused
  /// browsing context and to nothing else. Null in the target means the runtime
  /// this router was built with -- an unfocused tree, or a child that has just
  /// been terminated.
  ///
  /// A switch releases whatever keys were held, the way a gamepad disconnecting
  /// mid-press does, so no context is left with a key down it will never see go
  /// up. Main thread.
  void setFocusTarget(std::shared_ptr<FocusTarget> focus);

  /// Remotes as keys (see the header comment). Set before SDL_Init.
  static void configureHints();

  /// The SDL subsystems input needs beyond events: gamepads.
  static SDL_InitFlags requiredSubsystems() { return SDL_INIT_GAMEPAD; }

  /// Handles a keyboard, gamepad-button, gamepad-axis or gamepad hot-plug event.
  /// Returns true when the event was one of those.
  bool handleEvent(const SDL_Event& event);

  /// Once per frame, before the runtime's frame: polls the gamepads the Gamepad
  /// API reads, and repeats held gamepad keys, which SDL does not repeat itself
  /// (a keyboard's repeat comes from the OS).
  void tick(std::uint64_t nowNs);

  /// Delay before a held gamepad key repeats, and the interval after.
  static constexpr std::uint64_t kRepeatDelayNs = 400'000'000;
  static constexpr std::uint64_t kRepeatIntervalNs = 80'000'000;

 private:
  struct Held {
    KeyEvent event;
    std::uint64_t nextRepeatNs = 0;
  };

  void deliver(const KeyEvent& event, const std::string& source);
  /// ...to one runtime by name, which a release held over a focus switch needs:
  /// it belongs to the context that was holding the key, not to whoever has
  /// focus by the time it can be sent.
  void deliverTo(const std::shared_ptr<Runtime>& runtime, const KeyEvent& event, const std::string& source);
  /// The runtime input goes to now: the focused one, or the host's own.
  const std::shared_ptr<Runtime>& target() const;
  /// Adopt a focus switch, releasing the keys the outgoing context was holding.
  void followFocus();
  void press(const std::string& id, KeyEvent event, std::uint64_t nowNs);
  void release(const std::string& id);
  void handleAxis(const SDL_GamepadAxisEvent& event);
  /// True once the app has claimed the gamepads, releasing whatever gamepad
  /// keys are still held the first time it is seen.
  bool gamepadsClaimed();
  /// Fire `type` (gamepadconnected / gamepaddisconnected) at window. A task
  /// like a key's, but never dropped for a pause: a connection is a fact the
  /// page needs after it resumes.
  void deliverGamepad(const char* type, const GamepadConnection& connection);

  std::shared_ptr<Runtime> runtime_;
  /// Where focus is, shared with the instance binding that moves it, and the
  /// generation this router has already adopted.
  std::shared_ptr<FocusTarget> focus_;
  std::shared_ptr<Runtime> focused_;
  std::uint64_t focusGeneration_ = 0;
  // SCREENKIT_LOG_INPUT=1: log every key event with where it came from. The
  // way to see what a remote or controller actually sends on a device.
  bool logInput_ = false;
  std::unordered_map<SDL_JoystickID, SDL_Gamepad*> gamepads_;
  // Gamepad-generated keys currently held, by "<gamepad>:<source>", so a
  // release, a disconnect or a stick returning to centre can end exactly them.
  std::unordered_map<std::string, Held> held_;
  /// Keys still down when focus left a context. The context is frozen by then,
  /// so a keyup sent now is dropped at the gate -- these wait for it to run
  /// again, which is the same answer `gamepadsClaimed` gives for a paused page.
  struct PendingRelease {
    std::weak_ptr<Runtime> runtime;
    KeyEvent event;
    std::string why;
  };
  std::vector<PendingRelease> pendingReleases_;
  // The claim has been seen and logged.
  bool claimSeen_ = false;
};

}  // namespace screenkit
