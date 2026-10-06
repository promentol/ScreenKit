// The Hermes polyfills, end to end. A real Vite build through screenkit() --
// plugin-legacy, core-js, and the additionalLegacyPolyfills it is handed from
// hermes-builtins.json -- packed by `screenkit bundle` and run on the host. The
// app is the built-ins probe itself (runtime/tests/fixtures/hermes-builtins.js),
// so what it reports missing is what an app on this runtime actually lacks: it
// must be exactly the entries hermes-builtins.json says cannot be polyfilled.
//
// plugin.test.js checks the options handed to a fake plugin-legacy and the
// hermes-builtins ctest row probes a bare engine; a wrong core-js module name,
// or a plugin-legacy release that stops honouring additionalLegacyPolyfills,
// passed both. This does not.
//
// The package runs on `screenkit-host --window`, as a web build does: the
// headless path has no DOM prelude, and SystemJS reads `self`. A windowed host
// runs until it is closed, so the test waits for the probe's line and then asks
// it to quit.
//
// Needs SCREENKIT_HOST (the cli-bundle ctest row sets it) and a Vite with
// plugin-legacy -- taken from examples/blits-example-app, installed there. Skipped
// without either, or when the host cannot open a window (exit 71).

import assert from 'node:assert/strict'
import { spawn, spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { after, test } from 'node:test'
import { fileURLToPath, pathToFileURL } from 'node:url'

import screenkit from '../../vite-plugin/src/index.js'

const BIN = fileURLToPath(new URL('../bin/screenkit.js', import.meta.url))
const REPO = fileURLToPath(new URL('../../../../', import.meta.url))
const HOST = process.env.SCREENKIT_HOST
const VITE_APP = join(REPO, 'poc', 'blits-example-app')
const VITE = join(VITE_APP, 'node_modules', 'vite', 'dist', 'node', 'index.js')

const scratch = mkdtempSync(join(tmpdir(), 'screenkit-polyfills-test-'))
after(() => rmSync(scratch, { recursive: true, force: true }))

const skip = !HOST
  ? 'SCREENKIT_HOST is not set'
  : !existsSync(VITE) || !existsSync(join(VITE_APP, 'node_modules', '@vitejs', 'plugin-legacy'))
    ? `no Vite with plugin-legacy in ${VITE_APP} (npm install there)`
    : false

/** Run `host --window pkg` until a line containing `needle` appears; then quit it. */
function runWindowedUntil(host, pkg, needle, timeoutMs = 60_000) {
  return new Promise((resolve) => {
    const child = spawn(host, ['--window', pkg], { stdio: ['ignore', 'pipe', 'pipe'] })
    let output = ''
    let line
    const onData = (chunk) => {
      output += chunk
      line ??= output.split('\n').find((l) => l.includes(needle))
      if (line) child.kill('SIGTERM')
    }
    child.stdout.on('data', onData)
    child.stderr.on('data', onData)
    const timer = setTimeout(() => child.kill('SIGKILL'), timeoutMs)
    child.on('close', (status) => {
      clearTimeout(timer)
      resolve({ status, output, line })
    })
  })
}

test('Hermes polyfills: a screenkit() build leaves only the gaps hermes-builtins.json calls unpolyfillable', { skip }, async (t) => {
  const app = join(scratch, 'probe-app')
  const probe = readFileSync(join(REPO, 'runtime', 'tests', 'fixtures', 'hermes-builtins.js'), 'utf8')
  mkdirSync(app, { recursive: true })
  writeFileSync(join(app, 'index.html'), '<!doctype html><script type="module" src="/main.js"></script>\n')
  writeFileSync(join(app, 'main.js'), `${probe}\nconsole.log('hermes-builtins missing: ' + JSON.stringify(missing.slice().sort()));\n`)

  const { build } = await import(pathToFileURL(VITE).href)
  await build({ root: app, configFile: false, logLevel: 'error', plugins: [screenkit({ root: VITE_APP })] })

  const bundled = spawnSync(process.execPath, [BIN, 'bundle'], { cwd: app, encoding: 'utf8' })
  assert.equal(bundled.status, 0, `${bundled.stdout}${bundled.stderr}`)

  const { status, output, line } = await runWindowedUntil(HOST, join(app, 'app.skpkg'), 'hermes-builtins missing: ')
  if (status === 71) {
    t.skip('the host could not open a window or GL context (exit 71)')
    return
  }
  assert.ok(line, `the probe never reported (host exit ${status}):\n${output}`)
  assert.equal(status, 0, output)
  const missing = JSON.parse(line.slice(line.indexOf('hermes-builtins missing: ') + 'hermes-builtins missing: '.length))

  const builtins = JSON.parse(readFileSync(join(REPO, 'packages', '@screenkit', 'vite-plugin', 'src', 'hermes-builtins.json'), 'utf8'))
  const unpolyfillable = builtins.missing.filter((gap) => !gap.polyfill).map((gap) => gap.builtin).sort()
  assert.deepEqual(missing, unpolyfillable)
})
