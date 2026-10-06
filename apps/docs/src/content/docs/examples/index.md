---
title: Examples
description: Nine real apps in examples/ — what each one proves about the runtime, and what it looked like when it ran.
sidebar:
  order: 0
---

Every app in `examples/` is a real web project, built with Vite and packed with `screenkit bundle`.
They exist to answer one question each, and they are the fleet the device scripts run — so when a
page here says an app renders, a screenshot from the shipping macOS host says so too.

Every screenshot on these pages was captured by the runtime itself:

```sh
SCREENKIT_CAPTURE=shot.png SCREENKIT_CAPTURE_DELAY_MS=3000 \
  runtime/build/macos/screenkit-host --window --size 1280x720 examples/<app>/app.skpkg
```

That reads the default framebuffer after the frame's GL work has run and before it is presented —
what the screen is about to show. No screen recording, no simulator, no window manager in the way.

## The fleet

| Example | What it answers |
|---|---|
| [PixiJS hello world](/examples/pixi-hello/) | does an ordinary Pixi 8 app run unmodified? |
| [Phaser hello world](/examples/phaser-hello/) | the same for Phaser 4, with a bitmap font |
| [three.js cube](/examples/threejs-cube/) | does WebGL 2 work, lit and depth-tested? |
| [Lightning 3 + Blits](/examples/lightning3-blits/) | the M6 milestone: unmodified Lightning renders a UI with text |
| [Lightning 3 shaders](/examples/lightning3-shaders/) | eight shader programs, uniform arrays, a per-frame timed shader |
| [Blits example app](/examples/blits-example-app/) | a whole upstream app, `src/` untouched |
| [Pixi stress](/examples/pixi-stress/) | how much can it draw before the frame rate drops? |
| [Phaser stress](/examples/phaser-stress/) | the same measurement, same harness, other engine |
| [System info](/examples/system-info/) | what is this device, according to the runtime? |

## Running one

```sh
npm run build:screenkit -w screenkit-example-pixi-hello
runtime/build/macos/screenkit-host --window examples/pixi-hello/app.skpkg
```

Or on hardware, where the script takes the directory name:

```sh
sh tools/android/android.sh run pixi-hello
sh tools/batocera/pi.sh run lightning3-blits
```

:::note[What these screenshots are not]
All nine were captured on macOS, where GL is ANGLE over Metal. They show that the app runs and what
it draws — they are not evidence about tvOS, Android or the Pi. What has and has not run on each
target is in [Targets](/reference/targets/).
:::
