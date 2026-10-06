// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <jsi/jsi.h>

namespace screenkit {

/// Values per slot in the buffer `__screenkit.gamepads.read` fills: serial,
/// timestamp, flags, 17 buttons, 4 axes.
inline constexpr int kGamepadSlotValues = 3 + 17 + 4;

/// Install `__screenkit.gamepads`, what the DOM shim builds the W3C Gamepad API
/// from (runtime/js/README.md, "Gamepads"). The state is the process's
/// GamepadRegistry (input/Gamepads.h), which the main thread keeps current;
/// nothing here reads SDL except rumble.
///
///   read(buffer: ArrayBuffer, claim: boolean) -> slot count
///       Copies the snapshot and writes it into `buffer` as doubles, one
///       kGamepadSlotValues run per slot while they fit:
///         [0] connection serial, 0 for an empty slot
///         [1] last change, wall-clock ms since the epoch
///         [2] flags: 1 rumble
///         [3..19] the 17 standard buttons, 0..1
///         [20..23] the 4 standard axes, -1..1
///       Returns the slot count even when the buffer is too small for it, so
///       the caller can grow the buffer and read again. `claim` true is
///       `navigator.getGamepads()`: it claims the gamepads from InputRouter.
///   id(index) -> string | undefined
///       The slot's `id` as of the last read.
///   rumble(index, strong, weak, ms) -> boolean
///       SDL_RumbleGamepad on the pad that held `index` at the last read, under
///       SDL_LockJoysticks: strong is the low-frequency motor, weak the high,
///       each 0..65535, for at most 5000 ms. False when the pad is gone or
///       cannot rumble.
///
/// `__screenkit` must already exist (installHostIO). JS thread only.
void installGamepads(facebook::jsi::Runtime& runtime);

}  // namespace screenkit
