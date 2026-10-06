// Generates the stress tests' art, identical for poc/pixi-stress and poc/phaser-stress:
//
//   atlas.png + atlas.json   every sprite, one texture (TexturePacker "JSON hash", which Pixi's
//                            Spritesheet and Phaser's load.atlas both read)
//   tiles.png                a 32 px tileset: grass, dirt, brick, stone
//   hills-far.png            parallax layers that repeat horizontally
//   hills-near.png
//
// Every image is a power of two and at most 256 px, so it repeats and mips under WebGL1 on a
// Raspberry Pi's VideoCore GPU. Drawn procedurally with 4x4 supersampling; no inputs, no dependencies.
//
//   node make-stress-assets.cjs <public/assets dir>...
const fs = require('fs')
const path = require('path')
const zlib = require('zlib')

function crc32(buf) {
  let crc = 0xffffffff
  for (let n = 0; n < buf.length; n++) {
    let c = (crc ^ buf[n]) & 0xff
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1
    crc = (crc >>> 8) ^ c
  }
  return (crc ^ 0xffffffff) >>> 0
}
function chunk(type, data) {
  const length = Buffer.alloc(4)
  length.writeUInt32BE(data.length)
  const body = Buffer.concat([Buffer.from(type), data])
  const crc = Buffer.alloc(4)
  crc.writeUInt32BE(crc32(body))
  return Buffer.concat([length, body, crc])
}
function encodePng(image) {
  const header = Buffer.alloc(13)
  header.writeUInt32BE(image.w, 0)
  header.writeUInt32BE(image.h, 4)
  header[8] = 8 // bit depth
  header[9] = 6 // RGBA
  const raw = Buffer.alloc((image.w * 4 + 1) * image.h)
  for (let y = 0; y < image.h; y++) {
    raw[y * (image.w * 4 + 1)] = 0
    Buffer.from(image.px.buffer, y * image.w * 4, image.w * 4).copy(raw, y * (image.w * 4 + 1) + 1)
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', header),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ])
}

function makeImage(w, h) {
  return { w, h, px: new Uint8Array(w * h * 4) }
}

// Paint a w x h region at (x0, y0): `shade(u, v)` gives [r, g, b, a] (a 0-1) or null at a point
// in region pixels; each pixel averages 16 samples, premultiplied, so edges are antialiased.
function paint(image, x0, y0, w, h, shade) {
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      let r = 0, g = 0, b = 0, a = 0
      for (let sy = 0; sy < 4; sy++) {
        for (let sx = 0; sx < 4; sx++) {
          const c = shade(x + (sx + 0.5) / 4, y + (sy + 0.5) / 4)
          if (!c) continue
          r += c[0] * c[3]; g += c[1] * c[3]; b += c[2] * c[3]; a += c[3]
        }
      }
      if (a === 0) continue
      const i = ((y0 + y) * image.w + x0 + x) * 4
      image.px[i] = Math.round(r / a)
      image.px[i + 1] = Math.round(g / a)
      image.px[i + 2] = Math.round(b / a)
      image.px[i + 3] = Math.round((a / 16) * 255)
    }
  }
}

const hex = (value, alpha = 1) => [(value >> 16) & 255, (value >> 8) & 255, value & 255, alpha]
const mix = (a, b, t) => a.map((v, i) => v + (b[i] - v) * t)

// ---- the atlas ----------------------------------------------------------------

const atlas = makeImage(256, 128)
const frames = {}
function frame(name, x, y, w, h, shade) {
  paint(atlas, x, y, w, h, shade)
  frames[name] = {
    frame: { x, y, w, h }, rotated: false, trimmed: false,
    spriteSourceSize: { x: 0, y: 0, w, h }, sourceSize: { w, h },
  }
}

// A five-pointed star, for the bunnymark phase.
frame('star', 0, 0, 32, 32, (u, v) => {
  const dx = u - 16, dy = v - 16.5, angle = Math.atan2(dy, dx) + Math.PI / 2
  const radius = Math.hypot(dx, dy)
  const spike = 0.5 + 0.5 * Math.cos(5 * angle)
  return radius < 7 + 8.5 * spike ** 2 ? mix(hex(0xfff3b0), hex(0xffb703), radius / 15) : null
})

// The platformer's player: two running frames.
for (let f = 0; f < 2; f++) {
  frame(`player-${f}`, 32 + f * 24, 0, 24, 32, (u, v) => {
    if (Math.hypot(u - 12, v - 7) < 6) return hex(0xffd6a5) // head
    if (u > 6 && u < 18 && v > 12 && v < 23) return hex(0x3a86ff) // body
    const stride = f === 0 ? 3 : -3
    if (v >= 23 && v < 32 && (Math.abs(u - (9 + stride)) < 2.2 || Math.abs(u - (15 - stride)) < 2.2)) return hex(0x1d3557)
    return null
  })
}

// Enemies: a slime, squashing between frames.
for (let f = 0; f < 2; f++) {
  frame(`enemy-${f}`, 80 + f * 28, 0, 28, 24, (u, v) => {
    const rx = f === 0 ? 12 : 13.5, ry = f === 0 ? 11 : 9
    const dx = (u - 14) / rx, dy = (v - (24 - ry)) / ry
    if (dx * dx + dy * dy > 1 || v > 24) return null
    if (Math.hypot(u - 10, v - (24 - ry * 1.2)) < 2 || Math.hypot(u - 18, v - (24 - ry * 1.2)) < 2) return hex(0x10002b)
    return mix(hex(0x80ed99), hex(0x2d6a4f), (v - (24 - 2 * ry)) / (2 * ry))
  })
}

// A bullet with a hot core, and a soft spark for particles (both drawn additively).
frame('bullet', 136, 0, 12, 12, (u, v) => {
  const r = Math.hypot(u - 6, v - 6)
  return r < 5.5 ? mix(hex(0xffffff), hex(0xff4d6d), Math.min(1, r / 5)) : null
})
frame('spark', 148, 0, 16, 16, (u, v) => {
  const r = Math.hypot(u - 8, v - 8) / 8
  return r < 1 ? [255, 220, 150, (1 - r) ** 2] : null
})

// Six gems for the puzzle board: rounded squares with a highlight.
const GEM_COLORS = [0xef476f, 0xffd166, 0x06d6a0, 0x118ab2, 0x9b5de5, 0xf78c6b]
GEM_COLORS.forEach((color, index) => {
  frame(`gem-${index}`, index * 40, 40, 40, 40, (u, v) => {
    const qx = Math.max(Math.abs(u - 20) - 12, 0), qy = Math.max(Math.abs(v - 20) - 12, 0)
    if (Math.hypot(qx, qy) > 6) return null
    const base = hex(color)
    if (Math.hypot(u - 14, v - 13) < 5) return mix(base, hex(0xffffff), 0.6)
    return mix(base, hex(0x000000), Math.max(0, (v - 20) / 60))
  })
})

// ---- tiles and parallax -----------------------------------------------------

const tiles = makeImage(128, 32)
const speck = (u, v, seed) => ((Math.floor(u) * 73856093) ^ (Math.floor(v) * 19349663) ^ seed) % 7 === 0
paint(tiles, 0, 0, 32, 32, (u, v) => (v < 7 ? hex(0x52b788) : speck(u, v, 1) ? hex(0x6f4518) : hex(0x99582a)))
paint(tiles, 32, 0, 32, 32, (u, v) => (speck(u, v, 2) ? hex(0x6f4518) : hex(0x99582a)))
paint(tiles, 64, 0, 32, 32, (u, v) => {
  const row = Math.floor(v / 8), offset = row % 2 ? 8 : 0
  const mortar = v % 8 < 1 || (u + offset) % 16 < 1
  return mortar ? hex(0x495057) : hex(0xadb5bd)
})
paint(tiles, 96, 0, 32, 32, (u, v) => (speck(u, v, 3) ? hex(0x495057) : hex(0x6c757d)))

// Hills whose outline repeats every 256 px, so the layer tiles seamlessly.
function hills(color, height, waves) {
  const image = makeImage(256, 256)
  paint(image, 0, 0, 256, 256, (u, v) => {
    let top = 256 - height
    waves.forEach(([count, amplitude, phase]) => { top += amplitude * Math.sin((u / 256) * Math.PI * 2 * count + phase) })
    return v > top ? hex(color) : null
  })
  return image
}
const hillsFar = hills(0x3d5a80, 150, [[1, 30, 0.3], [3, 12, 1.7]])
const hillsNear = hills(0x293241, 90, [[2, 22, 2.1], [5, 6, 0.4]])

const outputs = process.argv.slice(2)
if (outputs.length === 0) {
  console.error('usage: node make-stress-assets.cjs <public/assets dir>...')
  process.exit(1)
}
const json = JSON.stringify({ frames, meta: { image: 'atlas.png', format: 'RGBA8888', size: { w: atlas.w, h: atlas.h }, scale: '1' } }, null, 1)
for (const dir of outputs) {
  fs.mkdirSync(dir, { recursive: true })
  fs.writeFileSync(path.join(dir, 'atlas.png'), encodePng(atlas))
  fs.writeFileSync(path.join(dir, 'atlas.json'), json)
  fs.writeFileSync(path.join(dir, 'tiles.png'), encodePng(tiles))
  fs.writeFileSync(path.join(dir, 'hills-far.png'), encodePng(hillsFar))
  fs.writeFileSync(path.join(dir, 'hills-near.png'), encodePng(hillsNear))
  console.log(`wrote ${dir}`)
}
