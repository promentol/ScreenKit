// The pinned hermesc: how it is found, how it is run, and how its output is
// read back.
//
// Found through tools/prebuilts/fetch.mjs --hermesc, the same resolver CMake
// uses, so an app and the runtime that loads it are compiled by one hermesc.

import { execFileSync, spawnSync } from 'node:child_process'
import { closeSync, existsSync, openSync, readSync } from 'node:fs'
import { fileURLToPath } from 'node:url'

import { BundleError } from './errors.js'

export const REPO_ROOT = fileURLToPath(new URL('../../../../', import.meta.url))
const FETCH = fileURLToPath(new URL('../../../../tools/prebuilts/fetch.mjs', import.meta.url))

// -Xes6-block-scoping: a bare `hermesc -emit-binary` leaves ES6 block scoping
// off, and the result loads, renders a first frame, then throws "undefined is
// not a function" on the first reactive update, because every closure made in a
// `for (let ...)` shares one binding. -w: warnings about the app's own code are
// not the packager's to report.
export const COMPILE_FLAGS = Object.freeze(['-emit-binary', '-O', '-Xes6-block-scoping', '-w'])

// An HBC file opens with an 8-byte magic followed by a little-endian uint32
// bytecode version -- the same header BytecodeLoader.cpp reads.
const HBC_MAGIC = Buffer.from([0xc6, 0x1f, 0xbc, 0x03, 0xc1, 0x03, 0x19, 0x1f])

// An AST dump of a large chunk runs to tens of megabytes of JSON.
const AST_MAX_BUFFER = 1024 * 1024 * 1024

export function pinnedHermesc() {
  let out
  try {
    out = execFileSync(process.execPath, [FETCH, '--hermesc'], {
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'pipe'],
    })
  } catch (err) {
    const why = String(err.stderr || err.message).trim().split('\n').slice(-3).join('\n  ')
    throw new BundleError(`could not resolve the pinned hermesc (node tools/prebuilts/fetch.mjs --hermesc):\n  ${why}`)
  }
  const bin = out.trim().split('\n').pop()
  if (!bin || !existsSync(bin)) {
    throw new BundleError(`tools/prebuilts/fetch.mjs --hermesc named "${bin}", which does not exist`)
  }
  return bin
}

/**
 * Parse one file into hermesc's ESTree-shaped AST, with byte ranges. hermesc
 * is the parser because it is the compiler: whatever it cannot parse cannot
 * ship, and nothing else has to agree with it.
 */
export function dumpAst(hermesc, cwd, file) {
  const run = spawnSync(hermesc, ['-dump-ast', '-pretty=false', '-dump-source-location=range', file], {
    cwd,
    maxBuffer: AST_MAX_BUFFER,
  })
  if (run.error) throw new BundleError(`hermesc could not parse ${file}: ${run.error.message}`)
  if (run.status !== 0) {
    throw new BundleError(`hermesc rejected ${file}:\n${firstDiagnostic(run.stderr.toString())}`)
  }
  try {
    return JSON.parse(run.stdout.toString())
  } catch (err) {
    throw new BundleError(`hermesc's AST for ${file} is not readable JSON: ${err.message}`)
  }
}

/**
 * Compile `input` to `output`, both relative to `cwd`. Relative on purpose: the
 * name hermesc is given is the name stack traces show.
 *
 * @param {(line: number) => { file: string, line: number, text: string } | null} [locate]
 *   maps a line of `input` back to the file it was packed from, so the
 *   diagnostic names that file rather than the packed script.
 */
export function compile(hermesc, cwd, input, output, locate) {
  const run = spawnSync(hermesc, [...COMPILE_FLAGS, '-out', output, input], { cwd })
  if (run.error) throw new BundleError(`hermesc did not run: ${run.error.message}`)
  if (run.status !== 0) {
    throw new BundleError(`hermesc rejected the packed bundle:\n${firstDiagnostic(run.stderr.toString(), locate)}`)
  }
}

/** The bytecode version in an .hbc header -- read, never assumed. */
export function readBytecodeVersion(path) {
  const header = Buffer.alloc(HBC_MAGIC.length + 4)
  const fd = openSync(path, 'r')
  let read
  try {
    read = readSync(fd, header, 0, header.length, 0)
  } finally {
    closeSync(fd)
  }
  if (read < header.length || !header.subarray(0, HBC_MAGIC.length).equals(HBC_MAGIC)) {
    throw new BundleError(`${path} is not Hermes bytecode: no HBC header`)
  }
  return header.readUInt32LE(HBC_MAGIC.length)
}

// `file:line:col: error: message`, then usually the source line and a caret.
const DIAGNOSTIC = /^(.+?):(\d+):(\d+): (error|warning|note): (.*)$/
const WIDTH = 160

/**
 * hermesc's first error. Only the first: the rest are usually consequences of
 * it. When `locate` can say which packed file the line came from, the error is
 * reported against that file with an excerpt of it -- hermesc's own echo of a
 * minified line is the whole rest of a 100 KB line, with no caret.
 */
export function firstDiagnostic(stderr, locate) {
  const lines = stderr.split('\n')
  const at = lines.findIndex((l) => DIAGNOSTIC.exec(l)?.[4] === 'error')
  if (at === -1) {
    const text = stderr.trim()
    return `  ${text ? text.split('\n').slice(0, 4).join('\n  ') : '(hermesc exited without a diagnostic)'}`
  }
  const [, , line, column, , message] = DIAGNOSTIC.exec(lines[at])
  const origin = locate?.(Number(line))
  if (origin) {
    return [`${origin.file}:${origin.line}:${column}: error: ${message}`, ...excerpt(origin.text, Number(column))]
      .map((l) => `  ${l}`)
      .join('\n')
  }
  const out = [lines[at]]
  for (const next of lines.slice(at + 1)) {
    if (DIAGNOSTIC.test(next) || /^Emitted \d+ errors?/.test(next) || next === '') break
    out.push(next)
  }
  return out.map((l) => `  ${l.length > WIDTH ? `${l.slice(0, WIDTH)}...` : l}`).join('\n')
}

/** A window of `text` around a 1-based column, and a caret under it. */
function excerpt(text, column) {
  const from = Math.max(0, column - 1 - WIDTH / 2)
  const to = Math.min(text.length, from + WIDTH)
  const head = from > 0 ? '...' : ''
  const tail = to < text.length ? '...' : ''
  return [`${head}${text.slice(from, to)}${tail}`, `${' '.repeat(head.length + column - 1 - from)}^`]
}
