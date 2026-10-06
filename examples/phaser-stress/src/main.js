import Phaser from 'phaser'

import * as S from './stress.js'

// A multiphase Phaser stress test: bunnymark, a platformer, a shooter, particles, a puzzle board,
// vector graphics and canvas text, each loaded until the frame rate drops (src/stress.js has the
// procedure and the shared game logic). Every scene is plain Phaser -- a Tilemap layer, TileSprites,
// the particle emitter, tweens, Graphics. The camera zooms the 720-unit-tall world to the screen,
// so a 720p Raspberry Pi and a 1080p TV draw the same scene.

// Phaser renders through WebGL1 whatever the URL says, which is also what a Raspberry Pi 3B+ has.
const options = { ...S.readOptions(), webglVersion: 1 }

const TEXT_FONT = 'Helvetica, Arial, sans-serif'

function viewWidth(scene) {
  const { width, height } = scene.scale.gameSize
  return width / (height / S.WORLD_HEIGHT)
}

const SCENES = {
  sprites(scene) {
    const rand = S.random(1)
    const bunnies = []
    const images = []
    return {
      enter() {},
      setLoad(count) {
        while (images.length < count) {
          const bunny = S.spawnBunny(rand, viewWidth(scene))
          images.push(scene.add.image(bunny.x, bunny.y, 'atlas', 'star').setScale(bunny.scale).setTint(bunny.tint))
          bunnies.push(bunny)
        }
      },
      update(dt) {
        const width = viewWidth(scene)
        for (let i = 0; i < images.length; i++) {
          const bunny = bunnies[i]
          S.stepBunny(bunny, width, dt)
          const image = images[i]
          image.x = bunny.x
          image.y = bunny.y
          image.rotation = bunny.rotation
        }
      },
      exit: () => images.forEach((image) => image.destroy()),
    }
  },

  platformer(scene) {
    const level = S.makeLevel()
    const rand = S.random(9)
    const camera = { x: 0, time: 0 }
    const enemies = []
    const images = []
    let map, far, near, player
    return {
      enter() {
        const data = []
        for (let row = 0; row < level.rows; row++) {
          data.push(Array.from(level.tiles.subarray(row * level.columns, (row + 1) * level.columns)))
        }
        map = scene.make.tilemap({ data, tileWidth: S.TILE, tileHeight: S.TILE })
        map.createLayer(0, map.addTilesetImage('tiles'), 0, 0)
        far = scene.add.tileSprite(0, S.WORLD_HEIGHT - 316, 256, 256, 'hills-far').setOrigin(0).setScrollFactor(0).setDepth(-2)
        near = scene.add.tileSprite(0, S.WORLD_HEIGHT - 256, 256, 256, 'hills-near').setOrigin(0).setScrollFactor(0).setDepth(-1)
        player = scene.add.image(0, 0, 'atlas', 'player-0').setOrigin(0.5, 1).setDepth(1)
      },
      setLoad(count) {
        while (images.length < count) {
          images.push(scene.add.image(0, 0, 'atlas', 'enemy-0').setOrigin(0.5, 1))
          enemies.push(S.spawnEnemy(level, rand))
        }
      },
      update(dt) {
        const width = viewWidth(scene)
        S.stepCamera(camera, level, width, dt)
        for (let i = 0; i < enemies.length; i++) {
          const enemy = enemies[i]
          const image = images[i]
          const frameBefore = enemy.frame
          S.stepEnemy(level, enemy, dt)
          image.x = enemy.x
          image.y = enemy.y
          if (enemy.frame !== frameBefore) image.setFrame(`enemy-${enemy.frame}`)
          image.flipX = enemy.vx < 0
        }
        scene.cameras.main.scrollX = camera.x
        if (far.width !== width) {
          far.setSize(width, 256)
          near.setSize(width, 256)
        }
        far.tilePositionX = camera.x * 0.2
        near.tilePositionX = camera.x * 0.5
        player.setPosition(camera.playerX, camera.playerY).setFrame(`player-${camera.playerFrame}`)
      },
      exit() {
        map.destroy()
        for (const object of [far, near, player, ...images]) object.destroy()
        scene.cameras.main.scrollX = 0
      },
    }
  },

  shooter(scene) {
    const shooter = S.makeShooter()
    const images = []
    let ship
    let turrets = []
    return {
      enter() {
        ship = scene.add.image(0, 0, 'atlas', 'player-0').setScale(1.5)
        turrets = Array.from({ length: 5 }, () => scene.add.image(0, 0, 'atlas', 'enemy-0').setScale(1.5))
      },
      setLoad(count) {
        S.setBulletCount(shooter, count)
        while (images.length < count) {
          images.push(scene.add.image(0, 0, 'atlas', 'bullet').setBlendMode(Phaser.BlendModes.ADD).setVisible(false))
        }
      },
      update(dt) {
        S.stepShooter(shooter, viewWidth(scene), dt)
        for (let i = 0; i < images.length; i++) {
          const bullet = shooter.bullets[i]
          const image = images[i]
          image.visible = bullet.alive
          image.x = bullet.x
          image.y = bullet.y
        }
        ship.setPosition(shooter.player.x, shooter.player.y)
        shooter.emitters.forEach((emitter, i) => turrets[i].setPosition(emitter.x, emitter.y))
      },
      exit: () => [ship, ...turrets, ...images].forEach((image) => image.destroy()),
    }
  },

  particles(scene) {
    const BURST = 40
    const rand = S.random(13)
    const clock = { due: 0 }
    let emitter
    let wanted = 0
    return {
      enter() {
        // Phaser's own particle system, with the motion the Pixi scene simulates by hand.
        emitter = scene.add.particles(0, 0, 'atlas', {
          frame: 'spark',
          lifespan: (S.PARTICLE_LIFE * 1000) / 60,
          speed: { min: 60, max: 240 },
          angle: { min: 0, max: 360 },
          gravityY: 400,
          scale: { start: 1.5, end: 0.5 },
          alpha: { start: 1, end: 0 },
          blendMode: 'ADD',
          emitting: false,
        })
      },
      setLoad(count) {
        wanted = count
      },
      update(dt) {
        const width = viewWidth(scene)
        for (let burst = S.burstsDue(clock, wanted, BURST, dt); burst > 0; burst--) {
          emitter.explode(BURST, 100 + rand() * (width - 200), 100 + rand() * 400)
        }
      },
      exit: () => emitter.destroy(),
    }
  },

  puzzle(scene) {
    const puzzle = S.makePuzzle()
    let images = []
    let left = 0
    const clear = () => {
      scene.tweens.killTweensOf(images)
      images.forEach((image) => image.destroy())
      images = []
    }
    return {
      enter() {},
      setLoad(count) {
        clear()
        S.setPuzzleSize(puzzle, count)
        left = (viewWidth(scene) - puzzle.side * puzzle.cell) / 2
        const half = puzzle.cell / 2
        const base = puzzle.cell / 44
        puzzle.gems.forEach((gem, i) => {
          gem.view = scene.add.image(left + gem.x + half, 90 + gem.y + half, 'atlas', `gem-${gem.color}`).setScale(base)
          images.push(gem.view)
          // The idle bob, as a Phaser puzzle game does it: a tween per gem.
          scene.tweens.add({
            targets: gem.view, scale: base * 1.08, duration: 400 + (i % 5) * 40,
            yoyo: true, repeat: -1, ease: 'Sine.easeInOut',
          })
        })
      },
      update(dt) {
        const half = puzzle.cell / 2
        for (const gem of S.stepPuzzle(puzzle, dt)) {
          scene.tweens.add({ targets: gem.view, x: left + gem.toX + half, y: 90 + gem.toY + half, duration: 200, ease: 'Sine.easeInOut' })
        }
      },
      exit: clear,
    }
  },

  vector(scene) {
    const shapes = S.makeShapes()
    let graphics
    return {
      enter() {
        graphics = scene.add.graphics()
      },
      setLoad: (count) => S.setShapeCount(shapes, count, viewWidth(scene)),
      update(dt) {
        S.stepShapes(shapes, viewWidth(scene), dt)
        graphics.clear()
        for (const s of shapes.shapes) {
          graphics.fillStyle(0x222222).fillRect(s.x - 12, s.y - 16, 24, 4)
          graphics.fillStyle(0x06d6a0).fillRect(s.x - 12, s.y - 16, 24 * s.hp, 4)
          graphics.fillStyle(s.color).fillCircle(s.x, s.y, 7)
        }
      },
      exit: () => graphics.destroy(),
    }
  },

  text(scene) {
    const texts = []
    let frameNumber = 0
    return {
      enter() {},
      setLoad(count) {
        while (texts.length < count) {
          const [x, y] = S.textSlot(texts.length, viewWidth(scene))
          texts.push(scene.add.text(x, y, '', { fontFamily: TEXT_FONT, fontSize: '20px', color: '#ffd166' }))
        }
      },
      update() {
        frameNumber++
        for (let i = 0; i < texts.length; i++) texts[i].setText(`text ${i}: frame ${frameNumber}`)
      },
      exit: () => texts.forEach((text) => text.destroy()),
    }
  },
}

class Stress extends Phaser.Scene {
  preload() {
    this.load.atlas('atlas', '/assets/atlas.png', '/assets/atlas.json')
    this.load.image('tiles', '/assets/tiles.png')
    this.load.image('hills-far', '/assets/hills-far.png')
    this.load.image('hills-near', '/assets/hills-near.png')
  }

  create() {
    // Zoom from the top-left corner, so world (0, 0) stays at the screen's.
    this.cameras.main.setOrigin(0, 0)
    this.status = this.add
      .text(12, 8, '', { fontFamily: TEXT_FONT, fontSize: '20px', color: '#ffffff', stroke: '#000000', strokeThickness: 4 })
      .setScrollFactor(0)
      .setDepth(100)

    // Each scene is built when its phase begins, so one phase's objects never weigh on another.
    const scenes = {}
    for (const [name, make] of Object.entries(SCENES)) {
      let current = null
      scenes[name] = {
        enter: () => (current = make(this)).enter(),
        setLoad: (count) => current.setLoad(count),
        update: (dt) => current.update(dt),
        exit: () => current.exit(),
      }
    }
    this.stress = S.createStress({ engine: 'phaser', scenes, options })
  }

  update(time, delta) {
    this.cameras.main.setZoom(this.scale.gameSize.height / S.WORLD_HEIGHT)
    if (this.stress.update(delta / (1000 / 60), performance.now())) this.status.setText(this.stress.status())
  }
}

new Phaser.Game({
  type: Phaser.WEBGL,
  backgroundColor: '#1b263b',
  scale: { mode: Phaser.Scale.RESIZE, width: window.innerWidth, height: window.innerHeight },
  scene: Stress,
})
