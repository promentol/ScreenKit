#!/usr/bin/env node
// screenkit -- the ScreenKit command line.
//
//   screenkit bundle [dist] [--out app.skpkg] [--spidermonkey[=source|lazy|eager]]
//
// Only `bundle` exists today; Architecture.md 11 lists run, deploy and doctor as
// later work.

import { BundleError, DEFAULT_DIST, DEFAULT_OUT, SPIDERMONKEY_MODES, bundle } from '../src/bundle.js'

const USAGE = `usage: screenkit bundle [dist] [--out <package>] [--spidermonkey[=<mode>]]

  Turn a finished ScreenKit Vite build into a .skpkg package directory.

  dist                  the build output (default: ${DEFAULT_DIST})
  --out <dir>           where to write the package (default: ${DEFAULT_OUT})
  --spidermonkey[=m]    also carry what a SpiderMonkey host runs: source (app.js), lazy
                        (+ app.stencil, the default) or eager (+ app.stencil with every
                        function compiled). The stencil is compiled in Docker.
                        SCREENKIT_SPIDERMONKEY=<mode> does the same from an npm script.`

function usage(message) {
  if (message) console.error(`screenkit: ${message}`)
  console.error(USAGE)
  process.exit(2)
}

const [command, ...args] = process.argv.slice(2)
if (command === '--help' || command === '-h' || command === 'help') {
  console.log(USAGE)
  process.exit(0)
}
if (command !== 'bundle') usage(command ? `unknown command "${command}"` : undefined)

let dist
let out
let spidermonkey = process.env.SCREENKIT_SPIDERMONKEY || ''
for (let i = 0; i < args.length; i++) {
  const arg = args[i]
  if (arg === '--help' || arg === '-h') {
    console.log(USAGE)
    process.exit(0)
  } else if (arg === '--out') {
    out = args[++i]
    if (!out) usage('--out needs a path')
  } else if (arg.startsWith('--out=')) {
    out = arg.slice('--out='.length)
    if (!out) usage('--out needs a path')
  } else if (arg === '--spidermonkey') {
    spidermonkey = 'lazy'
  } else if (arg.startsWith('--spidermonkey=')) {
    spidermonkey = arg.slice('--spidermonkey='.length)
    if (!SPIDERMONKEY_MODES.includes(spidermonkey)) usage(`--spidermonkey takes ${SPIDERMONKEY_MODES.join(', ')}`)
  } else if (arg.startsWith('-')) {
    usage(`unknown option "${arg}"`)
  } else if (dist === undefined) {
    dist = arg
  } else {
    usage(`unexpected argument "${arg}"`)
  }
}

try {
  bundle({ dist, out, spidermonkey, log: (line) => console.log(line) })
} catch (err) {
  if (!(err instanceof BundleError)) throw err
  console.error(`screenkit bundle: ${err.message}`)
  process.exit(1)
}
