# Engine bench: Hermes against SpiderMonkey on a Raspberry Pi

Two small C++ hosts, one per engine, run the same JavaScript and report the same numbers: frame
time, RSS and its peak, the JS heap, GC pauses, CPU, and startup. The JavaScript is Phaser-shaped
work with no Phaser and no graphics. It builds game objects, runs tweens, particles, arcade
physics, a tilemap, bitmap text and Graphics paths, and walks a depth-sorted display list into a
vertex buffer that nothing draws.

```sh
sh poc/engine-bench/bench.sh build     # both hosts and the stencils for linux-arm64, in Docker; staged in build/stage
sh poc/engine-bench/bench.sh local     # run everything in an arm64 container here -- a check, not a measurement
sh poc/engine-bench/bench.sh push      # to the Pi, /userdata/engine-bench
sh poc/engine-bench/bench.sh run       # every config on the Pi, reports in results/, table in results/summary.md
```

`run` reads the firmware's throttle flags (`vcgencmd get_throttled`) before and after, and refuses to
start on a Pi that is under-volted or throttled now. `ALLOW_THROTTLED=1` runs it anyway.

`PI` is the Pi's address (`batocera.local`), and `BENCH_ARGS` passes options to every run:
`BENCH_ARGS="--frames 60 --scenes sprites,ui" sh bench.sh run sm-jit hermes-jit`.

## Results: Raspberry Pi 3 B+, 2026-09-19

> **Caveat, found later the same day:** this Pi's supply under-volts under load. `vcgencmd get_throttled`
> read 0x50005 that evening (under-voltage and firmware throttling, now and since boot) with the ARM at
> 600 MHz, which `scaling_cur_freq` does not show. These runs were consistent with each other, but
> whether any of them ran throttled cannot be told after the fact; re-measure on a sound supply,
> checking `vcgencmd get_throttled` before and after.

Measured on Batocera 42, `scaling_cur_freq` 1.4 GHz and 54 °C throughout, with defaults: 180 frames per
scene and load, frames back to back. All tables are in [`results/summary.md`](results/summary.md)
and the raw reports in `results/*.json`. All five runs ended every segment in the same state.

**Memory: Hermes is about 20 MB smaller at its peak.** Hermes runs with the JIT's library,
SpiderMonkey with every tier:

| MB | hermes-interp | hermes-jit | sm-interp | sm-jit |
|---|---:|---:|---:|---:|
| engine up | 7.2 | 7.4 | 16.1 | 16.1 |
| + 1.4 MB of bulk code | 10.5 | 11.0 | 24.3 | 24.9 |
| peak RSS | 35.8 | 37.0 | 55.8 | 60.0 |
| PSS / private dirty at end | 33.5 / 28.5 | 34.3 / 28.9 | 53.2 / 42.5 | 57.7 / 45.4 |
| RSS after a full GC | 34.5 | 35.8 | 37.3 | 41.9 |

- **Code costs Hermes less than half as much.** Hermes' bytecode is mapped and file-backed:
  1.4 MB of library code added 3.3 MB of RSS to Hermes and 8.2 MB to SpiderMonkey.
- **SpiderMonkey gives memory back; Hermes keeps its heap.** After a full GC SpiderMonkey's
  anonymous memory drops to 24–27 MB. Hermes stays at its 28 MB heap size, so the gap narrows
  to 2–6 MB.
- **What the JITs cost in memory.** Hermes' JIT adds 1.2 MB over its interpreter; SpiderMonkey's
  Ion adds 4 MB over its interpreter.
- **Threads.** Hermes runs with 2 threads, SpiderMonkey with 5.

**Speed: SpiderMonkey's JIT is in another class.** Median frame time relative to the Hermes
interpreter, over all 24 scene/loads:

| | hermes-jit | sm-interp | sm-baseline | sm-jit |
|---|---:|---:|---:|---:|
| object and array code (7 scenes) | 1.3–1.5× | 0.27–0.33× | 1.8–2.2× | 5.3–9.9× |
| float-heavy (vector: ear clipping) | 2.1× | 0.15× | 1.1× | 8.5× |

- **Interpreters:** SpiderMonkey's C++ interpreter is 3–7× slower than Hermes' interpreter.
- **Template JITs:** SpiderMonkey's Baseline JIT beats Hermes' JIT everywhere except the
  float-heavy scene.

What that means in a 60 fps frame (p95 ≤ 16.7 ms; `+` means the largest load tried still fit):

| scene | hermes-interp | hermes-jit | sm-baseline | sm-jit |
|---|---:|---:|---:|---:|
| sprites | 100 | 100 | 400 | 1600+ |
| particles | 250 | 250 | 250 | 1000 |
| platformer (enemies) | 50 | 50 | 200+ | 200+ |
| shooter (bullets) | 100 | 100 | 100 | 400 |
| puzzle (gems) | 64 | 64 | 256 | 1024+ |
| swarm (bodies) | 50 | 50 | 200 | 500+ |
| vector (shapes) | 0 | 3 | 0 | 12 |
| ui (panels) | 5 | 5 | 20 | 80+ |

**Startup: Hermes wins by close to 10×.**

- **The engine:** Hermes starts in 20 ms, SpiderMonkey in 160 ms.
- **The bundle:** loading 1.4 MB of code takes 74–82 ms from bytecode against about 785 ms parsing
  source.
- **The workload:** 18 ms against 60 ms.

**GC: SpiderMonkey pauses more often but for less time.**

- **Collections:** SpiderMonkey made 381–670 collections against Hermes' 91.
- **Longest pause:** 7–12 ms for SpiderMonkey. For Hermes it was 32–35 ms: one young-generation
  collection just after 1,600 sprites were built, two dropped frames at 60 fps.
- **Total pause time is similar:** 263–417 ms against 390 ms.

## Layout

| | |
|---|---|
| `js/workload.js` | the workload: a small Phaser-like engine and eight scenes, plain ES5, one global `bench` |
| `js/gen-bulk.mjs` | generates `bulk.js`, 1.4 MB of library-shaped code, loaded first so each engine carries a game-sized bundle |
| `src/Bench.{h,cpp}` | the engine-neutral host: options, the frame loop, the report |
| `src/Metrics.{h,cpp}` | `/proc` and `/sys` readings, frame statistics, a JSON writer |
| `src/HermesMain.cpp` | `bench-hermes`, through JSI as the ScreenKit runtime embeds Hermes |
| `src/SpiderMonkeyMain.cpp` | `bench-spidermonkey`, mozjs-128 with one global and the internal job queue; loads source or a ScreenKit stencil |
| `docker/spidermonkey.Dockerfile` | Debian 13's SpiderMonkey 128 ESR, the build machine for that host |
| `run.sh` | one config, where it runs (staged beside the binaries) |
| `bench.sh` | build, stage, push, run, compare |
| `compare.mjs` | reports → `summary.md` |

## The configurations

| config | engine | what runs |
|---|---|---|
| `hermes-interp` | the shipping Hermes library (`hermes-linux-arm64`, no JIT compiled in) | interpreter, bytecode from `hermesc -O` |
| `hermes-jit` | the JIT build of the same commit (`hermes-jit-linux-arm64`) | JIT after 32 calls |
| `hermes-jit-force` | same | every function JIT-compiled on first call (what `VARIANT=jit` ships); not run by default |
| `sm-interp` | SpiderMonkey 128.14 ESR (Debian's `libmozjs-128`) | C++ interpreter only |
| `sm-baseline` | same | + Baseline Interpreter and Baseline JIT: template JITs, the closest thing to Hermes' JIT |
| `sm-jit` | same | + Ion/Warp, the optimising JIT, as Firefox runs |
| `sm-<tier>-lazy` | same | that tier, from a lazy stencil |
| `sm-<tier>-eager` | same | that tier, from an eager stencil |

Hermes runs the bytecode `hermesc` compiled with the flags a `.skpkg` uses. Both engines get the
same `bulk` and `workload`.

SpiderMonkey runs them in one of three forms:

- **Source**, parsed on the device.
- **Stencil**, SpiderMonkey's compiled form, written ahead of time by `screenkit-smc`
  (`tools/spidermonkey/smc.sh`) as a `--spidermonkey` package has it. The host loads it the way the
  ScreenKit runtime does: the file is mapped, its build id checked, and its bytecode run in place
  (`borrowBuffer`, `usePinnedBytecode`), so it counts as file-backed RSS. There are two kinds:
  - *lazy*: top-level code compiled; each function compiled from the source it carries on its
    first call (bulk: 2.4 MB);
  - *eager*: every function compiled (bulk: 4.4 MB).

By default `run` covers the interpreter and the full JIT in all three forms, and Baseline from
source only. `sm-baseline-lazy` and `sm-baseline-eager` exist; name them to run them.

A stencil changes how code is loaded, not the code that runs. So the engine tables in
`summary.md` leave the stencil runs out, and a section of its own sets them against source, per
tier:

- load time and the RSS it leaves (anon against file);
- what the first calls cost: building each scene, its first frame, the warm-up frames;
- steady frame time relative to source, which should be 1;
- peak RSS, and RSS after a full GC;
- the time from process start to the first frame.

**Stencils on the Pi 3, 2026-09-20.** The Pi was still under-volted (`0x50005`, ARM held at 600 MHz),
so the absolute times are slow. All three forms ran at the same fixed clock, interleaved, with a warm
page cache. Startup is the median of 5 runs per config (a 100-sprite scene); ms:

| | source | lazy stencil | eager stencil |
|---|---:|---:|---:|
| engine up (no JIT / JIT) | 173 / 182 | 163 / 177 | 168 / 175 |
| load 1.4 MB bulk (no JIT / JIT) | 844 / 901 | 128 / 154 | 191 / 202 |
| load workload (no JIT / JIT) | 69 / 70 | 36 / 34 | 36 / 30 |
| first scene: build + first frame (no JIT / JIT) | 70 / 66 | 71 / 67 | 55 / 47 |
| **process start to first frame** (no JIT / JIT) | **1163 / 1223** | **398 / 433** | **450 / 457** |
| RSS after loading, MB (JIT) | 25.3 | 24.8 | 28.4 |
| steady frame, JIT: best of 3 rounds, geo-mean of 15 segments | 2.88 | 2.87 | 2.89 |

- **Loading gets 2.7–2.9× faster to the first frame.** Parsing the source was 0.85–0.9 s of the
  start; a stencil loads in 0.13–0.2 s.
- **Lazy against eager:**
  - lazy loads fastest;
  - eager decodes more up front, but its functions need no compiling on their first call (the first
    scene is built and drawn about 20 ms sooner);
  - with this bulk code, which the scenes barely call, lazy reaches the first frame slightly sooner;
  - an app that calls much of its code at start favours eager: Phaser's startup went from
    3.6–4.4 s (lazy) to 2.2–2.6 s (eager).
- **Steady frame time does not change.** Once a function has run, the same bytecode is running and
  the JIT compiles it the same way. One full run each first showed stencils at 0.87–0.90× in the
  object-heavy scenes. Three interleaved rounds put that down to noise: on this Pi a single config
  varies by up to 20% between runs.
- **The cost:** an eager stencil adds 3 MB of RSS after loading: 4.5 MB more file-backed (its
  mapped bytecode), 1.4 MB less anonymous. A lazy stencil costs nothing.

Reports: `results/pi-600mhz/` (`jit/` and `interp/` are full runs, `interp` at `--frames 60
--scale 0.25`; `startup/` and `jit-repeat/` are the repeats).

**Stencils in the arm64 container** (`bench.sh local` on an M-series Mac, 60 frames; a check, not a
measurement). Loading 1.4 MB of code took 36 ms from source, 9–11 ms from a lazy stencil and
17–18 ms from an eager one. The eager stencil decodes more up front in exchange for compiling
nothing on first calls. That saving is too small to show on this machine: the Pi has to measure it.
Every run's end state matched source in all 24 segments.

## What the workload does

Each scene is examples/phaser-stress's scene written against a small engine that keeps Phaser's
structure:

- **Game objects** are event emitters. They sit behind accessor properties (`rotation` wraps,
  `alpha` clamps and flags, `depth` queues a sort).
- **The scene** emits `preupdate`, `update` and `postupdate`. It runs an update list, a tween
  manager and an arcade physics world.
- **Rendering** depth-sorts the display list with Phaser's merge sort. For every object it builds
  `TransformMatrix` products and writes four vertices per quad (or three per triangle) into a
  `Float32Array`/`Uint32Array` batch. A draw call is counted where WebGL would take one.

| scene | load | what it leans on |
|---|---|---|
| sprites | 100 / 400 / 1600 sprites | bunnymark: accessor writes, matrix math, the vertex batch |
| particles | 250 / 1000 / 4000 live particles | pooled particles eased over their life, a record per death and `splice` |
| platformer | 10 / 50 / 200 enemies | a 400×23 tilemap of `Tile` objects, culling, bodies against tiles (a fresh array per query), animations by frame name |
| shooter | 100 / 400 / 1600 bullets | bullets allocated on firing and destroyed off-screen (`indexOf` + `splice` from the display list), `filter` every frame, events |
| puzzle | 64 / 256 / 1024 gems | a tween per gem (properties by name, through setters), swap tweens coming and going, depth changes, match scanning |
| swarm | 50 / 200 / 500 bodies | body-against-body: a spatial hash of fresh arrays in a `Map`, pairs in a `Set` |
| vector | 3 / 12 / 48 shapes | Graphics redrawn every frame: a 100-point arc per circle, ear-clipped with a node per vertex |
| ui | 5 / 20 / 80 panels | nested containers, bitmap text laid out every frame (an object per glyph), `toFixed`, JSON save/load |

Time is fixed at 60 steps per game second, whatever the engine's speed. Every engine therefore
does exactly the same work, and each segment ends with a checksum of its state. `compare.mjs`
shows whether the checksums agree.

## What is measured

Per run, in `results/<config>.json`:

- **Startup:** time and memory to start the engine, to load `bulk`, and to load `workload`.
- **Per scene and load:**
  - frame time, split into warm-up frames (the first 30, where JITs compile) and steady frames:
    median, p95, p99, max, and frames over 16.7 ms;
  - the time to build the scene;
  - CPU per frame across all threads (JIT compiler threads, GC helpers);
  - peak RSS sampled every frame, and RSS at the end;
  - collections, total pause, longest pause, background GC time, and the JS heap.
- **End:** RSS, peak RSS (VmHWM), and RSS and heap after a full GC.
- **The machine:** at start and end, the CPU clock and temperature, and on a Pi the firmware's
  throttle flags and real ARM clock (`vcgencmd`). A Pi 3 on a weak supply runs at 600 MHz while
  cpufreq still reports 1.4 GHz. `summary.md` flags any run the firmware reports as slowed.

RSS is split by `RssAnon` and `RssFile`. *Anon* is memory the engine made: heaps, JIT code,
stacks. *File* is resident pages of mapped files: the engine library, and Hermes' bytecode, which
is mapped rather than read.

## Tuning

`--param name=value`, per engine, for RAM experiments without a rebuild:

- **bench-hermes:** `initHeapBytes`, `maxHeapBytes`, `jitThreshold`, `jitMemoryLimit`.
- **bench-spidermonkey:**
  - `maxNurseryBytes` (64 MB by default, a large part of its RSS), `minNurseryBytes`;
  - `incremental` (0/1, off by default in a bare embedding; Firefox turns it on), `sliceMs`;
  - `parallelMarking`, `maxHelperThreads`.

`--pace 60` holds the loop to 60 fps instead of running frames back to back. Idle time between
frames then belongs to the GC and JIT threads, as it would in a game.

## Caveats

- **SpiderMonkey is 128 ESR, Debian's build.** It runs on Batocera 42 as shipped (it needs
  GLIBC_2.38 and GLIBCXX_3.4.30, and the Pi has 2.40 and 3.4.32). A newer ESR means building
  SpiderMonkey, which needs Rust.
- **The engines count their heaps differently.** Compare RSS across engines, not "heap".
  - Hermes' `usedBytes` is everything allocated in its GC heap.
  - SpiderMonkey's is the tenured heap, with the nursery on top.
- **GC pause accounting is each engine's own events.**
  - Hermes: young collections stop the JS thread; Hades' old generation runs on a background
    thread and is counted apart.
  - SpiderMonkey: every slice and nursery collection is a pause. Its helper-thread work shows
    only as CPU.
- **EmulationStation keeps running** during a Pi run, as it did for the Phaser and Pixi stress
  numbers. It uses about 10% of one of the four cores.
- **This is not Phaser.** The shapes follow Phaser 3/4 so that each engine is loaded the way a
  Phaser game loads it. The absolute numbers are not a Phaser game's.
