---
title: 'Input: remotes and gamepads'
description: Every remote arrives as a keyboard event; gamepads arrive as keys until your app claims them through the Gamepad API.
---

Every platform's input arrives as **DOM keyboard events**, because SDL has already normalised the
hardware:

| Hardware | Arrives as |
|---|---|
| tvOS Siri Remote | arrows, `Enter` (select), `Escape` (Menu) |
| Android TV / Fire TV remote | arrows, `Enter` (D-pad centre), `AC Back`, media keys |
| macOS / Linux keyboard | a keyboard |
| Game controllers | keys, until you claim them (below) |

Each becomes a `keydown` / `keyup` with `key`, `code` and the legacy `keyCode` that TV frameworks
still key on — Blits and Lightning read 37–40 for the arrows, 13 for enter and 8 for back.

```js
window.addEventListener('keydown', (e) => {
  switch (e.key) {
    case 'ArrowRight': focusNext(); break
    case 'Enter':      activate();  break
    case 'Escape':     goBack();    break   // tvOS Menu, Android Back
  }
})
```

## Gamepads

Gamepads are the W3C Gamepad API in the `standard` mapping. `gamepadconnected` and
`gamepaddisconnected` are trusted `GamepadEvent`s at `window`, fired as tasks that a pause holds
rather than drops — a controller connecting is a fact the page needs after it resumes.

```js
window.addEventListener('gamepadconnected', (e) => {
  console.log(e.gamepad.index, e.gamepad.id)
})

function frame () {
  const [pad] = navigator.getGamepads()
  if (pad?.buttons[0].pressed) jump()
  requestAnimationFrame(frame)
}
```

:::caution[The claim is one-way and process-wide]
Your app's **first** `navigator.getGamepads()` claims the gamepads for the whole process. From then
on, gamepad buttons and sticks produce no key events, and whatever keys they were holding are
released. Keyboards and remotes are unaffected, and there is no way back.

This is what you want if you read pads directly — and a surprise if you called `getGamepads()` to
feature-detect. Don't probe with it.
:::

## Input follows focus

Keys go to the focused browsing context and to nothing else. When a launcher calls `focus()` on an
`<iframe>`, the launcher stops receiving input and the instance starts. A focus switch releases keys
that were held, the way a controller disconnecting mid-press does, so no context is left with a key
down it will never see go up.

## Leaving the app

`window.close()` calls the host's exit path where the host defines one — the Android host finishes the
activity. On tvOS, the Menu button at the app's root does not yet exit to the Home screen; the
JavaScript-visible way out is the app's own `window.close()` call rather than a guess about Back.
