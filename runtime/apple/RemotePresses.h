// Copyright (c) ScreenKit contributors. MIT.
#pragma once

namespace screenkit {

/// tvOS: keep the Siri Remote working while a keyboard is attached.
///
/// SDL 3.4's UIKit view turns a remote press into a key event only while no
/// keyboard is connected (`if (!SDL_HasKeyboard())` in SDL_uikitview.m), because
/// a keyboard's keys also arrive as presses and GCKeyboard already reports them.
/// Attach one -- a Bluetooth keyboard, or the simulator's Connect Hardware
/// Keyboard -- and the remote's arrows, Select and Menu go nowhere. This wraps
/// the view's press handlers: SDL's run first, unchanged, and while a keyboard is
/// attached every press that is not a keyboard key (`UIPress.key` is nil) is
/// pushed as the key event SDL would have sent, mapped the way SDL maps it.
///
/// Call once, after SDL_Init and before the window exists. Logs and does nothing
/// if SDL's view class or its handlers are not where SDL 3.4 has them.
void installRemotePressForwarding();

}  // namespace screenkit
