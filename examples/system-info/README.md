# System info

What the device is, on screen: CPU, GPU and memory. No engine and no assets --
the smallest ScreenKit app in this repo, and a fair exercise of the canvas text
path, since it draws its panel with `fillText` and uploads that canvas to WebGL
as a texture (which is what Pixi and Phaser do underneath).

```sh
npm install
npm run dev                # in a browser, http://localhost:5180
npm run build:screenkit    # -> app.skpkg/
../../runtime/build/macos/screenkit-host --window app.skpkg
```

On a Raspberry Pi it is a Ports entry like any other app:

```sh
APPS="system-info" sh tools/batocera/pi.sh apps
APPS="phaser-stress system-info" sh tools/batocera/pi.sh push
```

**Where the numbers come from.** The GPU lines are the WebGL context's own
(`RENDERER`, `VENDOR`, `VERSION`, limits, extension count). The CPU and memory
lines come from `system.json` in the package, which `pi-prelaunch.sh` rewrites
once a second from `/proc/cpuinfo`, `/proc/meminfo`, `/sys/class/thermal` and
`vcgencmd`; `pi.sh` copies that script into the package and its launcher runs it
beside the app. Anywhere without a sampler -- a Mac -- the panel says so and
still shows everything WebGL knows.

It repaints once a second rather than every frame: a screen of canvas text costs
more than a frame on a Pi 3 (`EMBEDDED_LINUX_EXPERIMENTS.md`).
