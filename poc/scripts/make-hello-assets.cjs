// Assets for the PixiJS and Phaser hello worlds (poc/pixi-hello, poc/phaser-hello).
//
//   node poc/scripts/make-hello-assets.cjs <out-dir>
//
// Writes, from Lato Black (poc/blits-example-app/public/fonts, SIL Open Font
// License):
//   lato-black-msdf.xml + .png   an MSDF bitmap font, BMFont XML -- PixiJS
//   lato-black-plain.xml + .png  the same glyphs as a plain alpha atlas -- Phaser,
//                                whose BitmapText has no distance-field support
//   logo.png                     a 256x256 rounded-square logo
//
// msdf-bmfont-xml comes from poc/blits-example-app's node_modules (npm install
// there first); nothing here is a dependency of the samples themselves.
const path = require('path')
const generateBMFont = require(require.resolve('msdf-bmfont-xml', { paths: [path.join(__dirname, '..', 'blits-example-app')] }))
const fs = require('fs')
const zlib = require('zlib')
const out = process.argv[2]
fs.mkdirSync(out, { recursive: true })
const charset = Array.from({ length: 95 }, (_, i) => String.fromCharCode(32 + i)).join('')
const range = 4

function decodePng(buf) {
  let pos = 8, w, h, idat = []
  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos), type = buf.toString('ascii', pos + 4, pos + 8), body = buf.subarray(pos + 8, pos + 8 + len)
    if (type === 'IHDR') { w = body.readUInt32BE(0); h = body.readUInt32BE(4); if (body[8] !== 8 || body[9] !== 6) throw new Error('need RGBA8') }
    if (type === 'IDAT') idat.push(body)
    pos += 12 + len
  }
  const raw = zlib.inflateSync(Buffer.concat(idat)), bpp = 4, stride = w * bpp, px = Buffer.alloc(w * h * 4)
  let prev = Buffer.alloc(stride)
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)], line = Buffer.from(raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1)))
    for (let x = 0; x < stride; x++) {
      const a = x >= bpp ? line[x - bpp] : 0, b = prev[x], c = x >= bpp ? prev[x - bpp] : 0
      if (f === 1) line[x] = (line[x] + a) & 255
      else if (f === 2) line[x] = (line[x] + b) & 255
      else if (f === 3) line[x] = (line[x] + ((a + b) >> 1)) & 255
      else if (f === 4) { const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c); line[x] = (line[x] + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c)) & 255 }
    }
    line.copy(px, y * stride); prev = line
  }
  return { w, h, px }
}
function crc32(buf) { let c, crc = 0xffffffff; for (let n = 0; n < buf.length; n++) { c = (crc ^ buf[n]) & 0xff; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; crc = (crc >>> 8) ^ c } return (crc ^ 0xffffffff) >>> 0 }
function chunk(type, data) { const len = Buffer.alloc(4); len.writeUInt32BE(data.length); const td = Buffer.concat([Buffer.from(type), data]); const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td)); return Buffer.concat([len, td, crc]) }
function encodePng(w, h, px) {
  const rows = []; for (let y = 0; y < h; y++) rows.push(Buffer.concat([Buffer.from([0]), px.subarray(y * w * 4, (y + 1) * w * 4)]))
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(Buffer.concat(rows))), chunk('IEND', Buffer.alloc(0))])
}

generateBMFont(path.join(__dirname, '..', 'blits-example-app', 'public', 'fonts', 'Lato-Black.ttf'),
  { outputType: 'xml', fieldType: 'msdf', distanceRange: range, fontSize: 64, textureSize: [512, 512], charset, filename: 'lato-black' },
  (error, textures, font) => {
    if (error) throw error
    const msdfPng = textures[0].texture
    fs.writeFileSync(path.join(out, 'lato-black-msdf.png'), msdfPng)
    const xml = String(font.data).replace(/file="[^"]*"/, 'file="lato-black-msdf.png"')
    fs.writeFileSync(path.join(out, 'lato-black-msdf.xml'), xml)
    // Plain: white glyphs, alpha from the median distance at the atlas's own scale.
    const { w, h, px } = decodePng(msdfPng)
    for (let i = 0; i < px.length; i += 4) {
      const [r, g, b] = [px[i], px[i + 1], px[i + 2]]
      const median = Math.max(Math.min(r, g), Math.min(Math.max(r, g), b)) / 255
      const alpha = Math.min(1, Math.max(0, (median - 0.5) * range + 0.5))
      px[i] = px[i + 1] = px[i + 2] = 255; px[i + 3] = Math.round(alpha * 255)
    }
    fs.writeFileSync(path.join(out, 'lato-black-plain.png'), encodePng(w, h, px))
    fs.writeFileSync(path.join(out, 'lato-black-plain.xml'),
      xml.replace(/file="lato-black-msdf.png"/, 'file="lato-black-plain.png"').replace(/\s*<distanceField[^>]*\/>/, ''))
    console.log('wrote', fs.readdirSync(out).join(' '))
  })

// The logo: a rounded square with a diagonal gradient and a ring, antialiased.
const W = 256
const clamp = (v) => Math.max(0, Math.min(1, v))
const rows = []
for (let y = 0; y < W; y++) {
  const row = [0]
  for (let x = 0; x < W; x++) {
    const px = x + 0.5 - W / 2, py = y + 0.5 - W / 2
    const r = 48, half = W / 2 - 8
    const qx = Math.abs(px) - (half - r), qy = Math.abs(py) - (half - r)
    const box = Math.hypot(Math.max(qx, 0), Math.max(qy, 0)) + Math.min(Math.max(qx, qy), 0) - r
    const inside = clamp(0.5 - box)
    const ring = clamp(0.5 - Math.abs(Math.hypot(px, py) - 62) + 8)
    const t = (x + y) / (2 * W)
    const base = [236 * (1 - t) + 99 * t, 72 * (1 - t) + 102 * t, 153 * (1 - t) + 241 * t]
    const color = base.map((c) => c * (1 - ring) + 255 * ring)
    row.push(...color.map((c) => Math.round(c)), Math.round(255 * inside))
  }
  rows.push(Buffer.from(row))
}
const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(W, 4); ihdr[8] = 8; ihdr[9] = 6
fs.writeFileSync(path.join(out, 'logo.png'), Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
  chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(Buffer.concat(rows))), chunk('IEND', Buffer.alloc(0))]))
console.log('logo.png')
