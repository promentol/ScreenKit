// What the device is, on screen: CPU, GPU and memory.
//
// No engine. The page draws its panel with canvas 2D text and uploads that
// canvas to WebGL as a texture -- which is what Pixi and Phaser do underneath,
// and what makes this a fair little exercise of the runtime's text path. The GPU
// lines come from the GL context itself; the CPU and memory lines come from
// `system.json`, which the host's launcher rewrites once a second from /proc
// (examples/system-info/pi-prelaunch.sh). Without a sampler -- on a Mac, say -- the
// panel says so and still shows everything WebGL knows.
//
// It repaints once a second, not every frame: a screen of canvas text costs more
// than a frame on a Raspberry Pi (EMBEDDED_LINUX_EXPERIMENTS.md).

const SAMPLE_URL = '/system.json'
const REPAINT_MS = 1000

const canvas = document.createElement('canvas')
document.body.appendChild(canvas)
const gl = canvas.getContext('webgl')
if (!gl) throw new Error('system-info needs a WebGL context')

// The panel: an ordinary 2D canvas, uploaded as one texture.
const panel = document.createElement('canvas')
const ctx = panel.getContext('2d')

// ---- the GL side: one textured quad ------------------------------------------

const program = gl.createProgram()
const vertex = gl.createShader(gl.VERTEX_SHADER)
gl.shaderSource(vertex, `
attribute vec2 position;
varying vec2 uv;
void main() {
  uv = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
  gl_Position = vec4(position, 0.0, 1.0);
}`)
gl.compileShader(vertex)
const fragment = gl.createShader(gl.FRAGMENT_SHADER)
gl.shaderSource(fragment, `
precision mediump float;
uniform sampler2D panel;
varying vec2 uv;
void main() { gl_FragColor = texture2D(panel, uv); }`)
gl.compileShader(fragment)
gl.attachShader(program, vertex)
gl.attachShader(program, fragment)
gl.linkProgram(program)
if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
  throw new Error('panel program: ' + gl.getProgramInfoLog(program))
}
gl.useProgram(program)

const quad = gl.createBuffer()
gl.bindBuffer(gl.ARRAY_BUFFER, quad)
gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW)
const position = gl.getAttribLocation(program, 'position')
gl.enableVertexAttribArray(position)
gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 0, 0)

const texture = gl.createTexture()
gl.bindTexture(gl.TEXTURE_2D, texture)
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR)
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR)
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE)
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)
gl.uniform1i(gl.getUniformLocation(program, 'panel'), 0)

// ---- what WebGL knows about the GPU -------------------------------------------

function glString(name) {
  const value = gl.getParameter(gl[name])
  return typeof value === 'string' ? value : String(value)
}

const gpu = {
  renderer: glString('RENDERER'),
  vendor: glString('VENDOR'),
  version: glString('VERSION'),
  shading: glString('SHADING_LANGUAGE_VERSION'),
  maxTexture: gl.getParameter(gl.MAX_TEXTURE_SIZE),
  maxAttributes: gl.getParameter(gl.MAX_VERTEX_ATTRIBS),
  maxUnits: gl.getParameter(gl.MAX_TEXTURE_IMAGE_UNITS),
  webgl2: typeof WebGL2RenderingContext === 'function' && gl instanceof WebGL2RenderingContext ? '2' : '1',
  extensions: (gl.getSupportedExtensions() || []).length,
}

// ---- the sample the launcher writes ---------------------------------------------

let sample = null
let sampleError = null

async function readSample() {
  try {
    const response = await fetch(SAMPLE_URL)
    if (!response.ok) throw new Error('HTTP ' + response.status)
    const next = await response.json()
    sample = next
    sampleError = next.note ? next.note : null
  } catch (error) {
    sampleError = String(error && error.message ? error.message : error)
  }
}

// ---- the panel --------------------------------------------------------------------

const COLORS = { bg: '#0b1020', head: '#8fd3ff', label: '#7c8aa5', value: '#e8eef8', bar: '#4ade80', warn: '#fbbf24' }

function bytes(kb) {
  if (typeof kb !== 'number' || !isFinite(kb) || kb < 0) return '--'
  if (kb === 0) return '0 MB'
  const mb = kb / 1024
  return mb >= 1024 ? (mb / 1024).toFixed(2) + ' GB' : Math.round(mb) + ' MB'
}

function line(text, x, y, color, size, weight) {
  ctx.fillStyle = color
  ctx.font = (weight || '') + ' ' + (size || 18) + 'px Helvetica, Arial, sans-serif'
  ctx.fillText(text, x, y)
}

/// Trim to what fits, with an ellipsis: an ANGLE renderer string is longer than
/// any column here, and a clipped line reads as a bug.
function fit(text, available, size) {
  ctx.font = size + 'px Helvetica, Arial, sans-serif'
  if (ctx.measureText(text).width <= available) return text
  let trimmed = text
  while (trimmed.length > 1 && ctx.measureText(trimmed + '…').width > available) {
    trimmed = trimmed.slice(0, -1)
  }
  return trimmed + '…'
}

function field(label, value, x, y, labelWidth, columnWidth) {
  const gap = Math.round(labelWidth * 0.12)
  line(fit(label, labelWidth - gap, 16), x, y, COLORS.label, 16)
  const text = value === undefined || value === null || value === '' ? '--' : String(value)
  line(fit(text, columnWidth - labelWidth, 16), x + labelWidth, y, COLORS.value, 16)
}

/// A used/total bar, so memory pressure is visible without reading numbers.
function bar(x, y, width, height, fraction, color) {
  ctx.fillStyle = '#1b2437'
  ctx.fillRect(x, y, width, height)
  ctx.fillStyle = color
  ctx.fillRect(x, y, Math.max(0, Math.min(1, fraction)) * width, height)
}

function paint() {
  const width = panel.width
  const height = panel.height
  const pad = Math.round(width * 0.04)
  const column = Math.round((width - pad * 3) / 2)
  const labelWidth = Math.round(column * 0.42)
  const step = Math.max(20, Math.round(height / 26))

  ctx.fillStyle = COLORS.bg
  ctx.fillRect(0, 0, width, height)

  const host = (sample && sample.host) || {}
  line('ScreenKit — this device', pad, pad + step, COLORS.head, 26, 'bold')
  line(host.model || host.kernel || 'system information', pad, pad + step * 2, COLORS.label, 16)

  // Left column: CPU and memory.
  let y = pad + step * 4
  const cpu = (sample && sample.cpu) || {}
  line('CPU', pad, y, COLORS.head, 20, 'bold')
  y += step
  field('model', cpu.model, pad, y, labelWidth, column); y += step
  field('cores', cpu.cores, pad, y, labelWidth, column); y += step
  field('clock', cpu.mhz ? cpu.mhz + ' MHz' : null, pad, y, labelWidth, column); y += step
  field('governor', cpu.governor, pad, y, labelWidth, column); y += step
  field('temperature', cpu.tempC ? cpu.tempC + ' °C' : null, pad, y, labelWidth, column); y += step
  field('load avg', cpu.load, pad, y, labelWidth, column); y += step * 2

  const mem = (sample && sample.memory) || {}
  const used = mem.totalKb && mem.availableKb ? mem.totalKb - mem.availableKb : 0
  line('MEMORY', pad, y, COLORS.head, 20, 'bold')
  y += step
  field('total', bytes(mem.totalKb), pad, y, labelWidth, column); y += step
  field('available', bytes(mem.availableKb), pad, y, labelWidth, column); y += step
  field('used', used ? bytes(used) : null, pad, y, labelWidth, column); y += step
  field('free', bytes(mem.freeKb), pad, y, labelWidth, column); y += step
  field('buffers/cache', bytes((mem.buffersKb || 0) + (mem.cachedKb || 0)), pad, y, labelWidth, column); y += step
  field('swap used', mem.swapTotalKb ? bytes(mem.swapTotalKb - (mem.swapFreeKb || 0)) + ' of ' + bytes(mem.swapTotalKb) : null,
        pad, y, labelWidth, column); y += Math.round(step * 0.8)
  if (mem.totalKb) {
    const fraction = used / mem.totalKb
    bar(pad, y, column, Math.round(step * 0.45), fraction, fraction > 0.85 ? COLORS.warn : COLORS.bar)
  }

  // Right column: GPU and the runtime.
  const right = pad * 2 + column
  y = pad + step * 4
  line('GPU', right, y, COLORS.head, 20, 'bold')
  y += step
  field('renderer', gpu.renderer, right, y, labelWidth, column); y += step
  field('vendor', gpu.vendor, right, y, labelWidth, column); y += step
  field('GL version', gpu.version, right, y, labelWidth, column); y += step
  field('GLSL', gpu.shading, right, y, labelWidth, column); y += step
  field('WebGL', gpu.webgl2 + ' (' + gpu.extensions + ' extensions)', right, y, labelWidth, column); y += step
  field('max texture', gpu.maxTexture + ' px', right, y, labelWidth, column); y += step
  field('attribs / units', gpu.maxAttributes + ' / ' + gpu.maxUnits, right, y, labelWidth, column); y += step
  field('video memory', (sample && sample.gpu && sample.gpu.memory) || null, right, y, labelWidth, column); y += step * 2

  line('RUNTIME', right, y, COLORS.head, 20, 'bold')
  y += step
  field('drawable', width + ' × ' + height, right, y, labelWidth, column); y += step
  field('screen', window.innerWidth + ' × ' + window.innerHeight, right, y, labelWidth, column); y += step
  field('JS engine', host.engine || 'Hermes', right, y, labelWidth, column); y += step
  field('kernel', host.kernel, right, y, labelWidth, column); y += step
  field('uptime', host.uptime, right, y, labelWidth, column); y += step
  field('sampled', sample && sample.sampledAt ? sample.sampledAt : null, right, y, labelWidth, column); y += step

  if (sampleError) {
    line(sampleError.length > 96 ? sampleError.slice(0, 96) + '…' : sampleError, pad, height - pad,
         COLORS.warn, 14)
  }
}

// ---- the loop -----------------------------------------------------------------------

function resize() {
  const width = gl.drawingBufferWidth
  const height = gl.drawingBufferHeight
  if (panel.width === width && panel.height === height) return
  panel.width = width
  panel.height = height
  gl.viewport(0, 0, width, height)
}

function present() {
  gl.bindTexture(gl.TEXTURE_2D, texture)
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, panel)
  gl.clearColor(0, 0, 0, 1)
  gl.clear(gl.COLOR_BUFFER_BIT)
  gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4)
}

async function tick() {
  resize()
  await readSample()
  paint()
  present()
  setTimeout(tick, REPAINT_MS)
}

tick()
