// Copyright (c) ScreenKit contributors. MIT.
#include "RemotePresses.h"

#import <UIKit/UIKit.h>
#import <objc/runtime.h>

#include <string>

#include <SDL3/SDL.h>

#include <screenkit/Log.h>

namespace screenkit {
namespace {

constexpr const char* kTag = "screenkit.input";

/// SDL_uikitview.m's scancodeFromPress: for presses with no key -- the same
/// table, so a forwarded press is the event SDL itself would have sent.
SDL_Scancode scancodeForRemote(UIPressType type) {
  switch (type) {
    case UIPressTypeUpArrow:
      return SDL_SCANCODE_UP;
    case UIPressTypeDownArrow:
      return SDL_SCANCODE_DOWN;
    case UIPressTypeLeftArrow:
      return SDL_SCANCODE_LEFT;
    case UIPressTypeRightArrow:
      return SDL_SCANCODE_RIGHT;
    case UIPressTypeSelect:
      return SDL_SCANCODE_RETURN;
    case UIPressTypeMenu:
      return SDL_SCANCODE_ESCAPE;
    case UIPressTypePlayPause:
      return SDL_SCANCODE_PAUSE;
    default:
      return SDL_SCANCODE_UNKNOWN;
  }
}

void forwardRemotePresses(NSSet<UIPress*>* presses, bool down) {
  // With no keyboard SDL has already sent these; with one, it dropped them.
  if (!SDL_HasKeyboard()) return;
  for (UIPress* press in presses) {
    if (press.key != nil) continue;  // a keyboard key: GCKeyboard reports it
    const SDL_Scancode scancode = scancodeForRemote(press.type);
    if (scancode == SDL_SCANCODE_UNKNOWN) continue;
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.timestamp = SDL_GetTicksNS();
    event.key.scancode = scancode;
    event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
    event.key.down = down;
    if (!SDL_PushEvent(&event)) {
      log(LogLevel::Warn, kTag, std::string("could not queue a remote press: ") + SDL_GetError());
    }
  }
}

/// Replace `selector` on `cls` with SDL's implementation followed by the forward.
bool wrap(Class cls, SEL selector, bool down) {
  Method method = class_getInstanceMethod(cls, selector);
  if (method == nullptr) return false;
  using Handler = void (*)(id, SEL, NSSet<UIPress*>*, UIPressesEvent*);
  const auto original = reinterpret_cast<Handler>(method_getImplementation(method));
  IMP wrapped = imp_implementationWithBlock(^(id self, NSSet<UIPress*>* presses, UIPressesEvent* event) {
    original(self, selector, presses, event);
    forwardRemotePresses(presses, down);
  });
  method_setImplementation(method, wrapped);
  return true;
}

}  // namespace

void installRemotePressForwarding() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  Class view = NSClassFromString(@"SDL_uikitview");
  if (view == nil) {
    log(LogLevel::Warn, kTag, "SDL_uikitview not found: the remote does nothing while a keyboard is attached");
    return;
  }
  const bool ok = wrap(view, @selector(pressesBegan:withEvent:), true) &&
                  wrap(view, @selector(pressesEnded:withEvent:), false) &&
                  wrap(view, @selector(pressesCancelled:withEvent:), false);
  if (!ok) {
    log(LogLevel::Warn, kTag, "SDL_uikitview's press handlers moved: the remote does nothing while a keyboard is attached");
  }
}

}  // namespace screenkit
