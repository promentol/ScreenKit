// Copyright (c) ScreenKit contributors. MIT.
#include "Gamepads.h"

#include <algorithm>
#include <cstdio>

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_time.h>

namespace screenkit {
namespace {

/// Wall-clock milliseconds since the epoch: the clock `Date.now()` reads, which
/// the DOM shim's `performance.now()` is measured on.
double epochMs() {
  SDL_Time now = 0;
  if (!SDL_GetCurrentTime(&now)) return 0;
  return static_cast<double>(now) / 1.0e6;
}

double trigger(Sint16 value) {
  return std::clamp(static_cast<double>(value) / SDL_JOYSTICK_AXIS_MAX, 0.0, 1.0);
}

/// -32768 and 32767 are both full travel, so each side scales by its own end.
double stick(Sint16 value) {
  if (value < 0) return std::max(-1.0, static_cast<double>(value) / -static_cast<double>(SDL_JOYSTICK_AXIS_MIN));
  return std::min(1.0, static_cast<double>(value) / SDL_JOYSTICK_AXIS_MAX);
}

}  // namespace

GamepadRegistry& GamepadRegistry::shared() {
  static GamepadRegistry registry;
  return registry;
}

void GamepadRegistry::readStandard(SDL_Gamepad* pad, std::array<double, kStandardGamepadButtons>& buttons,
                                   std::array<double, kStandardGamepadAxes>& axes) {
  // Index -> SDL button; the triggers (6, 7) are axes and filled in below.
  static constexpr SDL_GamepadButton kButtons[kStandardGamepadButtons] = {
      SDL_GAMEPAD_BUTTON_SOUTH,      SDL_GAMEPAD_BUTTON_EAST,           SDL_GAMEPAD_BUTTON_WEST,
      SDL_GAMEPAD_BUTTON_NORTH,      SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
      SDL_GAMEPAD_BUTTON_INVALID,    SDL_GAMEPAD_BUTTON_INVALID,        SDL_GAMEPAD_BUTTON_BACK,
      SDL_GAMEPAD_BUTTON_START,      SDL_GAMEPAD_BUTTON_LEFT_STICK,     SDL_GAMEPAD_BUTTON_RIGHT_STICK,
      SDL_GAMEPAD_BUTTON_DPAD_UP,    SDL_GAMEPAD_BUTTON_DPAD_DOWN,      SDL_GAMEPAD_BUTTON_DPAD_LEFT,
      SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_GUIDE};
  for (int i = 0; i < kStandardGamepadButtons; ++i) {
    buttons[i] = kButtons[i] == SDL_GAMEPAD_BUTTON_INVALID ? 0.0
                 : SDL_GetGamepadButton(pad, kButtons[i]) ? 1.0
                                                          : 0.0;
  }
  buttons[6] = trigger(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
  buttons[7] = trigger(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
  axes[0] = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
  axes[1] = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
  axes[2] = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX));
  axes[3] = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
}

std::string GamepadRegistry::standardId(const char* name, std::uint16_t vendor, std::uint16_t product) {
  char ids[64];
  std::snprintf(ids, sizeof ids, " (STANDARD GAMEPAD Vendor: %04x Product: %04x)", vendor, product);
  return std::string(name != nullptr && name[0] != '\0' ? name : "Unknown Gamepad") + ids;
}

bool GamepadRegistry::connect(SDL_Gamepad* pad, GamepadConnection& out) {
  if (pad == nullptr) return false;
  const SDL_JoystickID instance = SDL_GetGamepadID(pad);
  if (instance == 0) return false;
  for (Open& entry : open_) {
    if (entry.instance == instance) {
      ++entry.refs;
      out = entry.connection;
      return true;
    }
  }

  GamepadSlot slot;
  slot.serial = nextSerial_++;
  slot.instance = instance;
  slot.id = standardId(SDL_GetGamepadName(pad), SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad));
  slot.rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
  slot.timestampMs = epochMs();
  readStandard(pad, slot.buttons, slot.axes);

  Open entry;
  entry.pad = pad;
  entry.instance = instance;
  entry.refs = 1;
  entry.slotted = true;
  entry.connection.serial = slot.serial;
  entry.connection.id = slot.id;
  entry.connection.rumble = slot.rumble;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // The lowest free slot, as a browser assigns `index`.
    std::size_t index = 0;
    while (index < slots_.size() && slots_[index].serial != 0) ++index;
    if (index == slots_.size()) slots_.emplace_back();
    entry.connection.index = static_cast<int>(index);
    slots_[index] = std::move(slot);
  }
  out = entry.connection;
  open_.push_back(std::move(entry));
  return true;
}

bool GamepadRegistry::disconnect(SDL_JoystickID instance, GamepadConnection& out) {
  for (auto it = open_.begin(); it != open_.end(); ++it) {
    if (it->instance != instance) continue;
    out = it->connection;
    if (it->slotted) {
      it->slotted = false;
      std::lock_guard<std::mutex> lock(mutex_);
      slots_[it->connection.index] = GamepadSlot{};
      while (!slots_.empty() && slots_.back().serial == 0) slots_.pop_back();
    }
    if (--it->refs <= 0) open_.erase(it);
    return true;
  }
  return false;
}

void GamepadRegistry::poll() {
  std::array<double, kStandardGamepadButtons> buttons;
  std::array<double, kStandardGamepadAxes> axes;
  for (const Open& entry : open_) {
    if (!entry.slotted) continue;
    // SDL is read outside the lock, so the JS thread never waits on it.
    readStandard(entry.pad, buttons, axes);
    std::lock_guard<std::mutex> lock(mutex_);
    GamepadSlot& slot = slots_[entry.connection.index];
    if (slot.buttons == buttons && slot.axes == axes) continue;
    slot.buttons = buttons;
    slot.axes = axes;
    // Strictly later on every change, even within one clock tick or across a
    // wall-clock step back: the shim refreshes a pad only when its stamp moves.
    const double now = epochMs();
    slot.timestampMs = now > slot.timestampMs ? now : slot.timestampMs + 0.001;
  }
}

std::size_t GamepadRegistry::read(std::vector<GamepadSlot>& out, bool claim) {
  if (claim) claimed_.store(true, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(mutex_);
  out.resize(slots_.size());
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    const GamepadSlot& from = slots_[i];
    GamepadSlot& to = out[i];
    // The id only changes with the connection; copying it every frame would
    // allocate every frame.
    if (to.serial != from.serial) to.id = from.id;
    to.serial = from.serial;
    to.instance = from.instance;
    to.rumble = from.rumble;
    to.timestampMs = from.timestampMs;
    to.buttons = from.buttons;
    to.axes = from.axes;
  }
  return slots_.size();
}

}  // namespace screenkit
