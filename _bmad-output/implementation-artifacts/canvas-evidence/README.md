# Composited canvases: manual acceptance evidence

The acceptance criterion this covers (`spec-m6-composited-canvases.md`): *a page
with a game canvas and a HUD canvas above it, on macOS, the tvOS simulator, the
Android emulator and the Pi, with both on screen in z-order and the HUD's
transparent pixels showing the game through.*

`two-canvas.js` is the page: a first canvas with no CSS rect, which is therefore
the page's frame and animates a blue clear, and a second canvas placed at
`left:10%; top:10%; width:50%; height:50%; z-index:2`, which is therefore a layer
with a GL context and a drawing buffer of its own. The HUD clears its whole
buffer transparent and then fills its left half orange under a scissor, so what
shows through the right half is the game's own frame and nothing else. The log
line it prints on the way in says what it got:

```
two-canvas: hud context = true, distinct = true, hud buffer = 320x180, drawable = 640x360
```

-- a distinct context, and a drawing buffer that is the rect's pixels rather than
the screen's.

| File | Target |
|---|---|
| `macos-hud-over-game.png` | macOS, windowed host, `--size 640x360` on a 2x display |

Captured with the shipping host, not a test harness:

```sh
hermesc -emit-binary -O -Xes6-block-scoping -out two-canvas.hbc two-canvas.js
SCREENKIT_WINDOW_CAPTURE=macos-hud-over-game.png SCREENKIT_WINDOW_CAPTURE_DELAY_MS=2500 \
  runtime/build/macos/screenkit-host --window --size 640x360 two-canvas.hbc
```

`SCREENKIT_WINDOW_CAPTURE` and not `SCREENKIT_CAPTURE`: the frame capture reads
the *page's own* framebuffer before the present, and the composite happens inside
it, so a GL capture shows the game alone. The window capture is what the screen
shows.

**The other three targets have not run this.** The tvOS-simulator build is green
and the Android and Linux hosts build, but no device or simulator run has
happened, and on the SDL backends (`GlSurfaceSdl.cpp`) the canvas-layer path has
never executed -- `deferred-work.md`. What stands in for them meanwhile is
`canvas-composite`, which reads these same pixels out of the page's own
framebuffer under ctest, and `canvas-over-iframe`, which does it for a HUD canvas
over an `<iframe>` instance.
