# `<iframe>` instances: manual acceptance evidence

The acceptance criterion this covers (`spec-m9-iframe-instances.md`): *a launcher
page with an `<iframe>` whose `src` is a game package renders the game inside the
iframe's rect with the launcher's own UI around it, on macOS, the tvOS simulator,
the Android emulator and the Pi.*

The package used is a launcher built for the check: it clears its own canvas
magenta every frame and embeds `games/app.skpkg` -- a real PixiJS app, the
unmodified `poc/pixi-hello` package -- in an `<iframe>` inset 12% / 14%. So the
magenta is the launcher's own frame, and everything inside the dark rect is a
second app on its own runtime, thread and GL context, composited into the
launcher's frame by the compositor. Neither app knows the other is there.

| File | Target |
|---|---|
| `macos-launcher-embeds-game.png` | macOS, windowed host, 2560x1504 |
| `tvos-simulator-launcher-embeds-game.png` | Apple TV 4K simulator, 3840x2160 |
| `android-emulator-launcher-embeds-game.png` | Android TV emulator, 1920x1080 |

All three were re-taken against the patched build after the review round, so
what they show is what ships.

The Pi shot is missing: the Linux host is a Docker cross-build and the Docker
engine would not start on this machine. That leaves the Pi without any coverage
of this change at all -- the `iframe-*` rows deliberately do not run on device
(they need a drawable the test binary has not), so this manual check is the only
device coverage there is.
