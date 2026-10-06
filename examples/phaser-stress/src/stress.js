// The multiphase stress test, shared by examples/pixi-stress and examples/phaser-stress (this file is
// copied between them, so the two engines run the same procedure and the same game logic).
//
// Each engine builds one scene per phase; this file decides the load, measures, and owns the
// game logic that is not the engine's -- the level, enemy physics, bullet patterns, the puzzle
// board -- so both engines do identical JavaScript work and differ only in their own.
//
// Procedure:
//   baseline   a nearly empty screen: the rate the display presents at. The target is 90% of it,
//              capped at 54 fps (90% of 60), so a 120 Hz panel is not held to 108.
//   each phase starts at a light load and grows it by ~30% per one-second window while the rate
//              holds the target. Two slow windows in a row at one load end the phase (one slow
//              window is often the cost of creating the new objects). The result is the largest
//              load that held the target. A starting load that never holds is halved, the scene
//              built afresh, until one holds or the load is 1 -- so a slow device still gets a
//              number, and a 0 means the scene itself is too much.
//
// Loads start low and grow fast so a Raspberry Pi 3B+ finishes a phase in seconds, and a desktop
// still reaches its limit within MAX_WINDOWS.
//
// Options, from the URL: ?phases=platformer,shooter  ?target=30  ?webgl=2 -- or, where there is no
// URL, from the build: VITE_PHASES, VITE_TARGET, VITE_WEBGL.
//
// Output: `stress(<engine>): ...` per window and per phase, and one `result {json}` line.

export const WORLD_HEIGHT = 720
export const TILE = 32

export const PHASES = [
  { name: 'sprites', unit: 'sprites', start: 250, max: 100000, label: 'bunnymark: bouncing, spinning sprites' },
  { name: 'platformer', unit: 'enemies', start: 10, max: 5000, label: 'platformer: tilemap, parallax, enemies with tile physics' },
  { name: 'shooter', unit: 'bullets', start: 100, max: 30000, label: 'shooter: bullet patterns, additive blending' },
  { name: 'particles', unit: 'particles', start: 250, max: 60000, label: 'particles: explosion bursts' },
  { name: 'puzzle', unit: 'gems', start: 36, max: 10000, label: 'puzzle: animated match-3 board' },
  { name: 'vector', unit: 'shapes', start: 25, max: 10000, label: 'vector: shapes redrawn every frame' },
  { name: 'text', unit: 'texts', start: 2, max: 200, label: 'text: canvas text changing every frame' },
]

const MAX_WINDOWS = 30
const TARGET_SHARE = 0.9

export function readOptions() {
  const query = new URLSearchParams(typeof location !== 'undefined' && location.search ? location.search : '')
  // A runtime with no URL (ScreenKit) takes the same choices at build time: VITE_PHASES=platformer
  // VITE_TARGET=30 npm run build:screenkit.
  const env = import.meta.env || {}
  const list = query.get('phases') || env.VITE_PHASES
  const target = Number(query.get('target') || env.VITE_TARGET)
  return {
    phases: list ? PHASES.filter((phase) => list.split(',').includes(phase.name)) : PHASES,
    target: target > 0 ? target : 0,
    // WebGL1 unless asked: a Raspberry Pi 3B+ has GLES2 only, and the numbers should be comparable.
    webglVersion: (query.get('webgl') || env.VITE_WEBGL) === '2' ? 2 : 1,
  }
}

function nextLoad(phase, load) {
  if (phase.name === 'puzzle') {
    // The board stays square: grow its side ~15%, which is ~30% more gems.
    const side = Math.round(Math.sqrt(load))
    const next = Math.max(side + 1, Math.round(side * 1.15))
    return Math.min(phase.max, next * next)
  }
  return Math.min(phase.max, Math.max(load + 1, Math.round(load * 1.3)))
}

// `scenes[name]` is {enter(), setLoad(n), update(dt), exit()}; dt is in 60 fps frames.
export function createStress({ engine, scenes, options }) {
  const log = (line) => console.log(`stress(${engine}): ${line}`)
  const phases = options.phases
  let state = 'baseline'
  let index = -1
  let windowStart = performance.now()
  let frames = 0
  let windows = 0
  let fps = 0
  let baseline = 0
  let target = 0
  let load = 0
  let held = 0
  let heldFps = 0
  let slow = 0
  let phaseWindows = 0
  const results = {}
  const capped = []

  function enterPhase(next) {
    index = next
    if (index >= phases.length) {
      state = 'done'
      const result = { engine, webgl: options.webglVersion, display: round(baseline), target: round(target) }
      for (const phase of phases) result[phase.name] = results[phase.name]
      // Phases that ended at their maximum load or the time limit, still holding the target:
      // their result is a floor, not the limit.
      result.capped = capped
      log(`result ${JSON.stringify(result)}`)
      return
    }
    const phase = phases[index]
    state = 'phase'
    load = phase.start
    held = 0
    heldFps = 0
    slow = 0
    phaseWindows = 0
    log(`${phase.name}: ${phase.label}`)
    scenes[phase.name].enter()
    scenes[phase.name].setLoad(load)
  }

  function endPhase(stillHolding) {
    const phase = phases[index]
    results[phase.name] = held
    if (stillHolding) capped.push(phase.name)
    log(`${phase.name}: ${held} ${phase.unit} held ${round(target)} fps` +
        (held ? ` (${round(heldFps)})` : ' -- not even the starting load') +
        (stillHolding ? ` -- stopped at the ${held >= phase.max ? 'maximum load' : 'time limit'}, so the limit is higher` : ''))
    scenes[phase.name].exit()
    enterPhase(index + 1)
  }

  function onWindow() {
    if (state === 'baseline') {
      if (windows === 1) return // start-up
      baseline = Math.max(baseline, fps)
      if (windows < 3) return
      target = options.target || Math.min(baseline, 60) * TARGET_SHARE
      log(`display ${round(baseline)} fps, WebGL${options.webglVersion}; target ${round(target)} fps`)
      enterPhase(0)
      return
    }
    if (state !== 'phase') return
    const phase = phases[index]
    phaseWindows++
    log(`${phase.name}: ${load} ${phase.unit}: ${round(fps)} fps`)
    if (fps >= target) {
      held = load
      heldFps = fps
      slow = 0
      if (load >= phase.max || phaseWindows >= MAX_WINDOWS) return endPhase(true)
      load = nextLoad(phase, load)
      scenes[phase.name].setLoad(load)
    } else if (++slow >= 2 && held === 0 && load > 1 && phaseWindows < MAX_WINDOWS) {
      // Nothing has held yet: start again from half the load, in a fresh scene.
      load = Math.max(1, Math.floor(load / 2))
      slow = 0
      log(`${phase.name}: trying ${load} ${phase.unit}`)
      scenes[phase.name].exit()
      scenes[phase.name].enter()
      scenes[phase.name].setLoad(load)
    } else if (slow >= 2 || phaseWindows >= MAX_WINDOWS) {
      endPhase(false)
    }
  }

  return {
    // Once per rendered frame, before drawing. True when the phase just changed -- the only time to
    // redraw a status line: canvas text costs a Raspberry Pi 3 about 100 ms, which redrawn every
    // window would be measured as the load's cost. Each window's numbers go to the log instead.
    update(dt, now) {
      if (state === 'phase') scenes[phases[index].name].update(Math.min(dt, 3))
      frames++
      const elapsed = now - windowStart
      if (elapsed < 1000) return false
      fps = (frames * 1000) / elapsed
      frames = 0
      windowStart = now
      windows++
      const before = `${state}:${index}`
      onWindow()
      return `${state}:${index}` !== before || windows === 1
    },
    status() {
      if (state === 'baseline') return `${engine} stress | measuring the display | ${round(fps)} fps`
      if (state === 'phase') {
        const phase = phases[index]
        return `${engine} stress | ${index + 1}/${phases.length} ${phase.label} | target ${round(target)} fps`
      }
      const lines = [`${engine} stress done | display ${round(baseline)} fps, WebGL${options.webglVersion}, target ${round(target)} fps`]
      for (const phase of phases) {
        lines.push(`${phase.name.padEnd(11)} ${results[phase.name]}${capped.includes(phase.name) ? '+' : ''} ${phase.unit}`)
      }
      return lines.join('\n')
    },
    get done() {
      return state === 'done'
    },
  }
}

function round(value) {
  return Math.round(value * 10) / 10
}

// A seeded generator (mulberry32): both engines build the same level and the same patterns.
export function random(seed) {
  let s = seed | 0
  return () => {
    s = (s + 0x6d2b79f5) | 0
    let t = Math.imul(s ^ (s >>> 15), 1 | s)
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296
  }
}

// ---- sprites: bunnymark ------------------------------------------------------

export function spawnBunny(rand, width) {
  return {
    x: rand() * width, y: rand() * 360, vx: rand() * 10 - 5, vy: rand() * 10 - 5,
    rotation: 0, spin: (rand() - 0.5) * 0.2, scale: 0.5 + rand() * 0.6, tint: Math.floor(rand() * 0xffffff),
  }
}

export function stepBunny(b, width, dt) {
  b.x += b.vx * dt
  b.y += b.vy * dt
  b.vy += 0.5 * dt
  if (b.x < 0) { b.x = 0; b.vx = -b.vx } else if (b.x > width) { b.x = width; b.vx = -b.vx }
  if (b.y > WORLD_HEIGHT) {
    b.y = WORLD_HEIGHT
    b.vy *= -0.85
    if (b.x % 2 < 1) b.vy -= 3 + (b.x % 3)
  } else if (b.y < 0) {
    b.y = 0
    b.vy = 0
  }
  b.rotation += b.spin * dt
}

// ---- platformer --------------------------------------------------------------

export const TILE_GRASS = 0
export const TILE_DIRT = 1
export const TILE_BRICK = 2
export const TILE_STONE = 3

// A level 400 tiles wide: rolling ground with gaps, brick platforms and stone pillars.
export function makeLevel() {
  const rand = random(7)
  const columns = 400
  const rows = Math.ceil(WORLD_HEIGHT / TILE)
  const tiles = new Int8Array(columns * rows).fill(-1)
  let ground = 18
  for (let x = 0; x < columns; x++) {
    if (x > 10 && x < columns - 10 && rand() < 0.06) {
      x += 1 + Math.floor(rand() * 2) // a gap, two or three tiles
      continue
    }
    if (rand() < 0.12) ground = Math.min(20, Math.max(13, ground + (rand() < 0.5 ? -1 : 1)))
    tiles[ground * columns + x] = TILE_GRASS
    for (let y = ground + 1; y < rows; y++) tiles[y * columns + x] = TILE_DIRT
    if (x % 9 === 0 && rand() < 0.7) {
      const row = ground - 4 - Math.floor(rand() * 3), length = 3 + Math.floor(rand() * 4)
      for (let i = 0; i < length && x + i < columns; i++) tiles[row * columns + x + i] = TILE_BRICK
    }
    if (x % 23 === 5) tiles[(ground - 1) * columns + x] = TILE_STONE
  }
  return { columns, rows, tiles, width: columns * TILE }
}

export function solidAt(level, px, py) {
  const col = Math.floor(px / TILE), row = Math.floor(py / TILE)
  if (col < 0 || col >= level.columns || row < 0 || row >= level.rows) return false
  return level.tiles[row * level.columns + col] >= 0
}

export function spawnEnemy(level, rand) {
  return { x: 16 + rand() * (level.width - 32), y: rand() * 200, vx: (rand() < 0.5 ? -1 : 1) * (0.8 + rand() * 1.4), vy: 0, time: rand() * 16, frame: 0 }
}

// Gravity, walls, floors and patrolling that turns at ledges. The enemy's x is its centre, y its feet.
export function stepEnemy(level, e, dt) {
  const half = 12
  const nx = e.x + e.vx * dt
  if (solidAt(level, nx + Math.sign(e.vx) * half, e.y - 8) || nx < half || nx > level.width - half) e.vx = -e.vx
  else e.x = nx
  e.vy = Math.min(e.vy + 0.5 * dt, 10)
  const ny = e.y + e.vy * dt
  if (e.vy > 0 && solidAt(level, e.x, ny)) {
    e.y = Math.floor(ny / TILE) * TILE
    e.vy = 0
    if (!solidAt(level, e.x + Math.sign(e.vx) * (half + 2), e.y + 4)) e.vx = -e.vx // a ledge
  } else {
    e.y = ny
  }
  if (e.y > WORLD_HEIGHT + 64) {
    e.y = -32 // fell through a gap: drop back in from the top
    e.vy = 0
  }
  e.time += dt
  e.frame = Math.floor(e.time / 10) % 2
}

// The camera runs right and wraps; the player runs with it, standing on whatever is below.
export function stepCamera(camera, level, viewWidth, dt) {
  camera.x += 3 * dt
  if (camera.x > level.width - viewWidth) camera.x = 0
  camera.time += dt
  const px = camera.x + 240
  let py = WORLD_HEIGHT + 40
  for (let row = 0; row < level.rows; row++) {
    if (solidAt(level, px, row * TILE)) { py = row * TILE; break }
  }
  camera.playerX = px
  camera.playerY = py
  camera.playerFrame = Math.floor(camera.time / 6) % 2
}

// ---- shooter -----------------------------------------------------------------

export function makeShooter() {
  return { rand: random(11), bullets: [], time: 0, shots: 0, hits: 0, player: { x: 0, y: 0 }, emitters: [] }
}

export function setBulletCount(shooter, count) {
  while (shooter.bullets.length > count) shooter.bullets.pop()
  while (shooter.bullets.length < count) shooter.bullets.push({ x: 0, y: 0, vx: 0, vy: 0, alive: false })
}

// Five turrets spray rotating rings; a dead bullet respawns at once, so `count` stay in flight.
export function stepShooter(s, width, dt) {
  s.time += dt
  s.player.x = width / 2 + Math.sin(s.time * 0.03) * width * 0.35
  s.player.y = WORLD_HEIGHT - 80
  s.emitters.length = 5
  for (let i = 0; i < 5; i++) s.emitters[i] = { x: (width * (i + 1)) / 6 + Math.sin(s.time * 0.02 + i) * 60, y: 110 }
  const spawnLimit = Math.max(4, Math.ceil(s.bullets.length / 60)) // stagger a fresh wave over a second
  let spawned = 0
  for (const b of s.bullets) {
    if (!b.alive) {
      if (spawned++ >= spawnLimit) continue
      const e = s.emitters[s.shots % 5]
      const angle = s.shots * 0.4 + s.time * 0.05
      const speed = 2.5 + (s.shots % 7) * 0.35
      s.shots++
      b.x = e.x
      b.y = e.y
      b.vx = Math.cos(angle) * speed
      b.vy = Math.sin(angle) * speed
      b.alive = true
      continue
    }
    b.x += b.vx * dt
    b.y += b.vy * dt
    if (b.x < -16 || b.x > width + 16 || b.y < -16 || b.y > WORLD_HEIGHT + 16) b.alive = false
    else if (Math.abs(b.x - s.player.x) < 14 && Math.abs(b.y - s.player.y) < 14) {
      s.hits++
      b.alive = false
    }
  }
}

// ---- particles (the Pixi scene simulates its own; Phaser uses its emitter) -------

export const PARTICLE_LIFE = 60 // frames

// How many bursts of `size` to fire this frame to keep about `count` particles alive.
export function burstsDue(clock, count, size, dt) {
  clock.due += ((count / size) / PARTICLE_LIFE) * dt
  const bursts = Math.floor(clock.due)
  clock.due -= bursts
  return bursts
}

// ---- puzzle ------------------------------------------------------------------

// A board of gems that bob in place while pairs of neighbours swap every few frames.
export function makePuzzle() {
  return { rand: random(5), side: 0, cell: 64, gems: [], clock: 0 }
}

export function setPuzzleSize(p, count) {
  const side = Math.max(1, Math.round(Math.sqrt(count)))
  p.side = side
  p.cell = Math.min(64, (WORLD_HEIGHT - 100) / side)
  p.gems = []
  for (let row = 0; row < side; row++) {
    for (let col = 0; col < side; col++) {
      const x = col * p.cell, y = row * p.cell
      p.gems.push({ color: Math.floor(p.rand() * 6), x, y, fromX: x, fromY: y, toX: x, toY: y, t: 1, phase: p.rand() * 6.28, scale: 1 })
    }
  }
}

// Returns the gems that started a swap this frame, for engines that tween them.
export function stepPuzzle(p, dt) {
  const started = []
  p.clock += dt
  if (p.clock >= 8) {
    p.clock = 0
    const swaps = Math.max(1, Math.floor(p.gems.length / 12))
    for (let i = 0; i < swaps; i++) {
      const a = Math.floor(p.rand() * p.gems.length)
      const right = (a % p.side) + 1 < p.side
      const b = right ? a + 1 : a - 1
      const ga = p.gems[a], gb = p.gems[b]
      if (!gb || ga.t < 1 || gb.t < 1) continue
      ;[ga.toX, gb.toX] = [gb.toX, ga.toX]
      ;[ga.toY, gb.toY] = [gb.toY, ga.toY]
      p.gems[a] = gb
      p.gems[b] = ga
      for (const g of [ga, gb]) { g.fromX = g.x; g.fromY = g.y; g.t = 0; started.push(g) }
    }
  }
  for (const g of p.gems) {
    if (g.t < 1) {
      g.t = Math.min(1, g.t + dt / 12)
      const k = g.t * g.t * (3 - 2 * g.t)
      g.x = g.fromX + (g.toX - g.fromX) * k
      g.y = g.fromY + (g.toY - g.fromY) * k
    }
    g.phase += 0.08 * dt
    g.scale = 1 + Math.sin(g.phase) * 0.08
  }
  return started
}

// ---- vector ------------------------------------------------------------------

// Units with a health bar and a marker, bouncing around: what a strategy or tower-defence game
// redraws with its Graphics every frame.
export function makeShapes() {
  return { rand: random(3), shapes: [] }
}

export function setShapeCount(v, count, width) {
  while (v.shapes.length > count) v.shapes.pop()
  while (v.shapes.length < count) {
    const r = v.rand
    v.shapes.push({ x: r() * width, y: 60 + r() * (WORLD_HEIGHT - 80), vx: r() * 4 - 2, vy: r() * 4 - 2, hp: r(), color: Math.floor(r() * 0xffffff) | 0x404040 })
  }
}

export function stepShapes(v, width, dt) {
  for (const s of v.shapes) {
    s.x += s.vx * dt
    s.y += s.vy * dt
    if (s.x < 10 || s.x > width - 10) s.vx = -s.vx
    if (s.y < 60 || s.y > WORLD_HEIGHT - 10) s.vy = -s.vy
    s.hp = (s.hp + 0.004 * dt) % 1
  }
}

// ---- text --------------------------------------------------------------------

// Where the i-th changing text sits: five columns under the status line.
export function textSlot(i, width) {
  const columns = Math.max(1, Math.floor(width / 250))
  return [20 + (i % columns) * 250, 90 + Math.floor(i / columns) * 30]
}
