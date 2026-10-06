#!/usr/bin/env node
// The engine bench's reports side by side: startup and memory, frame times per scene and load,
// peak memory per scene, and GC; then SpiderMonkey from source against its stencils. Writes
// <dir>/summary.md and prints it.
//
//   node compare.mjs [results dir]
import { readFileSync, readdirSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'

const dir = process.argv[2] || new URL('./results', import.meta.url).pathname
const ORDER = ['hermes-interp', 'hermes-jit', 'hermes-jit-force', ...['sm-interp', 'sm-baseline', 'sm-jit'].flatMap((c) => [c, `${c}-lazy`, `${c}-eager`])]
const FORMS = { '': 'source', '-lazy': 'lazy stencil', '-eager': 'eager stencil' }
const stencilRun = (label) => /-(lazy|eager)$/.test(label)
const reports = readdirSync(dir)
  .filter((f) => f.endsWith('.json'))
  .map((f) => JSON.parse(readFileSync(join(dir, f), 'utf8')))
  .sort((a, b) => rank(a.label) - rank(b.label))
if (!reports.length) {
  console.error(`no reports in ${dir}`)
  process.exit(1)
}
function rank(label) {
  const i = ORDER.indexOf(label)
  return i === -1 ? ORDER.length : i
}

const mb = (kb) => (kb == null || kb < 0 ? '–' : (kb / 1024).toFixed(1))
const ms = (v, digits = 1) => (v == null || v < 0 ? '–' : v.toFixed(digits))
const step = (r, prefix) => r.startup.find((s) => s.step.startsWith(prefix))
const table = (header, rows) =>
  [`| ${header.join(' | ')} |`, `|${header.map((_, i) => (i ? '---:' : '---')).join('|')}|`, ...rows.map((r) => `| ${r.join(' | ')} |`)].join('\n')
// The engine tables compare engines and tiers; a stencil changes how a script is loaded, not the
// code that runs, so its runs have a section of their own below -- unless there is nothing else.
const engineReports = reports.some((r) => !stencilRun(r.label)) ? reports.filter((r) => !stencilRun(r.label)) : reports
const labels = engineReports.map((r) => r.label)

const out = []
const first = reports[0]
out.push(`# Hermes vs SpiderMonkey: Phaser-shaped work`)
out.push('')
out.push(`${first.machine.cpu}, ${first.machine.cores} cores, ${mb(first.machine.memTotalKb)} MB, kernel ${first.machine.kernel}. ` +
  `${first.options.frames} frames per scene and load (first ${first.options.warmup} left out of the steady numbers), ` +
  `view ${first.options.view}, ${first.options.pace ? `paced at ${first.options.pace} fps` : 'frames back to back'}.`)
out.push('')
out.push(table(['run', 'engine', 'mode', 'library'], reports.map((r) => [r.label, r.version, r.mode, (r.settings?.library || '').split('/').slice(-2).join('/')])))

// A Pi 3 on a weak supply runs at 600 MHz while cpufreq reports 1.4 GHz; only the firmware says so
// (vcgencmd, recorded at the start and end of each run). Bits 0-3: now; 16-19: since boot.
const firmware = reports.flatMap((r) => [r.machine?.now, r.machineEnd].filter(Boolean).map((m) => ({ label: r.label, ...m })))
const slowed = firmware.filter((m) => (m.throttled >= 0 && (m.throttled & 0xf)) ||
  (m.armClockHz > 0 && first.machine.cpuMaxFreqKHz > 0 && m.armClockHz < first.machine.cpuMaxFreqKHz * 1000 * 0.95))
if (slowed.length) {
  out.push('')
  out.push(`> **Throttled:** the firmware reported under-voltage or throttling, or a clock below the maximum, in ` +
    `${[...new Set(slowed.map((m) => m.label))].join(', ')} ` +
    `(${[...new Set(slowed.map((m) => `0x${Math.max(0, m.throttled).toString(16)} at ${Math.round(m.armClockHz / 1e6)} MHz`))].join('; ')}). ` +
    `Those numbers are not the machine's.`)
} else if (firmware.some((m) => m.throttled >= 0)) {
  out.push('')
  out.push(`Firmware throttle flags now: clear in every run (since boot: ${[...new Set(firmware.map((m) => `0x${m.throttled.toString(16)}`))].join(', ')}).`)
}

out.push('\n## Startup and memory\n')
out.push('RSS in MB. *Anon* is memory the process made (heaps, JIT code); *file* is mapped files (libraries, bytecode).\n')
out.push(table(
  ['', ...labels],
  [
    ['engine up, ms', ...engineReports.map((r) => ms(step(r, 'engine')?.ms))],
    ['load bulk, ms', ...engineReports.map((r) => ms(step(r, 'load bulk')?.ms))],
    ['load workload, ms', ...engineReports.map((r) => ms(step(r, 'load workload')?.ms))],
    ['RSS: process', ...engineReports.map((r) => mb(step(r, 'process')?.memory.rssKb))],
    ['RSS: engine up', ...engineReports.map((r) => mb(step(r, 'engine')?.memory.rssKb))],
    ['RSS: + bulk code', ...engineReports.map((r) => mb(step(r, 'load bulk')?.memory.rssKb))],
    ['RSS: + workload', ...engineReports.map((r) => mb(step(r, 'load workload')?.memory.rssKb))],
    ['RSS peak (VmHWM)', ...engineReports.map((r) => mb(r.end?.memory.hwmKb))],
    ['RSS at end', ...engineReports.map((r) => mb(r.end?.memory.rssKb))],
    ['  PSS at end', ...engineReports.map((r) => mb(r.end?.memory.pssKb))],
    ['  private dirty at end', ...engineReports.map((r) => mb(r.end?.memory.privateDirtyKb))],
    ['RSS after full GC', ...engineReports.map((r) => mb(r.end?.afterGc.memory.rssKb))],
    ['  anon', ...engineReports.map((r) => mb(r.end?.afterGc.memory.rssAnonKb))],
    ['  file', ...engineReports.map((r) => mb(r.end?.afterGc.memory.rssFileKb))],
    ['JS heap after full GC, MB', ...engineReports.map((r) => mb((r.end?.afterGc.heap.usedBytes ?? -1024) / 1024))],
    ['threads', ...engineReports.map((r) => r.end?.memory.threads ?? '–')],
    ['CPU total, s (all threads)', ...engineReports.map((r) => (r.end ? ((r.end.usage.userMs + r.end.usage.sysMs) / 1000).toFixed(1) : '–'))],
    ['wall total, s', ...engineReports.map((r) => (r.wallMs / 1000).toFixed(1))],
  ],
))

const segments = new Map()
for (const r of reports) {
  for (const s of r.segments || []) {
    const key = `${s.scene} ${s.load}`
    if (!segments.has(key)) segments.set(key, { scene: s.scene, load: s.load, by: {} })
    segments.get(key).by[r.label] = s
  }
}
const unit = Object.fromEntries((first.workload?.scenes || []).map((s) => [s.name, s.unit]))
const rows = [...segments.values()]

out.push('\n## Frame time\n')
out.push(`Steady frames, median / 95th percentile in ms; 16.7 ms is a 60 fps frame. The fastest median in each row is **bold**.\n`)
out.push(table(['scene', 'load', ...labels], rows.map((row) => {
  const best = Math.min(...labels.map((l) => row.by[l]?.steady.p50 ?? Infinity))
  return [row.scene, `${row.load} ${unit[row.scene] || ''}`, ...labels.map((l) => {
    const s = row.by[l]
    if (!s) return '–'
    const cell = `${ms(s.steady.p50, 2)} / ${ms(s.steady.p95, 2)}`
    return s.steady.p50 === best ? `**${cell}**` : cell
  })]
})))

out.push(`\n## What fits in a frame\n`)
out.push(`The largest load tried whose 95th-percentile frame stayed within ${ms(first.options.budgetMs)} ms; 0 when even the smallest did not.\n`)
const scenes = [...new Set(rows.map((row) => row.scene))]
out.push(table(['scene', ...labels], scenes.map((scene) => [`${scene} (${unit[scene] || 'load'})`, ...labels.map((l) => {
  const held = rows.filter((row) => row.scene === scene && row.by[l] && row.by[l].steady.p95 <= first.options.budgetMs)
  const loads = rows.filter((row) => row.scene === scene).map((row) => row.load)
  const best = held.length ? Math.max(...held.map((row) => row.load)) : 0
  return best === Math.max(...loads) ? `${best}+` : `${best}`
})])))

out.push('\n## Speed against the Hermes interpreter\n')
out.push('Median frame time of `hermes-interp` divided by each run\'s: above 1 is faster.\n')
const base = labels.includes('hermes-interp') ? 'hermes-interp' : labels[0]
out.push(table(['scene', 'load', ...labels], rows.map((row) => [row.scene, `${row.load}`, ...labels.map((l) => {
  const b = row.by[base]?.steady.p50, v = row.by[l]?.steady.p50
  return b && v ? `${(b / v).toFixed(2)}×` : '–'
})])))

out.push('\n## Memory per scene\n')
out.push('Peak RSS in MB while the scene ran (sampled every frame), and the JS heap at its end.\n')
out.push(table(['scene', 'load', ...labels.map((l) => `${l} RSS`), ...labels.map((l) => `${l} heap`)], rows.map((row) => [
  row.scene, `${row.load}`,
  ...labels.map((l) => mb(row.by[l]?.peakRssKb)),
  ...labels.map((l) => mb((row.by[l]?.heap.usedBytes ?? -1024) / 1024)),
])))

out.push('\n## Garbage collection\n')
out.push('Over all steady and warm-up frames: collections, total pause on the JS thread, the longest pause, and (Hermes) old-generation work on its background thread, in ms.\n')
out.push(table(['', ...labels], [
  ['collections', ...engineReports.map((r) => sum(r, (s) => s.gc.collections).toFixed(0))],
  ['pause total, ms', ...engineReports.map((r) => ms(sum(r, (s) => s.gc.pauseMs)))],
  ['longest pause, ms', ...engineReports.map((r) => ms(Math.max(...r.segments.map((s) => s.gc.maxPauseMs))))],
  ['background, ms', ...engineReports.map((r) => ms(sum(r, (s) => s.gc.backgroundMs)))],
  ['CPU per frame / frame time', ...engineReports.map((r) => (sum(r, (s) => s.cpuMs) / sum(r, (s) => s.wallMs)).toFixed(2))],
]))

// SpiderMonkey from source against the same scripts as stencils, per JIT tier. A stencil saves
// the parse, and an eager one the compiling of each function on its first call; once a function
// has run, the same bytecode runs, so steady frames should not move.
const stencilReports = reports.filter((r) => stencilRun(r.label))
if (stencilReports.length) {
  const tiers = [...new Set(stencilReports.map((r) => r.label.replace(/-(lazy|eager)$/, '')))]
  const cols = tiers.flatMap((tier) => Object.keys(FORMS).map((suffix) => reports.find((r) => r.label === tier + suffix)).filter(Boolean))
  const sourceOf = (r) => reports.find((x) => x.label === r.label.replace(/-(lazy|eager)$/, ''))
  const segs = (r) => r.segments || []
  const firstSeg = (r) => segs(r)[0]
  const total = (r, pick) => segs(r).reduce((t, s) => t + Math.max(0, pick(s) ?? 0), 0)
  const loadStep = (r, name) => step(r, `load ${name}`)
  const afterLoads = (r) => r.startup?.at(-1)?.memory
  const vsSource = (r) => {
    const base = sourceOf(r)
    if (!base || base === r) return '1'
    const ratios = segs(r).map((s) => {
      const b = base.segments?.find((x) => x.scene === s.scene && x.load === s.load)
      return b && s.steady.p50 > 0 ? b.steady.p50 / s.steady.p50 : null
    }).filter(Boolean)
    return ratios.length ? `${Math.exp(ratios.reduce((t, x) => t + Math.log(x), 0) / ratios.length).toFixed(2)}×` : '–'
  }
  const f = firstSeg(cols[0])
  out.push('\n## SpiderMonkey: source against stencils\n')
  out.push('The same scripts from source, and compiled ahead by `screenkit-smc`: a *lazy* stencil compiles a function ' +
    'from the source it carries on its first call, an *eager* one has every function compiled. A stencil is mapped and ' +
    'its bytecode run in place, as in the runtime. Times in ms, RSS in MB.\n')
  out.push(table(['', ...cols.map((r) => r.label)], [
    ['form', ...cols.map((r) => FORMS[r.label.match(/(-lazy|-eager)?$/)[0]])],
    ['load bulk', ...cols.map((r) => ms(loadStep(r, 'bulk')?.ms))],
    ['load workload', ...cols.map((r) => ms(loadStep(r, 'workload')?.ms))],
    ['RSS after loading (anon / file)', ...cols.map((r) => { const m = afterLoads(r); return m ? `${mb(m.rssKb)} (${mb(m.rssAnonKb)} / ${mb(m.rssFileKb)})` : '–' })],
    [`first scene: build + first frame${f ? ` (${f.scene} ${f.load})` : ''}`, ...cols.map((r) => ms(firstSeg(r) && firstSeg(r).enterMs + firstSeg(r).firstFrameMs))],
    ['every scene: build', ...cols.map((r) => ms(total(r, (s) => s.enterMs)))],
    ['every scene: first frame', ...cols.map((r) => ms(total(r, (s) => s.firstFrameMs)))],
    [`every scene: warm-up frames (first ${first.options.warmup})`, ...cols.map((r) => ms(total(r, (s) => s.warmup.mean * s.warmup.count), 0))],
    ['steady median, speed against source (geometric mean)', ...cols.map(vsSource)],
    ['RSS peak (VmHWM)', ...cols.map((r) => mb(r.end?.memory.hwmKb))],
    ['RSS after full GC (anon / file)', ...cols.map((r) => { const m = r.end?.afterGc.memory; return m ? `${mb(m.rssKb)} (${mb(m.rssAnonKb)} / ${mb(m.rssFileKb)})` : '–' })],
    ['time to the first frame of the first scene', ...cols.map((r) => {
      const steps = (r.startup || []).filter((s) => s.step !== 'process').reduce((t, s) => t + s.ms, 0)
      return ms(firstSeg(r) ? steps + firstSeg(r).enterMs + firstSeg(r).firstFrameMs : null, 0)
    })],
  ]))
}

// Every engine ran the same scenes; their state at the end should agree. A difference in the
// last digits can be libm (Math.sin and friends are not required to be exact); a large one is a bug.
const allLabels = reports.map((r) => r.label)
const mismatches = rows.filter((row) => new Set(allLabels.map((l) => row.by[l]?.result?.checksum).filter((c) => c != null)).size > 1)
out.push('\n## Same work?\n')
out.push(mismatches.length
  ? `Checksums differ in ${mismatches.length} of ${rows.length} segments:\n\n` +
    table(['scene', 'load', ...allLabels], mismatches.map((row) => [row.scene, `${row.load}`, ...allLabels.map((l) => `${row.by[l]?.result?.checksum ?? '–'}`)]))
  : `Every run's end state matched in all ${rows.length} segments.`)

const errors = reports.filter((r) => r.error)
if (errors.length) out.push('\n## Errors\n\n' + errors.map((r) => `- ${r.label}: ${r.error.split('\n')[0]}`).join('\n'))

function sum(report, pick) {
  return report.segments.reduce((total, s) => total + Math.max(0, pick(s) ?? 0), 0)
}

const text = out.join('\n') + '\n'
writeFileSync(join(dir, 'summary.md'), text)
console.log(text)
