# Lightning 3 shaders

A shader showcase for ScreenKit: every WebGL shader Blits registers, plus a
custom one, one tile each. It puts more on the GPU than the hello world. There
are eight different shader programs, uniform arrays (gradient stops and colours),
reactive shader props, and a timed shader that redraws every frame.

```sh
npm ci
npm run dev               # http://localhost:5175 -- the reference rendering
npm run build:screenkit   # dist-screenkit/, then app.skpkg/
```

On ScreenKit, run the package, as in `examples/lightning3-blits`:

```sh
SCREENKIT_APP_PKG=app.skpkg ../../runtime/scripts/build-tvos-simulator.sh          # tvOS
xcrun simctl install booted ../../runtime/build/tvos-simulator/screenkit-host.app
xcrun simctl launch booted dev.screenkit.host
../../runtime/build/macos/screenkit-host --window app.skpkg                          # macOS
```

## What is on screen

| Tile | How it is asked for | Renderer shader |
|---|---|---|
| rounded | `rounded="48"` | `rounded` |
| border | `border="{w: 10, color: ...}"` | `border` |
| rounded + shadow | `rounded` + `shadow="{color, projection: [x, y, blur, spread]}"` | `roundedWithShadow` |
| rounded + border + shadow | all three attributes | `roundedWithBorderAndShadow` |
| linearGradient | `:shader="{type: 'linearGradient', angle: $angle, colors: [...]}"` | `linearGradient`, angle is reactive state |
| radialGradient | `shader="{type: 'radialGradient', colors, w, h}"` | `radialGradient` |
| holePunch | `shader="{type: 'holePunch', x, y, w, h, radius}"` | `holePunch` |
| custom: plasma | `shader="{type: 'plasma', speed, colorA, colorB}"` | `src/shaders/Plasma.js` |

The two moving tiles take different paths through the renderer. The gradient's
`angle` is Blits state changed by a timer, so each change goes through
shader-prop reactivity into `update()` and then the uniforms. The plasma is a
*timed* shader (it has a `time` hook), so the renderer tracks its node, keeps
rendering every frame, and feeds `u_time`.

## Writing a custom shader

`src/shaders/Plasma.js` is the template, and it has comments on each part:
`props` with defaults, `update()` to turn props into uniforms, an optional `time`
hook, and a fragment shader that relies on the renderer's default vertex shader
for `v_nodeCoords` and friends. You register it in `src/index.js` under
`shaders: [{ name, type }]`.

One renderer 3.3.1 quirk is worth knowing. `u_time` is only fed when the program
also *uses* `u_dimensions`, because `WebGlShaderProgram` checks the wrong uniform
before enabling time. A timed shader that never reads `u_dimensions` gets
optimised down to a program where that check fails, and `u_time` stays 0.

## Verified

On the tvOS simulator (Apple TV 4K) it runs at 60 fps with no errors in the log.
Eleven sampled pixels match the Chromium rendering exactly: background, every
static tile, border edges, the shadow, the radial centre and corner, and inside
and outside the hole. The linear gradient and the plasma change between
screenshots taken a second apart. The macOS host runs it at the display's 120 Hz.

The ScreenKit build is the same `@screenkit/vite-plugin` + `screenkit bundle` pipeline as the hello
world, with the same Lightning fixes. This app needed no new runtime work.
