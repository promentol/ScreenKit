# Hermes vs SpiderMonkey: Phaser-shaped work

, 12 cores, 7837.0 MB, kernel 6.10.14-linuxkit. 60 frames per scene and load (first 20 left out of the steady numbers), view 640x480, frames back to back.

| run | engine | mode | library |
|---|---:|---:|---:|
| hermes-jit | Hermes, bytecode 99 | on | lib/libhermesvm.so |
| sm-interp | JavaScript-C128.14.0 | off | lib/libmozjs-128.so.0 |
| sm-interp-lazy | JavaScript-C128.14.0 | off | lib/libmozjs-128.so.0 |
| sm-interp-eager | JavaScript-C128.14.0 | off | lib/libmozjs-128.so.0 |
| sm-jit | JavaScript-C128.14.0 | on | lib/libmozjs-128.so.0 |
| sm-jit-lazy | JavaScript-C128.14.0 | on | lib/libmozjs-128.so.0 |
| sm-jit-eager | JavaScript-C128.14.0 | on | lib/libmozjs-128.so.0 |

## Startup and memory

RSS in MB. *Anon* is memory the process made (heaps, JIT code); *file* is mapped files (libraries, bytecode).

|  | hermes-jit | sm-interp | sm-jit |
|---|---:|---:|---:|
| engine up, ms | 14.6 | 33.9 | 31.6 |
| load bulk, ms | 10.3 | 36.4 | 36.9 |
| load workload, ms | 1.5 | 3.5 | 4.8 |
| RSS: process | 4.9 | 5.1 | 5.2 |
| RSS: engine up | 12.8 | 15.2 | 15.3 |
| RSS: + bulk code | 14.9 | 23.4 | 24.1 |
| RSS: + workload | 15.3 | 23.6 | 24.5 |
| RSS peak (VmHWM) | 39.6 | 72.8 | 62.0 |
| RSS at end | 39.6 | 56.3 | 59.3 |
|   PSS at end | 39.0 | 56.1 | 59.5 |
|   private dirty at end | 31.6 | 43.5 | 45.6 |
| RSS after full GC | 35.7 | 39.2 | 47.1 |
|   anon | 27.6 | 26.2 | 32.7 |
|   file | 8.1 | 13.0 | 14.3 |
| JS heap after full GC, MB | 1.4 | 2.4 | 2.4 |
| threads | 2 | 9 | 9 |
| CPU total, s (all threads) | 1.8 | 12.3 | 0.4 |
| wall total, s | 1.8 | 12.4 | 0.4 |

## Frame time

Steady frames, median / 95th percentile in ms; 16.7 ms is a 60 fps frame. The fastest median in each row is **bold**.

| scene | load | hermes-jit | sm-interp | sm-jit |
|---|---:|---:|---:|---:|
| sprites | 100 sprites | 0.14 / 0.26 | 0.76 / 1.25 | **0.09 / 0.36** |
| sprites | 400 sprites | 0.63 / 2.17 | 2.96 / 3.09 | **0.07 / 0.16** |
| sprites | 1600 sprites | 2.30 / 2.50 | 11.90 / 14.63 | **0.29 / 0.48** |
| particles | 250 particles | 0.17 / 0.26 | 0.96 / 1.45 | **0.04 / 0.17** |
| particles | 1000 particles | 0.72 / 1.06 | 4.08 / 5.87 | **0.10 / 0.21** |
| particles | 4000 particles | 3.12 / 5.13 | 16.64 / 24.02 | **0.33 / 0.51** |
| platformer | 10 enemies | 0.17 / 0.43 | 0.81 / 0.95 | **0.03 / 0.05** |
| platformer | 50 enemies | 0.22 / 0.28 | 1.31 / 1.67 | **0.03 / 0.05** |
| platformer | 200 enemies | 0.48 / 0.56 | 2.92 / 3.19 | **0.10 / 0.13** |
| shooter | 100 bullets | 0.18 / 0.37 | 0.93 / 2.00 | **0.03 / 0.04** |
| shooter | 400 bullets | 0.57 / 0.94 | 2.69 / 3.61 | **0.07 / 0.09** |
| shooter | 1600 bullets | 1.98 / 3.90 | 10.42 / 15.55 | **0.25 / 0.34** |
| puzzle | 64 gems | 0.09 / 0.14 | 0.61 / 1.50 | **0.02 / 0.08** |
| puzzle | 256 gems | 0.39 / 0.71 | 2.08 / 3.09 | **0.06 / 0.09** |
| puzzle | 1024 gems | 1.71 / 3.00 | 8.22 / 12.76 | **0.21 / 0.42** |
| swarm | 50 bodies | 0.11 / 0.24 | 0.59 / 0.69 | **0.03 / 0.06** |
| swarm | 200 bodies | 0.77 / 1.30 | 2.69 / 7.84 | **0.09 / 0.14** |
| swarm | 500 bodies | 1.78 / 2.19 | 8.49 / 9.23 | **0.31 / 0.37** |
| vector | 3 shapes | 0.33 / 0.44 | 5.28 / 11.72 | **0.11 / 0.13** |
| vector | 12 shapes | 1.29 / 1.45 | 21.05 / 24.92 | **0.42 / 0.48** |
| vector | 48 shapes | 5.35 / 6.26 | 84.14 / 89.59 | **1.65 / 1.72** |
| ui | 5 panels | 0.18 / 0.22 | 0.81 / 1.06 | **0.03 / 0.07** |
| ui | 20 panels | 0.66 / 1.04 | 2.90 / 3.31 | **0.06 / 0.09** |
| ui | 80 panels | 3.51 / 10.94 | 10.93 / 12.67 | **0.25 / 0.35** |

## What fits in a frame

The largest load tried whose 95th-percentile frame stayed within 16.7 ms; 0 when even the smallest did not.

| scene | hermes-jit | sm-interp | sm-jit |
|---|---:|---:|---:|
| sprites (sprites) | 1600+ | 1600+ | 1600+ |
| particles (particles) | 4000+ | 1000 | 4000+ |
| platformer (enemies) | 200+ | 200+ | 200+ |
| shooter (bullets) | 1600+ | 1600+ | 1600+ |
| puzzle (gems) | 1024+ | 1024+ | 1024+ |
| swarm (bodies) | 500+ | 500+ | 500+ |
| vector (shapes) | 48+ | 3 | 48+ |
| ui (panels) | 80+ | 80+ | 80+ |

## Speed against the Hermes interpreter

Median frame time of `hermes-interp` divided by each run's: above 1 is faster.

| scene | load | hermes-jit | sm-interp | sm-jit |
|---|---:|---:|---:|---:|
| sprites | 100 | 1.00× | 0.18× | 1.65× |
| sprites | 400 | 1.00× | 0.21× | 8.74× |
| sprites | 1600 | 1.00× | 0.19× | 7.91× |
| particles | 250 | 1.00× | 0.18× | 4.32× |
| particles | 1000 | 1.00× | 0.18× | 7.14× |
| particles | 4000 | 1.00× | 0.19× | 9.43× |
| platformer | 10 | 1.00× | 0.21× | 6.25× |
| platformer | 50 | 1.00× | 0.17× | 6.61× |
| platformer | 200 | 1.00× | 0.17× | 4.97× |
| shooter | 100 | 1.00× | 0.19× | 6.86× |
| shooter | 400 | 1.00× | 0.21× | 8.39× |
| shooter | 1600 | 1.00× | 0.19× | 7.82× |
| puzzle | 64 | 1.00× | 0.15× | 4.70× |
| puzzle | 256 | 1.00× | 0.19× | 6.77× |
| puzzle | 1024 | 1.00× | 0.21× | 8.25× |
| swarm | 50 | 1.00× | 0.19× | 4.11× |
| swarm | 200 | 1.00× | 0.28× | 8.52× |
| swarm | 500 | 1.00× | 0.21× | 5.69× |
| vector | 3 | 1.00× | 0.06× | 3.02× |
| vector | 12 | 1.00× | 0.06× | 3.06× |
| vector | 48 | 1.00× | 0.06× | 3.25× |
| ui | 5 | 1.00× | 0.22× | 6.65× |
| ui | 20 | 1.00× | 0.23× | 10.94× |
| ui | 80 | 1.00× | 0.32× | 14.05× |

## Memory per scene

Peak RSS in MB while the scene ran (sampled every frame), and the JS heap at its end.

| scene | load | hermes-jit RSS | sm-interp RSS | sm-jit RSS | hermes-jit heap | sm-interp heap | sm-jit heap |
|---|---:|---:|---:|---:|---:|---:|---:|
| sprites | 100 | 15.8 | 24.1 | 27.2 | 1.9 | 2.2 | 2.2 |
| sprites | 400 | 15.9 | 24.6 | 27.9 | 2.3 | 2.4 | 2.4 |
| sprites | 1600 | 17.9 | 25.1 | 29.8 | 2.4 | 2.7 | 2.8 |
| particles | 250 | 18.0 | 25.1 | 30.3 | 2.8 | 2.7 | 2.8 |
| particles | 1000 | 18.0 | 25.2 | 30.4 | 3.2 | 2.7 | 2.8 |
| particles | 4000 | 18.3 | 25.7 | 31.4 | 3.4 | 3.0 | 3.1 |
| platformer | 10 | 22.4 | 29.3 | 38.7 | 9.1 | 4.4 | 6.0 |
| platformer | 50 | 30.5 | 33.8 | 43.4 | 13.3 | 6.6 | 8.3 |
| platformer | 200 | 34.5 | 40.9 | 49.5 | 19.0 | 9.8 | 11.0 |
| shooter | 100 | 34.5 | 40.9 | 49.9 | 19.3 | 9.8 | 11.0 |
| shooter | 400 | 34.5 | 41.2 | 49.9 | 20.2 | 9.8 | 11.0 |
| shooter | 1600 | 34.5 | 42.2 | 51.0 | 20.4 | 9.8 | 11.0 |
| puzzle | 64 | 34.5 | 42.2 | 51.5 | 20.8 | 9.8 | 11.0 |
| puzzle | 256 | 34.5 | 42.2 | 51.5 | 21.6 | 9.8 | 11.0 |
| puzzle | 1024 | 34.5 | 44.2 | 51.8 | 21.1 | 11.1 | 11.0 |
| swarm | 50 | 34.5 | 44.2 | 52.3 | 22.0 | 11.1 | 11.2 |
| swarm | 200 | 34.8 | 44.2 | 52.4 | 21.8 | 11.1 | 11.2 |
| swarm | 500 | 39.2 | 47.2 | 54.8 | 4.0 | 11.1 | 11.3 |
| vector | 3 | 39.3 | 49.8 | 56.5 | 4.4 | 11.1 | 11.3 |
| vector | 12 | 39.3 | 57.1 | 61.8 | 6.0 | 11.6 | 11.3 |
| vector | 48 | 39.4 | 72.6 | 62.0 | 4.4 | 11.6 | 11.4 |
| ui | 5 | 39.4 | 52.3 | 53.9 | 5.7 | 11.6 | 11.4 |
| ui | 20 | 39.4 | 52.4 | 54.8 | 6.6 | 11.7 | 11.6 |
| ui | 80 | 39.4 | 56.3 | 59.3 | 6.3 | 12.0 | 12.2 |

## Garbage collection

Over all steady and warm-up frames: collections, total pause on the JS thread, the longest pause, and (Hermes) old-generation work on its background thread, in ms.

|  | hermes-jit | sm-interp | sm-jit |
|---|---:|---:|---:|
| collections | 28 | 7 | 26 |
| pause total, ms | 12.7 | 2.5 | 3.5 |
| longest pause, ms | 2.0 | 1.0 | 1.4 |
| background, ms | 0.0 | 0.0 | 0.0 |
| CPU per frame / frame time | 1.00 | 1.00 | 1.12 |

## SpiderMonkey: source against stencils

The same scripts from source, and compiled ahead by `screenkit-smc`: a *lazy* stencil compiles a function from the source it carries on its first call, an *eager* one has every function compiled. A stencil is mapped and its bytecode run in place, as in the runtime. Times in ms, RSS in MB.

|  | sm-interp | sm-interp-lazy | sm-interp-eager | sm-jit | sm-jit-lazy | sm-jit-eager |
|---|---:|---:|---:|---:|---:|---:|
| form | source | lazy stencil | eager stencil | source | lazy stencil | eager stencil |
| load bulk | 36.4 | 8.6 | 17.9 | 36.9 | 11.2 | 17.4 |
| load workload | 3.5 | 2.0 | 2.8 | 4.8 | 2.5 | 3.2 |
| RSS after loading (anon / file) | 23.6 (11.2 / 12.4) | 23.0 (7.9 / 15.0) | 26.5 (9.4 / 17.1) | 24.5 (11.2 / 13.3) | 24.4 (8.4 / 15.9) | 27.8 (9.9 / 17.9) |
| first scene: build + first frame (sprites 100) | 4.6 | 3.5 | 1.9 | 3.8 | 3.3 | 3.1 |
| every scene: build | 117.5 | 106.0 | 103.7 | 19.6 | 29.0 | 26.0 |
| every scene: first frame | 204.6 | 180.8 | 183.9 | 11.8 | 16.9 | 12.4 |
| every scene: warm-up frames (first 20) | 3710 | 3615 | 3696 | 100 | 141 | 129 |
| steady median, speed against source (geometric mean) | 1 | 1.05× | 1.03× | 1 | 0.87× | 0.95× |
| RSS peak (VmHWM) | 72.8 | 73.4 | 59.4 | 62.0 | 61.8 | 65.2 |
| RSS after full GC (anon / file) | 39.2 (26.2 / 13.0) | 38.9 (23.3 / 15.6) | 40.4 (22.6 / 17.7) | 47.1 (32.7 / 14.3) | 43.7 (26.7 / 17.0) | 50.4 (31.5 / 18.9) |
| time to the first frame of the first scene | 78 | 49 | 56 | 77 | 56 | 56 |

## Same work?

Every run's end state matched in all 24 segments.
