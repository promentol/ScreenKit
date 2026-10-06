// Copyright (c) ScreenKit contributors. MIT.
#include <screenkit/Input.h>

#include <screenkit/Instance.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_platform_defines.h>
#include <SDL3/SDL_timer.h>

#include <jsi/jsi.h>

#include <screenkit/Log.h>
#include <screenkit/Runtime.h>

#include "Gamepads.h"
#include "UserAgentDispatch.h"

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

constexpr const char* kTag = "screenkit.input";

struct Named {
  const char* code;
  const char* key;
  int keyCode;
};

/// Keys whose `key` is a name rather than the character they type. keyCodes are
/// Chromium's where it has one, and the CE-HTML / HbbTV virtual-key values TV
/// apps use for media keys it has none for (play 415, stop 413, rewind 412,
/// fast-forward 417).
bool namedKey(SDL_Scancode scancode, Named& out) {
  switch (scancode) {
    case SDL_SCANCODE_UP: out = {"ArrowUp", "ArrowUp", 38}; return true;
    case SDL_SCANCODE_DOWN: out = {"ArrowDown", "ArrowDown", 40}; return true;
    case SDL_SCANCODE_LEFT: out = {"ArrowLeft", "ArrowLeft", 37}; return true;
    case SDL_SCANCODE_RIGHT: out = {"ArrowRight", "ArrowRight", 39}; return true;
    case SDL_SCANCODE_RETURN: out = {"Enter", "Enter", 13}; return true;
    case SDL_SCANCODE_KP_ENTER: out = {"NumpadEnter", "Enter", 13}; return true;
    case SDL_SCANCODE_ESCAPE: out = {"Escape", "Escape", 27}; return true;
    case SDL_SCANCODE_BACKSPACE: out = {"Backspace", "Backspace", 8}; return true;
    case SDL_SCANCODE_TAB: out = {"Tab", "Tab", 9}; return true;
    case SDL_SCANCODE_DELETE: out = {"Delete", "Delete", 46}; return true;
    case SDL_SCANCODE_INSERT: out = {"Insert", "Insert", 45}; return true;
    case SDL_SCANCODE_HOME: out = {"Home", "Home", 36}; return true;
    case SDL_SCANCODE_END: out = {"End", "End", 35}; return true;
    case SDL_SCANCODE_PAGEUP: out = {"PageUp", "PageUp", 33}; return true;
    case SDL_SCANCODE_PAGEDOWN: out = {"PageDown", "PageDown", 34}; return true;
    case SDL_SCANCODE_PAUSE: out = {"Pause", "Pause", 19}; return true;
    case SDL_SCANCODE_MENU:
    case SDL_SCANCODE_APPLICATION: out = {"ContextMenu", "ContextMenu", 93}; return true;
    case SDL_SCANCODE_CAPSLOCK: out = {"CapsLock", "CapsLock", 20}; return true;
    case SDL_SCANCODE_LSHIFT: out = {"ShiftLeft", "Shift", 16}; return true;
    case SDL_SCANCODE_RSHIFT: out = {"ShiftRight", "Shift", 16}; return true;
    case SDL_SCANCODE_LCTRL: out = {"ControlLeft", "Control", 17}; return true;
    case SDL_SCANCODE_RCTRL: out = {"ControlRight", "Control", 17}; return true;
    case SDL_SCANCODE_LALT: out = {"AltLeft", "Alt", 18}; return true;
    case SDL_SCANCODE_RALT: out = {"AltRight", "Alt", 18}; return true;
    case SDL_SCANCODE_LGUI: out = {"MetaLeft", "Meta", 91}; return true;
    case SDL_SCANCODE_RGUI: out = {"MetaRight", "Meta", 93}; return true;
    case SDL_SCANCODE_MEDIA_PLAY_PAUSE: out = {"MediaPlayPause", "MediaPlayPause", 179}; return true;
    case SDL_SCANCODE_MEDIA_PLAY: out = {"MediaPlay", "MediaPlay", 415}; return true;
    case SDL_SCANCODE_MEDIA_PAUSE: out = {"MediaPause", "MediaPause", 19}; return true;
    case SDL_SCANCODE_MEDIA_STOP: out = {"MediaStop", "MediaStop", 413}; return true;
    case SDL_SCANCODE_MEDIA_REWIND: out = {"MediaRewind", "MediaRewind", 412}; return true;
    case SDL_SCANCODE_MEDIA_FAST_FORWARD: out = {"MediaFastForward", "MediaFastForward", 417}; return true;
    case SDL_SCANCODE_MEDIA_NEXT_TRACK: out = {"MediaTrackNext", "MediaTrackNext", 176}; return true;
    case SDL_SCANCODE_MEDIA_PREVIOUS_TRACK: out = {"MediaTrackPrevious", "MediaTrackPrevious", 177}; return true;
    case SDL_SCANCODE_VOLUMEUP: out = {"AudioVolumeUp", "AudioVolumeUp", 175}; return true;
    case SDL_SCANCODE_VOLUMEDOWN: out = {"AudioVolumeDown", "AudioVolumeDown", 174}; return true;
    case SDL_SCANCODE_MUTE: out = {"AudioVolumeMute", "AudioVolumeMute", 173}; return true;
    case SDL_SCANCODE_CHANNEL_INCREMENT: out = {"ChannelUp", "ChannelUp", 427}; return true;
    case SDL_SCANCODE_CHANNEL_DECREMENT: out = {"ChannelDown", "ChannelDown", 428}; return true;
    default: break;
  }
  if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) {
    static const char* const kF[] = {"F1", "F2", "F3", "F4", "F5", "F6",
                                     "F7", "F8", "F9", "F10", "F11", "F12"};
    const int i = scancode - SDL_SCANCODE_F1;
    out = {kF[i], kF[i], 112 + i};
    return true;
  }
  return false;
}

/// Keys that type a character: the physical `code` and legacy keyCode are
/// fixed per scancode; `key` is the character, taken from the layout.
bool printableKey(SDL_Scancode scancode, const char*& code, int& keyCode) {
  static const char* const kLetters[] = {
      "KeyA", "KeyB", "KeyC", "KeyD", "KeyE", "KeyF", "KeyG", "KeyH", "KeyI",
      "KeyJ", "KeyK", "KeyL", "KeyM", "KeyN", "KeyO", "KeyP", "KeyQ", "KeyR",
      "KeyS", "KeyT", "KeyU", "KeyV", "KeyW", "KeyX", "KeyY", "KeyZ"};
  static const char* const kDigits[] = {"Digit1", "Digit2", "Digit3", "Digit4", "Digit5",
                                        "Digit6", "Digit7", "Digit8", "Digit9", "Digit0"};
  static const char* const kNumpad[] = {"Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5",
                                        "Numpad6", "Numpad7", "Numpad8", "Numpad9", "Numpad0"};
  if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
    code = kLetters[scancode - SDL_SCANCODE_A];
    keyCode = 65 + (scancode - SDL_SCANCODE_A);
    return true;
  }
  if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_0) {
    const int i = scancode - SDL_SCANCODE_1;
    code = kDigits[i];
    keyCode = i == 9 ? 48 : 49 + i;
    return true;
  }
  if (scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_0) {
    const int i = scancode - SDL_SCANCODE_KP_1;
    code = kNumpad[i];
    keyCode = i == 9 ? 96 : 97 + i;
    return true;
  }
  switch (scancode) {
    case SDL_SCANCODE_SPACE: code = "Space"; keyCode = 32; return true;
    case SDL_SCANCODE_MINUS: code = "Minus"; keyCode = 189; return true;
    case SDL_SCANCODE_EQUALS: code = "Equal"; keyCode = 187; return true;
    case SDL_SCANCODE_LEFTBRACKET: code = "BracketLeft"; keyCode = 219; return true;
    case SDL_SCANCODE_RIGHTBRACKET: code = "BracketRight"; keyCode = 221; return true;
    case SDL_SCANCODE_BACKSLASH: code = "Backslash"; keyCode = 220; return true;
    case SDL_SCANCODE_SEMICOLON: code = "Semicolon"; keyCode = 186; return true;
    case SDL_SCANCODE_APOSTROPHE: code = "Quote"; keyCode = 222; return true;
    case SDL_SCANCODE_GRAVE: code = "Backquote"; keyCode = 192; return true;
    case SDL_SCANCODE_COMMA: code = "Comma"; keyCode = 188; return true;
    case SDL_SCANCODE_PERIOD: code = "Period"; keyCode = 190; return true;
    case SDL_SCANCODE_SLASH: code = "Slash"; keyCode = 191; return true;
    default: return false;
  }
}

bool isNumpadDigit(SDL_Scancode scancode) {
  return scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_0;
}

std::string utf8(char32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return out;
}

unsigned modifiersOf(SDL_Keymod mod) {
  unsigned out = 0;
  if (mod & SDL_KMOD_SHIFT) out |= KeyEvent::kShift;
  if (mod & SDL_KMOD_CTRL) out |= KeyEvent::kControl;
  if (mod & SDL_KMOD_ALT) out |= KeyEvent::kAlt;
  if (mod & SDL_KMOD_GUI) out |= KeyEvent::kMeta;
  return out;
}

KeyEvent named(const char* code, const char* key, int keyCode, bool down) {
  KeyEvent e;
  e.down = down;
  e.code = code;
  e.key = key;
  e.keyCode = keyCode;
  return e;
}

// A stick is a direction once it is past this, and stops being one below the
// release threshold -- the gap is what keeps a resting stick from chattering.
constexpr int kStickPress = 16384;   // half of SDL's axis range
constexpr int kStickRelease = 8192;

}  // namespace

KeyEvent backKeyEvent(bool down) { return named("BrowserBack", "GoBack", 8, down); }

bool keyEventFromKeyboard(const SDL_KeyboardEvent& event, KeyEvent& out) {
  out = KeyEvent{};
  out.down = event.down;
  out.repeat = event.repeat;
  out.modifiers = modifiersOf(event.mod);

  // Remote "back" buttons. Android's Back arrives as AC Back; tvOS's Menu button,
  // which the HIG defines as "return to the previous screen", arrives as Escape.
  // A tvOS app has no other use for Escape, so there it is back too; on a
  // desktop, Escape is Escape.
#if defined(SDL_PLATFORM_TVOS)
  const bool isBack = event.scancode == SDL_SCANCODE_AC_BACK || event.scancode == SDL_SCANCODE_ESCAPE;
#else
  const bool isBack = event.scancode == SDL_SCANCODE_AC_BACK;
#endif
  if (isBack) {
    const unsigned modifiers = out.modifiers;
    out = backKeyEvent(event.down);
    out.repeat = event.repeat;
    out.modifiers = modifiers;
    return true;
  }

  Named n;
  if (namedKey(event.scancode, n)) {
    out.code = n.code;
    out.key = n.key;
    out.keyCode = n.keyCode;
    return true;
  }

  const char* code = nullptr;
  int keyCode = 0;
  if (printableKey(event.scancode, code, keyCode)) {
    out.code = code;
    out.keyCode = keyCode;
    // The character this layout types, shift applied -- what `key` reports in
    // a browser. `key_event` false: the key-event keycode SDL delivers is the
    // unshifted one, and `key` is "A" with shift held. A printable keycode is
    // its Unicode code point; the numpad's are not printable in SDL, so their
    // digit comes from the code.
    if (isNumpadDigit(event.scancode)) {
      out.key = std::string(1, code[6]);
      return true;
    }
    const SDL_Keycode typed = SDL_GetKeyFromScancode(event.scancode, event.mod, false);
    out.key = (typed != SDLK_UNKNOWN && (typed & SDLK_SCANCODE_MASK) == 0 && typed >= 0x20)
                  ? utf8(static_cast<char32_t>(typed))
                  : std::string("Unidentified");
    return true;
  }

  if (event.scancode == SDL_SCANCODE_UNKNOWN) return false;
  // A real key with no DOM name: a browser still delivers it, as Unidentified.
  out.code = "";
  out.key = "Unidentified";
  out.keyCode = 0;
  return true;
}

bool keyEventFromGamepadButton(SDL_GamepadButton button, bool down, KeyEvent& out) {
  switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: out = named("ArrowUp", "ArrowUp", 38, down); return true;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: out = named("ArrowDown", "ArrowDown", 40, down); return true;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: out = named("ArrowLeft", "ArrowLeft", 37, down); return true;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: out = named("ArrowRight", "ArrowRight", 39, down); return true;
    // The bottom face button selects and the right one goes back, the layout
    // every console UI uses whatever the labels say.
    case SDL_GAMEPAD_BUTTON_SOUTH: out = named("Enter", "Enter", 13, down); return true;
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_BACK: out = backKeyEvent(down); return true;
    case SDL_GAMEPAD_BUTTON_START:
      out = named("MediaPlayPause", "MediaPlayPause", 179, down);
      return true;
    default: return false;
  }
}

InputRouter::InputRouter(std::shared_ptr<Runtime> runtime) : runtime_(std::move(runtime)) {
  const char* flag = std::getenv("SCREENKIT_LOG_INPUT");
  logInput_ = flag != nullptr && flag[0] != '\0' && flag[0] != '0';
}

void InputRouter::setFocusTarget(std::shared_ptr<FocusTarget> focus) {
  focus_ = std::move(focus);
  focusGeneration_ = focus_ ? focus_->generation() : 0;
  focused_ = focus_ ? focus_->current() : nullptr;
}

const std::shared_ptr<Runtime>& InputRouter::target() const { return focused_ ? focused_ : runtime_; }

void InputRouter::followFocus() {
  if (!focus_) return;
  const std::uint64_t generation = focus_->generation();
  if (generation == focusGeneration_) return;
  // Released against the *outgoing* context, before the new target is adopted:
  // exactly what a gamepad removed mid-press does (SDL_EVENT_GAMEPAD_REMOVED).
  // Except that a focus switch has already frozen that context, and `deliver`
  // drops everything a paused runtime would get -- so those wait, and go when it
  // runs again, the same answer `gamepadsClaimed` gives a paused page. Without
  // that, a game that took the remote mid-press resumes with the key still down.
  const std::shared_ptr<Runtime>& outgoing = target();
  const bool frozen = outgoing && outgoing->paused();
  for (auto it = held_.begin(); it != held_.end();) {
    KeyEvent up = it->second.event;
    up.down = false;
    up.repeat = false;
    const std::string why = "gamepad " + it->first + " (focus moved)";
    if (frozen) {
      pendingReleases_.push_back({outgoing, up, why});
    } else {
      deliver(up, why);
    }
    it = held_.erase(it);
  }
  focusGeneration_ = generation;
  focused_ = focus_->current();
  if (logInput_) {
    log(LogLevel::Log, kTag,
        focused_ ? "input follows focus: an <iframe> instance" : "input follows focus: this page");
  }
}

InputRouter::~InputRouter() {
  for (auto& entry : gamepads_) {
    GamepadConnection ignored;
    GamepadRegistry::shared().disconnect(entry.first, ignored);
    SDL_CloseGamepad(entry.second);
  }
}

void InputRouter::configureHints() {
  SDL_SetHint(SDL_HINT_TV_REMOTE_AS_JOYSTICK, "0");
}

bool InputRouter::handleEvent(const SDL_Event& event) {
  followFocus();
  switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      KeyEvent key;
      if (keyEventFromKeyboard(event.key, key)) {
        deliver(key, "keyboard scancode " + std::to_string(event.key.scancode));
      }
      return true;
    }

    case SDL_EVENT_GAMEPAD_ADDED: {
      if (gamepads_.count(event.gdevice.which) == 0) {
        SDL_Gamepad* pad = SDL_OpenGamepad(event.gdevice.which);
        if (pad != nullptr) {
          gamepads_[event.gdevice.which] = pad;
          const char* name = SDL_GetGamepadName(pad);
          log(LogLevel::Log, kTag, std::string("gamepad connected: ") + (name ? name : "(unnamed)"));
          GamepadConnection connection;
          if (GamepadRegistry::shared().connect(pad, connection)) deliverGamepad("gamepadconnected", connection);
        }
      }
      return true;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
      // Whatever it was holding is released, so a disconnect mid-press does not
      // leave a key down forever.
      const std::string prefix = std::to_string(event.gdevice.which) + ":";
      for (auto it = held_.begin(); it != held_.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
          KeyEvent up = it->second.event;
          up.down = false;
          up.repeat = false;
          deliver(up, it->first + " (disconnected)");
          it = held_.erase(it);
        } else {
          ++it;
        }
      }
      auto pad = gamepads_.find(event.gdevice.which);
      if (pad != gamepads_.end()) {
        GamepadConnection connection;
        if (GamepadRegistry::shared().disconnect(event.gdevice.which, connection)) {
          deliverGamepad("gamepaddisconnected", connection);
        }
        SDL_CloseGamepad(pad->second);
        gamepads_.erase(pad);
      }
      return true;
    }

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
      // The app reads the pads itself now (navigator.getGamepads()).
      if (gamepadsClaimed()) return true;
      KeyEvent key;
      const auto button = static_cast<SDL_GamepadButton>(event.gbutton.button);
      if (!keyEventFromGamepadButton(button, event.gbutton.down, key)) return true;
      const std::string id =
          std::to_string(event.gbutton.which) + ":button:" + std::to_string(event.gbutton.button);
      if (event.gbutton.down) {
        press(id, key, event.gbutton.timestamp);
      } else {
        release(id);
      }
      return true;
    }

    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      if (!gamepadsClaimed()) handleAxis(event.gaxis);
      return true;

    default:
      return false;
  }
}

void InputRouter::handleAxis(const SDL_GamepadAxisEvent& event) {
  // The left stick navigates like the D-pad; a gamepad without one (many on
  // embedded Linux) is otherwise unusable in a TV UI.
  bool horizontal;
  if (event.axis == SDL_GAMEPAD_AXIS_LEFTX) {
    horizontal = true;
  } else if (event.axis == SDL_GAMEPAD_AXIS_LEFTY) {
    horizontal = false;
  } else {
    return;
  }
  const std::string base = std::to_string(event.which) + (horizontal ? ":stick-x:" : ":stick-y:");
  const std::string negative = base + "-";
  const std::string positive = base + "+";
  const int value = event.value;

  auto want = [&](const std::string& id, bool active, SDL_GamepadButton as) {
    const bool isHeld = held_.count(id) != 0;
    if (active && !isHeld) {
      KeyEvent key;
      keyEventFromGamepadButton(as, true, key);
      press(id, key, event.timestamp);
    } else if (!active && isHeld) {
      release(id);
    }
  };
  // Pressed past kStickPress, kept until back under kStickRelease.
  const bool negativeHeld = held_.count(negative) != 0;
  const bool positiveHeld = held_.count(positive) != 0;
  const bool negActive = negativeHeld ? value < -kStickRelease : value < -kStickPress;
  const bool posActive = positiveHeld ? value > kStickRelease : value > kStickPress;
  want(negative, negActive, horizontal ? SDL_GAMEPAD_BUTTON_DPAD_LEFT : SDL_GAMEPAD_BUTTON_DPAD_UP);
  want(positive, posActive, horizontal ? SDL_GAMEPAD_BUTTON_DPAD_RIGHT : SDL_GAMEPAD_BUTTON_DPAD_DOWN);
}

void InputRouter::press(const std::string& id, KeyEvent event, std::uint64_t nowNs) {
  if (held_.count(id) != 0) return;  // already down; repeat comes from tick()
  event.down = true;
  event.repeat = false;
  deliver(event, "gamepad " + id);
  held_[id] = Held{event, nowNs + kRepeatDelayNs};
}

void InputRouter::release(const std::string& id) {
  auto it = held_.find(id);
  if (it == held_.end()) return;
  KeyEvent up = it->second.event;
  up.down = false;
  up.repeat = false;
  deliver(up, "gamepad " + id);
  held_.erase(it);
}

bool InputRouter::gamepadsClaimed() {
  if (!GamepadRegistry::shared().claimed()) return false;
  if (!claimSeen_) {
    claimSeen_ = true;
    log(LogLevel::Log, kTag,
        "gamepads claimed: the app called navigator.getGamepads(), so gamepad buttons and sticks no longer send keys");
  }
  // Keys held when the claim landed get their keyup; nothing presses again. A
  // paused runtime would drop the keyups and leave the keys down, so they wait
  // for the first call after it resumes.
  if (target() && target()->paused()) return true;
  for (auto it = held_.begin(); it != held_.end();) {
    KeyEvent up = it->second.event;
    up.down = false;
    up.repeat = false;
    deliver(up, "gamepad " + it->first + " (claimed)");
    it = held_.erase(it);
  }
  return true;
}

void InputRouter::tick(std::uint64_t nowNs) {
  followFocus();
  // Whatever was still down when a context lost focus, now that it runs again.
  for (auto it = pendingReleases_.begin(); it != pendingReleases_.end();) {
    std::shared_ptr<Runtime> owner = it->runtime.lock();
    if (!owner) {
      it = pendingReleases_.erase(it);
      continue;
    }
    if (owner->paused()) {
      ++it;
      continue;
    }
    deliverTo(owner, it->event, it->why);
    it = pendingReleases_.erase(it);
  }
  // The snapshot navigator.getGamepads() reads, taken before the frame so a
  // read inside requestAnimationFrame is at most a frame old. Not gated on a
  // pause: the state is a fact, and a resumed page reads it current.
  GamepadRegistry::shared().poll();
  gamepadsClaimed();

  // A paused runtime gets no input, repeats included. Pushing the deadline
  // along, rather than leaving it, keeps a resume from firing a stale repeat
  // the instant it lands.
  const bool paused = target() && target()->paused();
  for (auto& entry : held_) {
    Held& held = entry.second;
    if (paused) {
      held.nextRepeatNs = std::max(held.nextRepeatNs, nowNs + kRepeatIntervalNs);
      continue;
    }
    if (nowNs < held.nextRepeatNs) continue;
    KeyEvent repeat = held.event;
    repeat.repeat = true;
    deliver(repeat, "gamepad " + entry.first);
    held.nextRepeatNs = nowNs + kRepeatIntervalNs;
  }
}

void InputRouter::deliverGamepad(const char* type, const GamepadConnection& connection) {
  if (logInput_) {
    log(LogLevel::Log, kTag,
        std::string(type) + " index " + std::to_string(connection.index) + " <- " + connection.id);
  }
  const std::shared_ptr<Runtime>& runtime = target();
  if (!runtime) return;
  std::shared_ptr<JsExecutor> executor = runtime->executor();
  if (!executor) return;
  // Unlike a key, neither dropped while paused nor by a pause after queueing:
  // the freeze gate holds the task, and the page hears of the pad on resume.
  std::weak_ptr<JsExecutor> weakExecutor = executor;
  std::string eventType = type;
  executor->invokeAsync([eventType, connection, weakExecutor](jsi::Runtime& rt) {
    auto executor = weakExecutor.lock();
    if (!executor) return;
    // The DOM shim defines the hook; a bundle run without it has no window.
    jsi::Value hook = rt.global().getProperty(rt, "__screenkitGamepad");
    if (!hook.isObject() || !hook.getObject(rt).isFunction(rt)) return;
    try {
      jsi::Value stepper = hook.getObject(rt).getFunction(rt).call(
          rt, {jsi::String::createFromAscii(rt, eventType), jsi::Value(connection.index),
               jsi::Value(static_cast<double>(connection.serial)), jsi::String::createFromUtf8(rt, connection.id),
               jsi::Value(connection.rumble)});
      runUserAgentDispatch(*executor, rt, stepper);
    } catch (const jsi::JSIException& e) {
      log(LogLevel::Error, kTag, eventType + " event failed: " + e.what());
    }
  });
}

void InputRouter::deliver(const KeyEvent& event, const std::string& source) {
  deliverTo(target(), event, source);
}

void InputRouter::deliverTo(const std::shared_ptr<Runtime>& runtime, const KeyEvent& event,
                            const std::string& source) {
  if (logInput_) {
    log(LogLevel::Log, kTag,
        std::string(event.down ? "keydown " : "keyup ") + event.key + " keyCode " +
            std::to_string(event.keyCode) + (event.repeat ? " repeat" : "") + " <- " + source);
  }
  if (!runtime) return;
  // `Paused` means no input (Architecture.md, lifecycle table). Dropped here,
  // not queued: the freeze gate would hold the task and deliver it on resume, a
  // key press from before the pause landing in whatever is on screen after it.
  if (runtime->paused()) {
    if (logInput_) log(LogLevel::Log, kTag, "  dropped: runtime paused");
    return;
  }
  std::shared_ptr<JsExecutor> executor = runtime->executor();
  if (!executor) return;
  // A press queued just before a pause is held by the same gate, so the task
  // itself checks: a pause since it was queued means it is dropped too.
  const std::uint64_t epoch = executor->pauseEpoch();
  // One task per event, like a browser's user-interaction task source: FIFO
  // with timers, a microtask checkpoint after it, frame work after that.
  std::weak_ptr<JsExecutor> weakExecutor = executor;
  const bool logInput = logInput_;
  executor->invokeAsync([event, weakExecutor, epoch, logInput](jsi::Runtime& rt) {
    auto executor = weakExecutor.lock();
    if (!executor) return;
    if (executor->pauseEpoch() != epoch) {
      if (logInput) log(LogLevel::Log, kTag, "  dropped: runtime paused after " + event.key + " was queued");
      return;
    }
    // The DOM shim defines the hook; a bundle run without it has no document for
    // a key event to reach, so the event goes nowhere, quietly.
    jsi::Value hook = rt.global().getProperty(rt, "__screenkitKey");
    if (!hook.isObject() || !hook.getObject(rt).isFunction(rt)) return;
    try {
      jsi::Value stepper = hook.getObject(rt).getFunction(rt).call(
          rt, {jsi::String::createFromAscii(rt, event.down ? "keydown" : "keyup"),
               jsi::String::createFromUtf8(rt, event.key), jsi::String::createFromAscii(rt, event.code),
               jsi::Value(event.keyCode), jsi::Value(event.repeat),
               jsi::Value(static_cast<double>(event.modifiers))});
      runUserAgentDispatch(*executor, rt, stepper);
    } catch (const jsi::JSIException& e) {
      log(LogLevel::Error, kTag, std::string("key event failed: ") + e.what());
    }
  });
}

}  // namespace screenkit
