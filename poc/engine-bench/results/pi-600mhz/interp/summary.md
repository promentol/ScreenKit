# Hermes vs SpiderMonkey: Phaser-shaped work

Raspberry Pi 3 Model B Plus Rev 1.3, 4 cores, 908.4 MB, kernel 6.12.25-v8. 60 frames per scene and load (first 30 left out of the steady numbers), view 640x480, frames back to back.

| run | engine | mode | library |
|---|---:|---:|---:|
| sm-interp | JavaScript-C128.14.0 | off | lib/libmozjs-128.so.0 |
| sm-interp-lazy | JavaScript-C128.14.0 | off | lib/libmozjs-128.so.0 |
| sm-interp-eager | JavaScript-C128.14.0 | off | lib/libmozjs-128.so.0 |

> **Throttled:** the firmware reported under-voltage or throttling, or a clock below the maximum, in sm-interp, sm-interp-lazy, sm-interp-eager (0x50005 at 600 MHz). Those numbers are not the machine's.

## Startup and memory

RSS in MB. *Anon* is memory the process made (heaps, JIT code); *file* is mapped files (libraries, bytecode).

|  | sm-interp |
|---|---:|
| engine up, ms | 212.8 |
| load bulk, ms | 901.5 |
| load workload, ms | 71.7 |
| RSS: process | 5.5 |
| RSS: engine up | 15.9 |
| RSS: + bulk code | 24.2 |
| RSS: + workload | 24.4 |
| RSS peak (VmHWM) | 39.2 |
| RSS at end | 39.2 |
|   PSS at end | 36.9 |
|   private dirty at end | 25.9 |
| RSS after full GC | 29.3 |
|   anon | 15.8 |
|   file | 13.5 |
| JS heap after full GC, MB | 2.4 |
| threads | 5 |
| CPU total, s (all threads) | 100.3 |
| wall total, s | 101.0 |

## Frame time

Steady frames, median / 95th percentile in ms; 16.7 ms is a 60 fps frame. The fastest median in each row is **bold**.

| scene | load | sm-interp |
|---|---:|---:|
| sprites | 25 sprites | **6.07 / 6.69** |
| sprites | 100 sprites | **23.88 / 24.53** |
| sprites | 400 sprites | **92.15 / 92.42** |
| particles | 63 particles | **7.75 / 9.08** |
| particles | 250 particles | **30.21 / 44.96** |
| particles | 1000 particles | **135.90 / 192.70** |
| platformer | 3 enemies | **21.24 / 21.52** |
| platformer | 13 enemies | **25.02 / 25.51** |
| platformer | 50 enemies | **38.97 / 40.42** |
| shooter | 25 bullets | **6.98 / 8.33** |
| shooter | 100 bullets | **23.84 / 28.64** |
| shooter | 400 bullets | **85.81 / 111.30** |
| puzzle | 16 gems | **5.03 / 6.06** |
| puzzle | 64 gems | **15.71 / 20.59** |
| puzzle | 256 gems | **64.42 / 89.27** |
| swarm | 13 bodies | **5.33 / 5.90** |
| swarm | 50 bodies | **19.63 / 20.50** |
| swarm | 125 bodies | **51.19 / 52.64** |
| vector | 1 shapes | **55.46 / 55.73** |
| vector | 3 shapes | **175.50 / 205.90** |
| vector | 12 shapes | **669.50 / 720.40** |
| ui | 1 panels | **7.65 / 7.88** |
| ui | 5 panels | **23.72 / 24.33** |
| ui | 20 panels | **83.73 / 86.52** |

## What fits in a frame

The largest load tried whose 95th-percentile frame stayed within 16.7 ms; 0 when even the smallest did not.

| scene | sm-interp |
|---|---:|
| sprites (sprites) | 25 |
| particles (particles) | 63 |
| platformer (enemies) | 0 |
| shooter (bullets) | 25 |
| puzzle (gems) | 16 |
| swarm (bodies) | 13 |
| vector (shapes) | 0 |
| ui (panels) | 1 |

## Speed against the Hermes interpreter

Median frame time of `hermes-interp` divided by each run's: above 1 is faster.

| scene | load | sm-interp |
|---|---:|---:|
| sprites | 25 | 1.00× |
| sprites | 100 | 1.00× |
| sprites | 400 | 1.00× |
| particles | 63 | 1.00× |
| particles | 250 | 1.00× |
| particles | 1000 | 1.00× |
| platformer | 3 | 1.00× |
| platformer | 13 | 1.00× |
| platformer | 50 | 1.00× |
| shooter | 25 | 1.00× |
| shooter | 100 | 1.00× |
| shooter | 400 | 1.00× |
| puzzle | 16 | 1.00× |
| puzzle | 64 | 1.00× |
| puzzle | 256 | 1.00× |
| swarm | 13 | 1.00× |
| swarm | 50 | 1.00× |
| swarm | 125 | 1.00× |
| vector | 1 | 1.00× |
| vector | 3 | 1.00× |
| vector | 12 | 1.00× |
| ui | 1 | 1.00× |
| ui | 5 | 1.00× |
| ui | 20 | 1.00× |

## Memory per scene

Peak RSS in MB while the scene ran (sampled every frame), and the JS heap at its end.

| scene | load | sm-interp RSS | sm-interp heap |
|---|---:|---:|---:|
| sprites | 25 | 24.7 | 2.3 |
| sprites | 100 | 24.7 | 2.3 |
| sprites | 400 | 24.8 | 2.4 |
| particles | 63 | 24.8 | 2.4 |
| particles | 250 | 24.8 | 2.4 |
| particles | 1000 | 25.2 | 2.6 |
| platformer | 3 | 28.2 | 4.9 |
| platformer | 13 | 32.1 | 7.4 |
| platformer | 50 | 35.8 | 9.9 |
| shooter | 25 | 35.9 | 9.9 |
| shooter | 100 | 36.1 | 9.9 |
| shooter | 400 | 36.2 | 9.9 |
| puzzle | 16 | 36.2 | 9.9 |
| puzzle | 64 | 36.3 | 9.9 |
| puzzle | 256 | 36.8 | 10.0 |
| swarm | 13 | 36.8 | 10.0 |
| swarm | 50 | 37.1 | 10.1 |
| swarm | 125 | 37.4 | 10.3 |
| vector | 1 | 37.4 | 10.3 |
| vector | 3 | 37.4 | 10.3 |
| vector | 12 | 37.3 | 10.5 |
| ui | 1 | 37.5 | 10.5 |
| ui | 5 | 37.9 | 10.5 |
| ui | 20 | 39.1 | 10.6 |

## Garbage collection

Over all steady and warm-up frames: collections, total pause on the JS thread, the longest pause, and (Hermes) old-generation work on its background thread, in ms.

|  | sm-interp |
|---|---:|
| collections | 43 |
| pause total, ms | 34.3 |
| longest pause, ms | 5.1 |
| background, ms | 0.0 |
| CPU per frame / frame time | 1.00 |

## SpiderMonkey: source against stencils

The same scripts from source, and compiled ahead by `screenkit-smc`: a *lazy* stencil compiles a function from the source it carries on its first call, an *eager* one has every function compiled. A stencil is mapped and its bytecode run in place, as in the runtime. Times in ms, RSS in MB.

|  | sm-interp | sm-interp-lazy | sm-interp-eager |
|---|---:|---:|---:|
| form | source | lazy stencil | eager stencil |
| load bulk | 901.5 | 141.1 | 178.4 |
| load workload | 71.7 | 38.3 | 34.6 |
| RSS after loading (anon / file) | 24.4 (11.4 / 13.0) | 23.4 (7.9 / 15.5) | 27.0 (9.5 / 17.5) |
| first scene: build + first frame (sprites 25) | 32.3 | 33.3 | 14.2 |
| every scene: build | 2378.7 | 2305.3 | 2403.2 |
| every scene: first frame | 1513.6 | 1487.2 | 1450.8 |
| every scene: warm-up frames (first 30) | 45730 | 44733 | 44875 |
| steady median, speed against source (geometric mean) | 1 | 1.02× | 1.00× |
| RSS peak (VmHWM) | 39.2 | 38.7 | 42.3 |
| RSS after full GC (anon / file) | 29.3 (15.8 / 13.5) | 29.9 (13.8 / 16.1) | 33.2 (15.2 / 18.1) |
| time to the first frame of the first scene | 1218 | 402 | 392 |

## Same work?

Every run's end state matched in all 24 segments.
