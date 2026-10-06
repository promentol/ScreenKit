// Copyright (c) ScreenKit contributors. MIT.
#include "Gamepads.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

#include "input/Gamepads.h"

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

constexpr double kRumbleMaxMs = 5000;

static_assert(kGamepadSlotValues == 3 + kStandardGamepadButtons + kStandardGamepadAxes,
              "the slot layout the DOM shim reads");

[[noreturn]] void fail(jsi::Runtime& rt, const char* fn, const std::string& why) {
  throw jsi::JSError(rt, std::string("__screenkit.gamepads.") + fn + ": " + why);
}

double number(jsi::Runtime& rt, const jsi::Value* args, std::size_t count, std::size_t i, const char* fn) {
  if (i >= count || !args[i].isNumber()) fail(rt, fn, "argument " + std::to_string(i + 1) + " must be a number");
  return args[i].asNumber();
}

/// A finite number clamped to [low, high]; NaN is `low`.
double clamped(double value, double low, double high) {
  if (!(value >= low)) return low;
  return std::min(value, high);
}

void method(jsi::Runtime& rt, jsi::Object& target, const char* name, unsigned arity,
            jsi::HostFunctionType body) {
  target.setProperty(rt, name,
                     jsi::Function::createFromHostFunction(rt, jsi::PropNameID::forAscii(rt, name), arity,
                                                           std::move(body)));
}

/// The slot `index` names in the last read, or null.
const GamepadSlot* slotAt(const std::vector<GamepadSlot>& slots, double index) {
  if (!(index >= 0) || index >= static_cast<double>(slots.size()) || index != std::floor(index)) return nullptr;
  const GamepadSlot& slot = slots[static_cast<std::size_t>(index)];
  return slot.serial == 0 ? nullptr : &slot;
}

}  // namespace

void installGamepads(jsi::Runtime& runtime) {
  jsi::Value host = runtime.global().getProperty(runtime, "__screenkit");
  if (!host.isObject()) {
    throw jsi::JSError(runtime, "installGamepads: __screenkit is missing -- installHostIO runs first");
  }

  // The caller-owned storage the registry is read into: reused on every read,
  // so a frame's getGamepads() allocates nothing once the pads are known.
  auto slots = std::make_shared<std::vector<GamepadSlot>>();
  jsi::Object api(runtime);

  method(runtime, api, "read", 2,
         [slots](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) -> jsi::Value {
           if (count < 1 || !args[0].isObject() || !args[0].getObject(rt).isArrayBuffer(rt)) {
             fail(rt, "read", "argument 1 must be an ArrayBuffer");
           }
           const bool claim = count > 1 && args[1].isBool() && args[1].getBool();
           const std::size_t total = GamepadRegistry::shared().read(*slots, claim);

           jsi::ArrayBuffer buffer = args[0].getObject(rt).getArrayBuffer(rt);
           const std::size_t fits = buffer.size(rt) / (sizeof(double) * kGamepadSlotValues);
           std::uint8_t* out = buffer.data(rt);
           double values[kGamepadSlotValues];
           for (std::size_t i = 0; i < total && i < fits; ++i) {
             const GamepadSlot& slot = (*slots)[i];
             values[0] = slot.serial;
             values[1] = slot.timestampMs;
             values[2] = slot.rumble ? 1 : 0;
             std::copy(slot.buttons.begin(), slot.buttons.end(), values + 3);
             std::copy(slot.axes.begin(), slot.axes.end(), values + 3 + kStandardGamepadButtons);
             // memcpy, not a cast: nothing promises the bytes are aligned for a double.
             std::memcpy(out + i * sizeof values, values, sizeof values);
           }
           return static_cast<double>(total);
         });

  method(runtime, api, "id", 1,
         [slots](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) -> jsi::Value {
           const GamepadSlot* slot = slotAt(*slots, number(rt, args, count, 0, "id"));
           if (slot == nullptr) return jsi::Value::undefined();
           return jsi::String::createFromUtf8(rt, slot->id);
         });

  method(runtime, api, "rumble", 4,
         [slots](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) -> jsi::Value {
           const GamepadSlot* slot = slotAt(*slots, number(rt, args, count, 0, "rumble"));
           auto strong = static_cast<Uint16>(std::lround(clamped(number(rt, args, count, 1, "rumble"), 0, 65535)));
           auto weak = static_cast<Uint16>(std::lround(clamped(number(rt, args, count, 2, "rumble"), 0, 65535)));
           const auto ms = static_cast<Uint32>(std::lround(clamped(number(rt, args, count, 3, "rumble"), 0, kRumbleMaxMs)));
           if (slot == nullptr || !slot->rumble) return false;
           // SDL keeps a rumble with no duration going until it is told otherwise;
           // an effect with none is a stop.
           if (ms == 0) strong = weak = 0;
           // The one SDL call off the main thread. The lock holds the pad in
           // place between the lookup and the call; a pad that has gone since
           // the last read is simply not found.
           SDL_LockJoysticks();
           SDL_Gamepad* pad = SDL_GetGamepadFromID(slot->instance);
           const bool ok = pad != nullptr && SDL_RumbleGamepad(pad, strong, weak, ms);
           SDL_UnlockJoysticks();
           return ok;
         });

  host.getObject(runtime).setProperty(runtime, "gamepads", std::move(api));
}

}  // namespace screenkit
