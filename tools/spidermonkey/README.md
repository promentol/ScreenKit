# SpiderMonkey for the Linux runtime

The runtime's second engine (`SCREENKIT_ENGINE=spidermonkey`, Linux only): JSI over SpiderMonkey, Firefox's JavaScript engine,
JIT included. Every binding is written against JSI, so the GL, timers, net, text and media run on it unchanged. The runtime
side is `runtime/core/src/spidermonkey/`; this directory is the engine itself and the compiler.

| | |
|---|---|
| `fetch.sh` | Debian 13's `libmozjs-128` (SpiderMonkey 128.14 ESR), both packages pinned by sha256, unpacked into `dist/<target>/` for the device build |
| `Dockerfile` | the same library installed natively (Debian 13, arm64): for the JSI conformance suite and for `screenkit-smc` |
| `smc/`, `smc.sh` | `screenkit-smc`, which compiles a script to a **stencil** ahead of time, run in that container |

```sh
sh tools/spidermonkey/fetch.sh                                   # the library the device build links
sh runtime/tests/jsi/run.sh                                      # JSI's own conformance suite + the stencil tests
sh tools/spidermonkey/smc.sh [--eager] in.js out.stencil         # compile one script
ENGINE=spidermonkey sh tools/batocera/pi.sh build apps push      # the Pi, as a parallel installation (-sm launchers)
```

## Why Debian's library

Nobody publishes SpiderMonkey for embedders, and building it needs Rust and a Firefox source tree. Debian's arm64
`libmozjs-128` needs glibc 2.38 and GLIBCXX_3.4.30 at most, and Batocera 42 has 2.40 and 3.4.32, so it runs on the Pi as
it is. It carries its own ICU (complete Intl, unlike Hermes' Linux backend) and depends on nothing else but zlib. It ships
in `lib/` beside the host; the host is built in the usual Debian 12 container against the unpacked headers.

## Stencils: SpiderMonkey's bytecode

A stencil is SpiderMonkey's compiled form of a script -- bytecode, scopes, atoms, function data -- serialised with its
XDR format (`JS::EncodeStencil` / `JS::DecodeStencil`), the same thing Firefox keeps in its startup cache. Loading one
skips parsing and bytecode generation.

- **Lazy** (the default): top-level code is compiled; an inner function is compiled from the source the stencil carries
  on its first call. Phaser's 1.4 MB bundle: a 2.0 MB stencil.
- **Eager** (`--eager`): every function compiled ahead; nothing is parsed on the device. Phaser: 5.3 MB.
- **Run in place.** The runtime decodes with `borrowBuffer` and `usePinnedBytecode`, so the engine executes bytecode
  straight out of the loaded file instead of copying it -- the arrangement Hermes has with a mapped `.hbc`. The engine
  may point into it until it shuts down, so the file stays loaded for the life of the process.
- **One engine build.** A stencil loads only in the SpiderMonkey build that wrote it. Ours records a build id (the
  engine's version and pointer size) in its header, and the runtime refuses any other with a message rather than
  handing it to the engine. That is why `screenkit-smc` runs against the pinned library in Docker, and why a package
  keeps its source beside the stencil as the fallback.
- JIT code is never precompiled -- by SpiderMonkey or Hermes. The JIT warms up on each launch.

`screenkit bundle --spidermonkey[=source|lazy|eager]` (or `SCREENKIT_SPIDERMONKEY=` from an npm script) adds `app.js`
and `app.stencil` to a package beside `app.hbc` and records them in its manifest:

```json
"engines": { "spidermonkey": { "source": "app.js", "stencil": "app.stencil" } }
```

A SpiderMonkey host runs the stencil when it can load it and the source otherwise; a Hermes host ignores both. The DOM
shim is precompiled the same way at build time (`dom-shim.stencil` beside the host).

## What is different from Hermes

- **One runtime per thread.** SpiderMonkey has one `JSContext` per thread; a second runtime on a thread that has one is
  refused with an error. The host runs one per JS thread, so this never arises there; three conformance tests that keep
  two alive on one thread are skipped for this reason (`runtime/tests/jsi/run.sh`).
- **RTTI.** libmozjs is built without it, and the runtime subclasses one of its classes (the host-object proxy
  handler), so `SpiderMonkeyRuntime.cpp` is compiled with `-fno-rtti`.
- **Tuning from the environment:** `SCREENKIT_SM_JIT=off|baseline|on`, `SCREENKIT_SM_NURSERY_KB`,
  `SCREENKIT_SM_INCREMENTAL_GC=1` (`runtime/core/src/spidermonkey/SpiderMonkeyEngine.cpp`).
- **Unhandled promise rejections** are reported through the engine's own tracker, not `HermesInternal`.
