# Hermes vs SpiderMonkey: Phaser-shaped work

Raspberry Pi 3 Model B Plus Rev 1.3, 4 cores, 908.4 MB, kernel 6.12.25-v8. 180 frames per scene and load (first 30 left out of the steady numbers), view 640x480, frames back to back.

| run | engine | mode | library |
|---|---:|---:|---:|
| sm-jit | JavaScript-C128.14.0 | on | lib/libmozjs-128.so.0 |
| sm-jit-lazy | JavaScript-C128.14.0 | on | lib/libmozjs-128.so.0 |
| sm-jit-eager | JavaScript-C128.14.0 | on | lib/libmozjs-128.so.0 |

> **Throttled:** the firmware reported under-voltage or throttling, or a clock below the maximum, in sm-jit, sm-jit-lazy, sm-jit-eager (0x50005 at 600 MHz). Those numbers are not the machine's.

## Startup and memory

RSS in MB. *Anon* is memory the process made (heaps, JIT code); *file* is mapped files (libraries, bytecode).

|  | sm-jit |
|---|---:|
| engine up, ms | 405.7 |
| load bulk, ms | 875.6 |
| load workload, ms | 64.5 |
| RSS: process | 5.4 |
| RSS: engine up | 15.4 |
| RSS: + bulk code | 24.3 |
| RSS: + workload | 24.7 |
| RSS peak (VmHWM) | 60.1 |
| RSS at end | 60.1 |
|   PSS at end | 58.6 |
|   private dirty at end | 45.8 |
| RSS after full GC | 41.5 |
|   anon | 27.0 |
|   file | 14.5 |
| JS heap after full GC, MB | 2.4 |
| threads | 5 |
| CPU total, s (all threads) | 35.4 |
| wall total, s | 34.9 |

## Frame time

Steady frames, median / 95th percentile in ms; 16.7 ms is a 60 fps frame. The fastest median in each row is **bold**.

| scene | load | sm-jit |
|---|---:|---:|
| sprites | 100 sprites | **0.72 / 1.08** |
| sprites | 400 sprites | **2.84 / 2.96** |
| sprites | 1600 sprites | **12.17 / 12.27** |
| particles | 250 particles | **1.41 / 2.43** |
| particles | 1000 particles | **5.43 / 6.25** |
| particles | 4000 particles | **23.79 / 25.05** |
| platformer | 10 enemies | **1.22 / 2.14** |
| platformer | 50 enemies | **1.50 / 2.23** |
| platformer | 200 enemies | **4.20 / 5.24** |
| shooter | 100 bullets | **0.96 / 1.48** |
| shooter | 400 bullets | **4.68 / 5.60** |
| shooter | 1600 bullets | **20.38 / 20.93** |
| puzzle | 64 gems | **0.62 / 1.75** |
| puzzle | 256 gems | **2.30 / 3.84** |
| puzzle | 1024 gems | **10.31 / 15.76** |
| swarm | 50 bodies | **0.79 / 1.49** |
| swarm | 200 bodies | **3.41 / 4.70** |
| swarm | 500 bodies | **11.32 / 12.03** |
| vector | 3 shapes | **2.86 / 3.36** |
| vector | 12 shapes | **11.30 / 11.80** |
| vector | 48 shapes | **45.52 / 46.32** |
| ui | 5 panels | **0.78 / 1.65** |
| ui | 20 panels | **2.77 / 3.28** |
| ui | 80 panels | **11.19 / 14.41** |

## What fits in a frame

The largest load tried whose 95th-percentile frame stayed within 16.7 ms; 0 when even the smallest did not.

| scene | sm-jit |
|---|---:|
| sprites (sprites) | 1600+ |
| particles (particles) | 1000 |
| platformer (enemies) | 200+ |
| shooter (bullets) | 400 |
| puzzle (gems) | 1024+ |
| swarm (bodies) | 500+ |
| vector (shapes) | 12 |
| ui (panels) | 80+ |

## Speed against the Hermes interpreter

Median frame time of `hermes-interp` divided by each run's: above 1 is faster.

| scene | load | sm-jit |
|---|---:|---:|
| sprites | 100 | 1.00× |
| sprites | 400 | 1.00× |
| sprites | 1600 | 1.00× |
| particles | 250 | 1.00× |
| particles | 1000 | 1.00× |
| particles | 4000 | 1.00× |
| platformer | 10 | 1.00× |
| platformer | 50 | 1.00× |
| platformer | 200 | 1.00× |
| shooter | 100 | 1.00× |
| shooter | 400 | 1.00× |
| shooter | 1600 | 1.00× |
| puzzle | 64 | 1.00× |
| puzzle | 256 | 1.00× |
| puzzle | 1024 | 1.00× |
| swarm | 50 | 1.00× |
| swarm | 200 | 1.00× |
| swarm | 500 | 1.00× |
| vector | 3 | 1.00× |
| vector | 12 | 1.00× |
| vector | 48 | 1.00× |
| ui | 5 | 1.00× |
| ui | 20 | 1.00× |
| ui | 80 | 1.00× |

## Memory per scene

Peak RSS in MB while the scene ran (sampled every frame), and the JS heap at its end.

| scene | load | sm-jit RSS | sm-jit heap |
|---|---:|---:|---:|
| sprites | 100 | 28.2 | 2.4 |
| sprites | 400 | 28.4 | 2.5 |
| sprites | 1600 | 29.7 | 2.9 |
| particles | 250 | 30.7 | 3.0 |
| particles | 1000 | 30.9 | 3.2 |
| particles | 4000 | 31.7 | 3.8 |
| platformer | 10 | 37.0 | 6.3 |
| platformer | 50 | 40.9 | 8.7 |
| platformer | 200 | 45.3 | 11.3 |
| shooter | 100 | 45.5 | 11.3 |
| shooter | 400 | 46.8 | 11.4 |
| shooter | 1600 | 48.3 | 12.0 |
| puzzle | 64 | 48.6 | 12.1 |
| puzzle | 256 | 48.8 | 12.3 |
| puzzle | 1024 | 50.3 | 13.5 |
| swarm | 50 | 50.5 | 13.7 |
| swarm | 200 | 51.0 | 13.9 |
| swarm | 500 | 54.1 | 14.5 |
| vector | 3 | 54.5 | 14.6 |
| vector | 12 | 54.4 | 14.9 |
| vector | 48 | 55.7 | 16.0 |
| ui | 5 | 56.1 | 16.1 |
| ui | 20 | 57.7 | 16.3 |
| ui | 80 | 59.9 | 18.4 |

## Garbage collection

Over all steady and warm-up frames: collections, total pause on the JS thread, the longest pause, and (Hermes) old-generation work on its background thread, in ms.

|  | sm-jit |
|---|---:|
| collections | 465 |
| pause total, ms | 282.8 |
| longest pause, ms | 6.4 |
| background, ms | 0.0 |
| CPU per frame / frame time | 1.02 |

## SpiderMonkey: source against stencils

The same scripts from source, and compiled ahead by `screenkit-smc`: a *lazy* stencil compiles a function from the source it carries on its first call, an *eager* one has every function compiled. A stencil is mapped and its bytecode run in place, as in the runtime. Times in ms, RSS in MB.

|  | sm-jit | sm-jit-lazy | sm-jit-eager |
|---|---:|---:|---:|
| form | source | lazy stencil | eager stencil |
| load bulk | 875.6 | 131.8 | 193.0 |
| load workload | 64.5 | 28.7 | 31.4 |
| RSS after loading (anon / file) | 24.7 (11.2 / 13.5) | 24.7 (8.5 / 16.2) | 28.3 (10.0 / 18.2) |
| first scene: build + first frame (sprites 100) | 57.0 | 57.3 | 41.5 |
| every scene: build | 644.5 | 685.2 | 652.9 |
| every scene: first frame | 282.8 | 670.8 | 259.7 |
| every scene: warm-up frames (first 30) | 4693 | 5497 | 5008 |
| steady median, speed against source (geometric mean) | 1 | 0.87× | 0.90× |
| RSS peak (VmHWM) | 60.1 | 60.5 | 61.8 |
| RSS after full GC (anon / file) | 41.5 (27.0 / 14.5) | 42.3 (25.0 / 17.3) | 43.7 (25.5 / 18.2) |
| time to the first frame of the first scene | 1403 | 381 | 429 |

## Same work?

Every run's end state matched in all 24 segments.
