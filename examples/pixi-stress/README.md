# PixiJS stress test

How much an ordinary PixiJS 8 app can draw before the frame rate drops -- on ScreenKit (a Mac, a
Raspberry Pi on Batocera) or in a browser.
`src/main.js` builds the scene with plain Pixi (`Sprite`, `Text`, the ticker); `src/stress.js` runs
the measurement, and is the same file as in `../phaser-stress`, so the two engines' numbers compare.

```sh
npm install
npm run dev               # the browser build, http://localhost:5178 -- results in the devtools console
npm run build:screenkit   # vite build (vite.screenkit.config.js) -> dist-screenkit/, screenkit bundle -> app.skpkg/
../../runtime/build/macos/screenkit-host --window app.skpkg
```

On a Raspberry Pi running Batocera: `sh tools/batocera/pi.sh bench pixi-stress` (see
`tools/batocera/README.md`), or launch `screenkit-pixi-stress` from the Ports menu. In a browser,
`?phases=platformer,shooter`, `?target=30` and `?webgl=2` narrow a run; a ScreenKit build takes the
same choices as `VITE_PHASES`, `VITE_TARGET` and `VITE_WEBGL` at build time.

## What it measures

Seven phases, each a kind of game, each loaded until the frame rate drops. `src/stress.js` has the
procedure and the game logic both engines share; `src/main.js` builds each scene the way a PixiJS
game would.

| Phase | Scene | Load |
|---|---|---|
| sprites | bunnymark: bouncing, spinning, tinted sprites | sprites |
| platformer | a 400-tile scrolling tilemap, two parallax layers, a running player | enemies with gravity and tile collisions |
| shooter | five turrets spraying rotating bullet patterns, additive blending | live bullets |
| particles | explosion bursts that fall and fade, additive | live particles |
| puzzle | a match-3 board: every gem bobbing, neighbours swapping | gems |
| vector | units with health bars, redrawn with Graphics every frame | shapes |
| text | canvas Text changing every frame | texts |

The target is 90% of the display's rate, capped at 54 fps. A phase starts light and grows ~30% a
second while the target holds; its result is the largest load that held. A starting load that does not
hold is halved until one does, so a slow device still gets a number, and **0 means the scene itself is
too much** there. Every window is logged as `stress(pixi): ...`, then one `result {json}` line.
The status line changes only between phases: redrawing canvas text every second would be measured as
part of the load on a slow device.

Loads start low (250 sprites, 10 enemies, 2 texts) so a Pi finishes each phase in seconds, and
everything stays within a Pi 3's GPU: WebGL1, power-of-two textures no larger than 256 px
(`node ../scripts/make-stress-assets.cjs public/assets` regenerates them).

## Results

| Where | sprites | platformer | shooter | particles | puzzle | vector | text |
|---|---|---|---|---|---|---|---|
| Raspberry Pi 3 B+, Batocera 42, 640x480 on a 60 Hz TV, WebGL1 (VideoCore IV), 2026-09-17 | 125 | 0 | 100 | 325 | 144 | 4 | 0 |
| the same at 1280x720 | 125 | 0 | 100 | 325 | 144 | 5 | 0 |

On the Pi the CPU is the limit, which is why a quarter of the pixels changes so little: Hermes
interprets bytecode on four Cortex-A53 cores. The platformer
never holds the target even with one enemy, because every visible tile is handled in JavaScript each
frame; one canvas text changing every frame costs more than a frame.
