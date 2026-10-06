# Hermes vs SpiderMonkey: Phaser-shaped work

Raspberry Pi 3 Model B Plus Rev 1.3, 4 cores, 908.4 MB, kernel 6.12.25-v8. 180 frames per scene and load (first 30 left out of the steady numbers), view 640x480, frames back to back.

| run | engine | mode | library |
|---|---:|---:|---:|
| hermes-interp | Hermes, bytecode 99 | off | lib-interp/libhermesvm.so |
| hermes-jit | Hermes, bytecode 99 | on | lib/libhermesvm.so |
| sm-interp | JavaScript-C128.14.0 | off | lib/libmozjs-128.so.0 |
| sm-baseline | JavaScript-C128.14.0 | baseline | lib/libmozjs-128.so.0 |
| sm-jit | JavaScript-C128.14.0 | on | lib/libmozjs-128.so.0 |

## Startup and memory

RSS in MB. *Anon* is memory the process made (heaps, JIT code); *file* is mapped files (libraries, bytecode).

|  | hermes-interp | hermes-jit | sm-interp | sm-baseline | sm-jit |
|---|---:|---:|---:|---:|---:|
| engine up, ms | 21.4 | 19.8 | 160.4 | 167.8 | 158.5 |
| load bulk, ms | 82.2 | 73.9 | 781.7 | 784.6 | 786.8 |
| load workload, ms | 17.5 | 17.6 | 66.7 | 59.4 | 59.4 |
| RSS: process | 5.0 | 5.0 | 5.4 | 5.4 | 5.4 |
| RSS: engine up | 7.2 | 7.4 | 16.1 | 16.1 | 16.1 |
| RSS: + bulk code | 10.5 | 11.0 | 24.3 | 24.8 | 24.9 |
| RSS: + workload | 10.9 | 11.5 | 24.6 | 25.3 | 25.3 |
| RSS peak (VmHWM) | 35.8 | 37.0 | 55.8 | 59.3 | 60.0 |
| RSS at end | 35.8 | 37.0 | 55.8 | 59.3 | 60.0 |
|   PSS at end | 33.5 | 34.3 | 53.2 | 56.6 | 57.7 |
|   private dirty at end | 28.5 | 28.9 | 42.5 | 45.1 | 45.4 |
| RSS after full GC | 34.5 | 35.8 | 37.3 | 38.8 | 41.9 |
|   anon | 26.9 | 27.6 | 23.8 | 24.5 | 26.9 |
|   file | 7.6 | 8.1 | 13.5 | 14.3 | 15.0 |
| JS heap after full GC, MB | 1.4 | 1.4 | 2.4 | 2.4 | 2.4 |
| threads | 2 | 2 | 5 | 5 | 5 |
| CPU total, s (all threads) | 254.7 | 162.6 | 1186.0 | 168.8 | 35.0 |
| wall total, s | 255.1 | 162.6 | 1186.7 | 168.9 | 34.3 |

## Frame time

Steady frames, median / 95th percentile in ms; 16.7 ms is a 60 fps frame. The fastest median in each row is **bold**.

| scene | load | hermes-interp | hermes-jit | sm-interp | sm-baseline | sm-jit |
|---|---:|---:|---:|---:|---:|---:|
| sprites | 100 sprites | 6.27 / 6.67 | 4.60 / 4.67 | 22.91 / 23.00 | 3.29 / 3.38 | **0.70 / 1.04** |
| sprites | 400 sprites | 25.44 / 25.93 | 18.49 / 18.72 | 91.33 / 91.68 | 13.17 / 13.28 | **2.80 / 2.90** |
| sprites | 1600 sprites | 100.30 / 101.60 | 75.24 / 75.58 | 366.40 / 368.30 | 53.40 / 53.88 | **11.94 / 12.12** |
| particles | 250 particles | 12.50 / 14.57 | 8.53 / 9.96 | 44.31 / 51.58 | 6.42 / 7.50 | **1.51 / 2.65** |
| particles | 1000 particles | 50.76 / 52.47 | 35.31 / 36.95 | 182.00 / 185.50 | 26.34 / 27.01 | **5.45 / 5.93** |
| particles | 4000 particles | 208.60 / 213.60 | 148.90 / 154.20 | 732.20 / 737.00 | 108.20 / 110.10 | **23.98 / 25.28** |
| platformer | 10 enemies | 6.54 / 7.07 | 4.52 / 4.82 | 22.65 / 23.65 | 3.30 / 3.58 | **1.23 / 2.21** |
| platformer | 50 enemies | 10.67 / 11.70 | 7.31 / 7.67 | 38.21 / 39.95 | 5.33 / 5.62 | **1.45 / 2.18** |
| platformer | 200 enemies | 27.13 / 28.67 | 18.23 / 18.60 | 97.97 / 99.87 | 13.34 / 13.69 | **4.04 / 4.41** |
| shooter | 100 bullets | 6.67 / 8.38 | 4.67 / 5.87 | 22.82 / 27.70 | 3.52 / 4.58 | **1.01 / 1.45** |
| shooter | 400 bullets | 31.69 / 32.38 | 22.84 / 23.75 | 107.50 / 108.60 | 17.28 / 17.78 | **4.47 / 4.81** |
| shooter | 1600 bullets | 137.50 / 143.60 | 96.72 / 97.50 | 440.70 / 444.40 | 71.43 / 72.35 | **20.21 / 21.00** |
| puzzle | 64 gems | 4.45 / 6.08 | 3.01 / 4.19 | 15.14 / 19.78 | 2.30 / 3.23 | **0.62 / 1.70** |
| puzzle | 256 gems | 17.59 / 25.16 | 12.21 / 17.50 | 60.69 / 82.52 | 9.22 / 13.35 | **2.28 / 3.74** |
| puzzle | 1024 gems | 69.65 / 106.60 | 49.98 / 75.52 | 243.80 / 344.30 | 37.72 / 56.89 | **10.33 / 15.87** |
| swarm | 50 bodies | 5.10 / 5.65 | 3.65 / 4.20 | 18.52 / 18.87 | 2.76 / 2.90 | **0.78 / 1.50** |
| swarm | 200 bodies | 23.63 / 24.72 | 16.93 / 18.17 | 83.54 / 85.70 | 12.06 / 12.63 | **3.34 / 4.49** |
| swarm | 500 bodies | 75.94 / 77.48 | 56.22 / 57.30 | 264.50 / 269.00 | 36.80 / 42.21 | **10.84 / 12.06** |
| vector | 3 shapes | 24.27 / 24.68 | 11.64 / 11.93 | 165.90 / 166.90 | 22.12 / 22.60 | **2.88 / 3.42** |
| vector | 12 shapes | 96.49 / 98.64 | 46.35 / 48.70 | 662.90 / 666.80 | 88.70 / 95.38 | **11.42 / 11.90** |
| vector | 48 shapes | 388.30 / 394.00 | 184.90 / 187.60 | 2648.00 / 2658.00 | 353.50 / 355.50 | **45.38 / 45.98** |
| ui | 5 panels | 7.56 / 7.77 | 5.74 / 5.96 | 23.34 / 24.20 | 3.45 / 4.09 | **0.77 / 1.56** |
| ui | 20 panels | 26.95 / 27.72 | 20.48 / 21.69 | 82.84 / 85.25 | 12.38 / 12.88 | **2.73 / 3.11** |
| ui | 80 panels | 105.20 / 109.90 | 79.82 / 84.65 | 322.50 / 332.60 | 48.70 / 52.57 | **11.21 / 14.40** |

## What fits in a frame

The largest load tried whose 95th-percentile frame stayed within 16.7 ms; 0 when even the smallest did not.

| scene | hermes-interp | hermes-jit | sm-interp | sm-baseline | sm-jit |
|---|---:|---:|---:|---:|---:|
| sprites (sprites) | 100 | 100 | 0 | 400 | 1600+ |
| particles (particles) | 250 | 250 | 0 | 250 | 1000 |
| platformer (enemies) | 50 | 50 | 0 | 200+ | 200+ |
| shooter (bullets) | 100 | 100 | 0 | 100 | 400 |
| puzzle (gems) | 64 | 64 | 0 | 256 | 1024+ |
| swarm (bodies) | 50 | 50 | 0 | 200 | 500+ |
| vector (shapes) | 0 | 3 | 0 | 0 | 12 |
| ui (panels) | 5 | 5 | 0 | 20 | 80+ |

## Speed against the Hermes interpreter

Median frame time of `hermes-interp` divided by each run's: above 1 is faster.

| scene | load | hermes-interp | hermes-jit | sm-interp | sm-baseline | sm-jit |
|---|---:|---:|---:|---:|---:|---:|
| sprites | 100 | 1.00× | 1.36× | 0.27× | 1.91× | 8.94× |
| sprites | 400 | 1.00× | 1.38× | 0.28× | 1.93× | 9.09× |
| sprites | 1600 | 1.00× | 1.33× | 0.27× | 1.88× | 8.40× |
| particles | 250 | 1.00× | 1.47× | 0.28× | 1.95× | 8.27× |
| particles | 1000 | 1.00× | 1.44× | 0.28× | 1.93× | 9.32× |
| particles | 4000 | 1.00× | 1.40× | 0.28× | 1.93× | 8.70× |
| platformer | 10 | 1.00× | 1.45× | 0.29× | 1.98× | 5.33× |
| platformer | 50 | 1.00× | 1.46× | 0.28× | 2.00× | 7.36× |
| platformer | 200 | 1.00× | 1.49× | 0.28× | 2.03× | 6.72× |
| shooter | 100 | 1.00× | 1.43× | 0.29× | 1.90× | 6.61× |
| shooter | 400 | 1.00× | 1.39× | 0.29× | 1.83× | 7.09× |
| shooter | 1600 | 1.00× | 1.42× | 0.31× | 1.92× | 6.80× |
| puzzle | 64 | 1.00× | 1.48× | 0.29× | 1.94× | 7.19× |
| puzzle | 256 | 1.00× | 1.44× | 0.29× | 1.91× | 7.72× |
| puzzle | 1024 | 1.00× | 1.39× | 0.29× | 1.85× | 6.74× |
| swarm | 50 | 1.00× | 1.40× | 0.28× | 1.85× | 6.51× |
| swarm | 200 | 1.00× | 1.40× | 0.28× | 1.96× | 7.07× |
| swarm | 500 | 1.00× | 1.35× | 0.29× | 2.06× | 7.01× |
| vector | 3 | 1.00× | 2.09× | 0.15× | 1.10× | 8.43× |
| vector | 12 | 1.00× | 2.08× | 0.15× | 1.09× | 8.45× |
| vector | 48 | 1.00× | 2.10× | 0.15× | 1.10× | 8.56× |
| ui | 5 | 1.00× | 1.32× | 0.32× | 2.19× | 9.85× |
| ui | 20 | 1.00× | 1.32× | 0.33× | 2.18× | 9.86× |
| ui | 80 | 1.00× | 1.32× | 0.33× | 2.16× | 9.38× |

## Memory per scene

Peak RSS in MB while the scene ran (sampled every frame), and the JS heap at its end.

| scene | load | hermes-interp RSS | hermes-jit RSS | sm-interp RSS | sm-baseline RSS | sm-jit RSS | hermes-interp heap | hermes-jit heap | sm-interp heap | sm-baseline heap | sm-jit heap |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sprites | 100 | 11.2 | 11.9 | 24.8 | 25.6 | 28.4 | 1.9 | 1.9 | 2.3 | 2.4 | 2.4 |
| sprites | 400 | 11.5 | 12.3 | 24.9 | 25.7 | 28.7 | 2.3 | 2.3 | 2.4 | 2.5 | 2.5 |
| sprites | 1600 | 14.9 | 15.8 | 25.4 | 26.3 | 29.9 | 2.4 | 2.4 | 2.8 | 2.9 | 2.8 |
| particles | 250 | 14.8 | 15.8 | 25.5 | 26.6 | 30.7 | 2.9 | 2.9 | 2.9 | 3.0 | 3.0 |
| particles | 1000 | 14.8 | 15.8 | 25.7 | 26.7 | 31.1 | 3.8 | 3.8 | 3.0 | 3.2 | 3.2 |
| particles | 4000 | 16.2 | 17.1 | 26.4 | 27.7 | 31.9 | 4.3 | 4.3 | 3.7 | 3.8 | 3.8 |
| platformer | 10 | 20.8 | 21.9 | 30.1 | 31.9 | 36.8 | 9.1 | 9.1 | 6.1 | 6.3 | 6.1 |
| platformer | 50 | 25.7 | 26.8 | 34.0 | 35.6 | 40.8 | 13.3 | 13.1 | 8.6 | 8.7 | 8.7 |
| platformer | 200 | 30.8 | 31.9 | 38.1 | 40.0 | 45.3 | 19.0 | 19.4 | 11.2 | 11.3 | 11.3 |
| shooter | 100 | 30.9 | 32.1 | 38.4 | 39.8 | 45.4 | 19.6 | 17.8 | 11.2 | 11.3 | 11.3 |
| shooter | 400 | 31.2 | 32.1 | 39.8 | 41.3 | 46.1 | 17.9 | 20.2 | 11.2 | 11.4 | 11.4 |
| shooter | 1600 | 32.3 | 33.6 | 40.7 | 42.7 | 48.3 | 21.6 | 20.3 | 11.8 | 11.9 | 12.0 |
| puzzle | 64 | 32.7 | 33.6 | 40.8 | 42.9 | 48.7 | 19.8 | 21.0 | 12.0 | 12.0 | 12.2 |
| puzzle | 256 | 32.7 | 33.6 | 41.3 | 43.1 | 49.0 | 21.2 | 22.4 | 12.1 | 12.2 | 12.3 |
| puzzle | 1024 | 34.1 | 35.6 | 42.3 | 44.7 | 50.3 | 22.6 | 22.3 | 13.0 | 13.6 | 13.5 |
| swarm | 50 | 34.4 | 36.0 | 42.8 | 44.8 | 50.4 | 4.1 | 3.4 | 13.1 | 13.7 | 13.6 |
| swarm | 200 | 34.7 | 36.2 | 44.7 | 45.5 | 50.9 | 5.7 | 4.1 | 13.3 | 14.0 | 13.9 |
| swarm | 500 | 34.9 | 36.6 | 46.3 | 48.5 | 54.3 | 4.8 | 5.9 | 13.9 | 14.5 | 14.5 |
| vector | 3 | 35.7 | 36.7 | 46.4 | 48.6 | 54.9 | 7.5 | 5.3 | 14.0 | 14.6 | 14.6 |
| vector | 12 | 35.7 | 36.7 | 46.3 | 48.5 | 54.8 | 4.3 | 5.9 | 14.4 | 15.0 | 14.8 |
| vector | 48 | 35.8 | 36.9 | 49.2 | 53.0 | 55.7 | 7.4 | 5.2 | 15.9 | 16.5 | 15.6 |
| ui | 5 | 35.8 | 37.0 | 49.4 | 53.3 | 56.2 | 7.5 | 5.2 | 16.0 | 16.6 | 15.7 |
| ui | 20 | 35.8 | 37.0 | 51.2 | 55.2 | 57.9 | 5.8 | 7.5 | 16.2 | 16.8 | 15.9 |
| ui | 80 | 35.8 | 37.0 | 55.7 | 59.2 | 60.0 | 10.9 | 8.8 | 19.1 | 20.9 | 18.0 |

## Garbage collection

Over all steady and warm-up frames: collections, total pause on the JS thread, the longest pause, and (Hermes) old-generation work on its background thread, in ms.

|  | hermes-interp | hermes-jit | sm-interp | sm-baseline | sm-jit |
|---|---:|---:|---:|---:|---:|
| collections | 91 | 91 | 529 | 670 | 381 |
| pause total, ms | 389.9 | 393.1 | 331.2 | 416.7 | 263.1 |
| longest pause, ms | 35.3 | 32.3 | 12.4 | 10.6 | 6.7 |
| background, ms | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| CPU per frame / frame time | 1.00 | 1.00 | 1.00 | 1.00 | 1.02 |

## Same work?

Every run's end state matched in all 24 segments.
