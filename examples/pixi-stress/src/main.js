import {
  Application,
  Assets,
  Container,
  CullerPlugin,
  Graphics,
  Particle,
  ParticleContainer,
  Rectangle,
  Sprite,
  Text,
  Texture,
  TilingSprite,
  extensions,
} from 'pixi.js'

import * as S from './stress.js'

// A multiphase PixiJS stress test: bunnymark, a platformer, a shooter, particles, a puzzle board,
// vector graphics and canvas text, each loaded until the frame rate drops (src/stress.js has the
// procedure and the shared game logic). Every scene is plain Pixi. The world is 720 units tall and
// scaled to the screen, so a 720p Raspberry Pi and a 1080p TV draw the same scene.

// Off-screen tile chunks and enemies are skipped, as a real platformer would arrange.
extensions.add(CullerPlugin)

;(async () => {
  const options = S.readOptions()
  const app = new Application()
  await app.init({ background: '#1b263b', resizeTo: window, preferWebGLVersion: options.webglVersion })
  document.body.appendChild(app.canvas)

  // Decode textures on the main thread: ScreenKit has no Web Workers.
  Assets.setPreferences({ preferWorkers: false })
  const sheet = await Assets.load('/assets/atlas.json')
  const images = await Assets.load(['/assets/tiles.png', '/assets/hills-far.png', '/assets/hills-near.png'])
  const frame = (name) => sheet.textures[name]
  const tileTextures = [0, 1, 2, 3].map(
    (i) => new Texture({ source: images['/assets/tiles.png'].source, frame: new Rectangle(i * S.TILE, 0, S.TILE, S.TILE) }),
  )

  const world = new Container()
  const status = new Text({
    text: '',
    style: { fontFamily: 'Helvetica, Arial, sans-serif', fontSize: 20, fill: '#ffffff', stroke: { color: '#000000', width: 4 } },
  })
  status.position.set(12, 8)
  app.stage.addChild(world, status)

  // The screen in world units: 720 tall, as wide as the aspect ratio makes it.
  const viewWidth = () => app.screen.width / (app.screen.height / S.WORLD_HEIGHT)

  const scenes = {
    sprites() {
      const rand = S.random(1)
      const layer = new Container()
      const bunnies = []
      const sprites = []
      return {
        enter: () => world.addChild(layer),
        setLoad(count) {
          while (sprites.length < count) {
            const bunny = S.spawnBunny(rand, viewWidth())
            const sprite = new Sprite(frame('star'))
            sprite.anchor.set(0.5)
            sprite.scale.set(bunny.scale)
            sprite.tint = bunny.tint
            layer.addChild(sprite)
            sprites.push(sprite)
            bunnies.push(bunny)
          }
        },
        update(dt) {
          const width = viewWidth()
          for (let i = 0; i < sprites.length; i++) {
            const bunny = bunnies[i]
            S.stepBunny(bunny, width, dt)
            sprites[i].position.set(bunny.x, bunny.y)
            sprites[i].rotation = bunny.rotation
          }
        },
        exit: () => layer.destroy({ children: true }),
      }
    },

    platformer() {
      const level = S.makeLevel()
      const rand = S.random(9)
      const camera = { x: 0, time: 0 }
      const far = new TilingSprite({ texture: images['/assets/hills-far.png'], width: 256, height: 256 })
      const near = new TilingSprite({ texture: images['/assets/hills-near.png'], width: 256, height: 256 })
      const level2d = new Container()
      const actors = new Container()
      const player = new Sprite(frame('player-0'))
      const enemyFrames = [frame('enemy-0'), frame('enemy-1')]
      const playerFrames = [frame('player-0'), frame('player-1')]
      const enemies = []
      const sprites = []
      return {
        enter() {
          // The tilemap as 16-column chunks the culler can skip whole.
          for (let first = 0; first < level.columns; first += 16) {
            const chunk = new Container()
            chunk.cullable = true
            for (let col = first; col < Math.min(first + 16, level.columns); col++) {
              for (let row = 0; row < level.rows; row++) {
                const tile = level.tiles[row * level.columns + col]
                if (tile < 0) continue
                const sprite = new Sprite(tileTextures[tile])
                sprite.position.set(col * S.TILE, row * S.TILE)
                chunk.addChild(sprite)
              }
            }
            level2d.addChild(chunk)
          }
          player.anchor.set(0.5, 1)
          far.y = S.WORLD_HEIGHT - 316
          near.y = S.WORLD_HEIGHT - 256
          level2d.addChild(actors, player)
          world.addChild(far, near, level2d)
        },
        setLoad(count) {
          while (sprites.length < count) {
            const sprite = new Sprite(enemyFrames[0])
            sprite.anchor.set(0.5, 1)
            sprite.cullable = true
            actors.addChild(sprite)
            sprites.push(sprite)
            enemies.push(S.spawnEnemy(level, rand))
          }
        },
        update(dt) {
          const width = viewWidth()
          S.stepCamera(camera, level, width, dt)
          for (let i = 0; i < enemies.length; i++) {
            const enemy = enemies[i]
            S.stepEnemy(level, enemy, dt)
            const sprite = sprites[i]
            sprite.position.set(enemy.x, enemy.y)
            sprite.texture = enemyFrames[enemy.frame]
            sprite.scale.x = enemy.vx < 0 ? -1 : 1
          }
          level2d.x = -camera.x
          far.width = width
          near.width = width
          far.tilePosition.x = -camera.x * 0.2
          near.tilePosition.x = -camera.x * 0.5
          player.position.set(camera.playerX, camera.playerY)
          player.texture = playerFrames[camera.playerFrame]
        },
        exit() {
          far.destroy()
          near.destroy()
          level2d.destroy({ children: true })
        },
      }
    },

    shooter() {
      const shooter = S.makeShooter()
      const layer = new Container()
      const ship = new Sprite(frame('player-0'))
      const turrets = Array.from({ length: 5 }, () => new Sprite(frame('enemy-0')))
      const sprites = []
      return {
        enter() {
          for (const sprite of [ship, ...turrets]) {
            sprite.anchor.set(0.5)
            sprite.scale.set(1.5)
          }
          layer.addChild(ship, ...turrets)
          world.addChild(layer)
        },
        setLoad(count) {
          S.setBulletCount(shooter, count)
          while (sprites.length < count) {
            const sprite = new Sprite(frame('bullet'))
            sprite.anchor.set(0.5)
            sprite.blendMode = 'add'
            sprite.visible = false
            layer.addChild(sprite)
            sprites.push(sprite)
          }
        },
        update(dt) {
          S.stepShooter(shooter, viewWidth(), dt)
          for (let i = 0; i < sprites.length; i++) {
            const bullet = shooter.bullets[i]
            const sprite = sprites[i]
            sprite.visible = bullet.alive
            sprite.position.set(bullet.x, bullet.y)
          }
          ship.position.set(shooter.player.x, shooter.player.y)
          shooter.emitters.forEach((emitter, i) => turrets[i].position.set(emitter.x, emitter.y))
        },
        exit: () => layer.destroy({ children: true }),
      }
    },

    particles() {
      const BURST = 40
      const rand = S.random(13)
      const clock = { due: 0 }
      // Pixi's high-count path: a ParticleContainer, simulated by hand as a Pixi game would.
      const container = new ParticleContainer({
        dynamicProperties: { position: true, vertex: true, color: true, rotation: false, uvs: false },
      })
      container.blendMode = 'add'
      const particles = []
      const motion = []
      let wanted = 0
      return {
        enter: () => world.addChild(container),
        setLoad(count) {
          wanted = count
          while (particles.length < count) {
            const particle = new Particle({ texture: frame('spark'), anchorX: 0.5, anchorY: 0.5, alpha: 0 })
            container.addParticle(particle)
            particles.push(particle)
            motion.push({ life: 0, vx: 0, vy: 0 })
          }
        },
        update(dt) {
          const width = viewWidth()
          let cursor = 0
          for (let burst = S.burstsDue(clock, wanted, BURST, dt); burst > 0; burst--) {
            const x = 100 + rand() * (width - 200)
            const y = 100 + rand() * 400
            for (let fired = 0; fired < BURST && cursor < particles.length; cursor++) {
              const m = motion[cursor]
              if (m.life > 0) continue
              const angle = rand() * Math.PI * 2
              const speed = 1 + rand() * 3
              m.life = S.PARTICLE_LIFE
              m.vx = Math.cos(angle) * speed
              m.vy = Math.sin(angle) * speed
              particles[cursor].x = x
              particles[cursor].y = y
              fired++
            }
          }
          for (let i = 0; i < particles.length; i++) {
            const m = motion[i]
            const particle = particles[i]
            if (m.life <= 0) {
              particle.alpha = 0
              continue
            }
            m.life -= dt
            m.vy += 0.11 * dt
            particle.x += m.vx * dt
            particle.y += m.vy * dt
            const left = Math.max(0, m.life / S.PARTICLE_LIFE)
            particle.alpha = left
            particle.scaleX = particle.scaleY = 0.5 + left
          }
        },
        exit: () => container.destroy(),
      }
    },

    puzzle() {
      const puzzle = S.makePuzzle()
      const layer = new Container()
      let left = 0
      return {
        enter: () => world.addChild(layer),
        setLoad(count) {
          layer.removeChildren().forEach((child) => child.destroy())
          S.setPuzzleSize(puzzle, count)
          for (const gem of puzzle.gems) {
            gem.view = new Sprite(frame(`gem-${gem.color}`))
            gem.view.anchor.set(0.5)
            layer.addChild(gem.view)
          }
          left = (viewWidth() - puzzle.side * puzzle.cell) / 2
        },
        update(dt) {
          S.stepPuzzle(puzzle, dt)
          const half = puzzle.cell / 2
          const base = puzzle.cell / 44
          for (const gem of puzzle.gems) {
            gem.view.position.set(left + gem.x + half, 90 + gem.y + half)
            gem.view.scale.set(base * gem.scale)
          }
        },
        exit: () => layer.destroy({ children: true }),
      }
    },

    vector() {
      const shapes = S.makeShapes()
      const graphics = new Graphics()
      return {
        enter: () => world.addChild(graphics),
        setLoad: (count) => S.setShapeCount(shapes, count, viewWidth()),
        update(dt) {
          S.stepShapes(shapes, viewWidth(), dt)
          graphics.clear()
          for (const s of shapes.shapes) {
            graphics.rect(s.x - 12, s.y - 16, 24, 4).fill(0x222222)
            graphics.rect(s.x - 12, s.y - 16, 24 * s.hp, 4).fill(0x06d6a0)
            graphics.circle(s.x, s.y, 7).fill(s.color)
          }
        },
        exit: () => graphics.destroy(),
      }
    },

    text() {
      const layer = new Container()
      const style = { fontFamily: 'Helvetica, Arial, sans-serif', fontSize: 20, fill: '#ffd166' }
      const texts = []
      let frameNumber = 0
      return {
        enter: () => world.addChild(layer),
        setLoad(count) {
          while (texts.length < count) {
            const text = new Text({ text: '', style })
            text.position.set(...S.textSlot(texts.length, viewWidth()))
            layer.addChild(text)
            texts.push(text)
          }
        },
        update() {
          frameNumber++
          for (let i = 0; i < texts.length; i++) texts[i].text = `text ${i}: frame ${frameNumber}`
        },
        exit: () => layer.destroy({ children: true }),
      }
    },
  }

  // Each scene is built when its phase begins, so one phase's objects never weigh on another.
  const lazyScenes = {}
  for (const [name, make] of Object.entries(scenes)) {
    let scene = null
    lazyScenes[name] = {
      enter: () => (scene = make()).enter(),
      setLoad: (count) => scene.setLoad(count),
      update: (dt) => scene.update(dt),
      exit: () => scene.exit(),
    }
  }

  const stress = S.createStress({ engine: 'pixi', scenes: lazyScenes, options })
  app.ticker.add((ticker) => {
    world.scale.set(app.screen.height / S.WORLD_HEIGHT)
    if (stress.update(ticker.deltaTime, performance.now())) status.text = stress.status()
  })
})()
