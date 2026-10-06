// @screenkit/vite-plugin: plugin order, the outDir default, the Hermes polyfills,
// and the Lightning transform -- it rewrites its anchor, fails the build when
// the anchor is gone, and fails it again at buildEnd when its package was built
// but its target module never was. Plain hook calls with a stand-in plugin
// context; no Vite needed.

import assert from 'node:assert/strict'
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { after, describe, test } from 'node:test'

import screenkit, {
  BLITS_CONTAIN_FIT,
  blitsFitsTheDrawable,
  DEFAULT_OUT_DIR,
  HERMES_BUILTINS,
  HERMES_POLYFILLS,
  hermesBitExactQuadCopy,
  legacyRoots,
  lightningOnScreenKit,
  resolveScreenkitShaka,
  SHAKA_IMPORT,
  shakaOnScreenKit,
} from '../src/index.js'

const RENDERER_ID = '/app/node_modules/@lightningjs/renderer/dist/src/core/renderers/webgl/WebGlRenderer.js'
const RENDERER_OTHER_ID = '/app/node_modules/@lightningjs/renderer/dist/src/main-api/Renderer.js'
const BLITS_ID = '/app/node_modules/@lightningjs/blits/src/engines/L3/launch.js'
const BLITS_OTHER_ID = '/app/node_modules/@lightningjs/blits/src/launch.js'

// Blits' fallback chain, verbatim, indented as it is in the module.
const BLITS_RATIO = `        deviceLogicalPixelRatio:
          settings.pixelRatio ||
          SCREEN_RESOLUTIONS[settings.screenResolution] ||
          SCREEN_RESOLUTIONS[screenHeight] ||
          1,
`

// Rollup's this.error throws; so does this.
function context() {
  return {
    error(message) {
      throw new Error(message)
    },
  }
}

// A root with no @vitejs/plugin-legacy, so the lazy legacy entry rejects; the
// tests that only look at the synchronous plugins swallow that.
const emptyRoot = mkdtempSync(join(tmpdir(), 'screenkit-plugin-test-'))
after(() => rmSync(emptyRoot, { recursive: true, force: true }))

function plugins() {
  const list = screenkit({ root: emptyRoot })
  for (const p of list) if (typeof p?.then === 'function') p.catch(() => {})
  return list
}

describe('screenkit()', () => {
  test('puts the Lightning transform first, ahead of the app\'s own plugins', () => {
    const list = plugins()
    assert.deepEqual(list.slice(0, 1).map((p) => p.name), ['screenkit-hermes-bit-exact-quad-copy'])
    assert.equal(list[0].enforce, 'pre')
  })

  test('builds the fixes fresh for every build, so one build\'s bookkeeping cannot excuse another\'s', () => {
    const first = plugins()
    const second = plugins()
    assert.notEqual(first[0], second[0])
    // The first build transformed WebGlRenderer.js; the second saw the renderer but not it.
    first[0].transform.call(context(), 'for (let j = 0; j < 20; j++) {\n  scratch[j] = f[slot + j];\n}\n', RENDERER_ID)
    assert.doesNotThrow(() => first[0].buildEnd.call(context()))
    second[0].transform.call(context(), 'export default {}', RENDERER_OTHER_ID)
    assert.throws(() => second[0].buildEnd.call(context()), /screenkit-hermes-bit-exact-quad-copy/)
  })

  test('forces the output shape screenkit bundle reads: outDir, emptyOutDir and a root base', () => {
    const defaults = plugins().find((p) => p?.name === 'screenkit:build-defaults')
    const forced = { base: '/', build: { outDir: 'dist-screenkit', emptyOutDir: true } }
    assert.equal(DEFAULT_OUT_DIR, 'dist-screenkit')
    const notes = captureWarnings(() => {
      assert.deepEqual(defaults.config({}), forced)
      assert.deepEqual(defaults.config({ build: {} }), forced)
    })
    assert.deepEqual(notes, [])
  })

  test('overrides what a merged app config says, and says so', () => {
    const defaults = plugins().find((p) => p?.name === 'screenkit:build-defaults')
    let result
    const notes = captureWarnings(() => {
      result = defaults.config({ base: '/legacy/chrome-71/', build: { outDir: 'dist/legacy/chrome-71/', emptyOutDir: false } })
    })
    assert.deepEqual(result, { base: '/', build: { outDir: 'dist-screenkit', emptyOutDir: true } })
    assert.equal(notes.length, 3)
    assert.match(notes[0], /base "\/legacy\/chrome-71\/" replaced by "\/"/)
    assert.match(notes[1], /build\.outDir "dist\/legacy\/chrome-71\/" replaced by "dist-screenkit"/)
    assert.match(notes[2], /emptyOutDir false replaced by true/)
  })

  test('screenkit({ outDir }) chooses the output directory', () => {
    const list = screenkit({ root: emptyRoot, outDir: 'tv-build' })
    for (const p of list) if (typeof p?.then === 'function') p.catch(() => {})
    const defaults = list.find((p) => p?.name === 'screenkit:build-defaults')
    const notes = captureWarnings(() => {
      assert.deepEqual(defaults.config({ build: { outDir: 'tv-build' } }), { base: '/', build: { outDir: 'tv-build', emptyOutDir: true } })
    })
    assert.deepEqual(notes, [])
  })

  test('refuses a build with plugin-legacy added twice', () => {
    const defaults = plugins().find((p) => p?.name === 'screenkit:build-defaults')
    const legacy = [{ name: 'vite:legacy-config' }, { name: 'vite:legacy-post-process' }]
    assert.doesNotThrow(() => defaults.configResolved({ plugins: legacy }))
    assert.throws(
      () => defaults.configResolved({ plugins: [...legacy, ...legacy] }),
      /plugin-legacy is in this build 2 times/,
    )
  })
})

describe('where plugin-legacy is resolved from', () => {
  test('root, then the package npm runs a script for, then the working directory -- without repeats', () => {
    assert.deepEqual(legacyRoots('/app', { npm_package_json: '/npm-app/package.json' }, '/cwd'), ['/app', '/npm-app', '/cwd'])
    assert.deepEqual(legacyRoots(undefined, { npm_package_json: '/npm-app/package.json' }, '/cwd'), ['/npm-app', '/cwd'])
    assert.deepEqual(legacyRoots(undefined, {}, '/cwd'), ['/cwd'])
    assert.deepEqual(legacyRoots(undefined, { npm_package_json: '/cwd/package.json' }, '/cwd'), ['/cwd'])
  })

  test('the npm package is used when the working directory has no plugin-legacy', async () => {
    const app = mkdtempSync(join(tmpdir(), 'screenkit-plugin-app-'))
    after(() => rmSync(app, { recursive: true, force: true }))
    writeFileSync(join(app, 'package.json'), '{"name":"app"}')
    const pkg = join(app, 'node_modules', '@vitejs', 'plugin-legacy')
    mkdirSync(pkg, { recursive: true })
    writeFileSync(join(pkg, 'package.json'), '{"name":"@vitejs/plugin-legacy","main":"index.cjs"}')
    writeFileSync(join(pkg, 'index.cjs'), 'module.exports = (options) => ({ name: "fake-legacy", options })')

    const saved = process.env.npm_package_json
    process.env.npm_package_json = join(app, 'package.json')
    try {
      // The working directory (this package) has no plugin-legacy of its own.
      const legacy = await screenkit().find((p) => typeof p?.then === 'function')
      assert.equal(legacy.name, 'fake-legacy')
      assert.deepEqual(legacy.options, {
        targets: ['defaults', 'not IE 11'],
        renderModernChunks: false,
        polyfills: true,
        additionalLegacyPolyfills: [...HERMES_POLYFILLS],
      })
    } finally {
      if (saved === undefined) delete process.env.npm_package_json
      else process.env.npm_package_json = saved
    }
  })

  test('not installed anywhere: the error names every place it looked', async () => {
    const saved = process.env.npm_package_json
    const savedCwd = process.cwd()
    // The last root `legacyRoots` adds is the working directory, and this suite
    // runs inside an installed workspace where plugin-legacy is hoisted above
    // it -- so "nowhere" has to be arranged, not assumed. Both named roots are
    // empty directories outside the repo.
    const emptyCwd = mkdtempSync(join(tmpdir(), 'screenkit-legacy-cwd-'))
    delete process.env.npm_package_json
    try {
      process.chdir(emptyCwd)
      const pending = screenkit({ root: emptyRoot }).find((p) => typeof p?.then === 'function')
      await assert.rejects(pending, (err) =>
        err.message.includes(`not installed in ${emptyRoot}, or `) && err.message.includes(emptyCwd),
      )
    } finally {
      process.chdir(savedCwd)
      rmSync(emptyCwd, { recursive: true, force: true })
      if (saved !== undefined) process.env.npm_package_json = saved
    }
  })
})

function captureWarnings(run) {
  const notes = []
  const original = console.warn
  console.warn = (message) => notes.push(String(message))
  try {
    run()
  } finally {
    console.warn = original
  }
  return notes
}

describe('hermesBitExactQuadCopy', () => {
  test('replaces the element-wise quad copy in WebGlRenderer.js with TypedArray.set', () => {
    const code = 'for (let j = 0; j < 20; j++) {\n  scratch[j] = f[slot + j];\n}\n'
    const out = hermesBitExactQuadCopy().transform.call(context(), code, RENDERER_ID)
    assert.equal(out.code, 'scratch.set(f.subarray(slot, slot + 20));\n')
  })

  test('leaves every other module alone', () => {
    assert.equal(hermesBitExactQuadCopy().transform.call(context(), 'scratch[j] = f[slot + j]', '/app/src/App.js'), null)
  })

  test('fails the build when the anchor is gone', () => {
    assert.throws(
      () => hermesBitExactQuadCopy().transform.call(context(), 'scratch.set(f.subarray(slot, slot + 20));', RENDERER_ID),
      /WebGlRenderer\.js changed/,
    )
  })
})

describe('blitsFitsTheDrawable', () => {
  test('replaces only the `|| 1` fallback, leaving the author-set options ahead of it', () => {
    const out = blitsFitsTheDrawable().transform.call(context(), BLITS_RATIO, BLITS_ID)
    assert.match(out.code, /settings\.pixelRatio \|\|/)
    assert.match(out.code, /SCREEN_RESOLUTIONS\[settings\.screenResolution\] \|\|/)
    assert.match(out.code, /SCREEN_RESOLUTIONS\[screenHeight\] \|\|/)
    assert.doesNotMatch(out.code, /\|\|\s*1,/)
  })

  test('leaves every other module alone', () => {
    assert.equal(blitsFitsTheDrawable().transform.call(context(), BLITS_RATIO, '/app/src/App.js'), null)
  })

  test('fails the build when the anchor is gone', () => {
    assert.throws(
      () => blitsFitsTheDrawable().transform.call(context(), 'deviceLogicalPixelRatio: 1,', BLITS_ID),
      /L3\/launch\.js changed/,
    )
  })
})

// The injected expression, evaluated in the scope Blits calls it in.
function containFit(view, settings = {}) {
  const body = 'return ' + BLITS_CONTAIN_FIT.replace(/,$/, '')
  return new Function('settings', 'platform', 'screenHeight', body)(
    settings, { viewport: view }, view && view.innerHeight)
}

describe('the scale a 1080p Blits app gets on a ScreenKit drawable', () => {
  const view = (innerWidth, innerHeight) => ({ innerWidth, innerHeight })

  test('a 16:9 drawable fits on both axes, agreeing with the table Blits already has', () => {
    assert.equal(containFit(view(1920, 1080)), 1)
    assert.ok(Math.abs(containFit(view(1280, 720)) - 0.66666667) < 1e-6)
    assert.equal(containFit(view(3840, 2160)), 2)
  })

  test('a 4:3 drawable is fitted by width, so nothing is cropped', () => {
    // 640/1920 = 0.333 fits; the height ratio 480/1080 = 0.444 would draw 853 wide.
    assert.ok(Math.abs(containFit(view(640, 480)) - 640 / 1920) < 1e-9)
    assert.ok(containFit(view(640, 480)) * 1920 <= 640)
  })

  test("an app that is not 1080p is measured against its own stage", () => {
    assert.equal(containFit(view(1280, 720), { w: 1280, h: 720 }), 1)
  })

  test('no viewport, or a zero-sized one, keeps the old behaviour', () => {
    assert.equal(containFit(undefined), 1)
    assert.equal(containFit(view(0, 0)), 1)
  })
})

// The shape of the module the fix rewrites, abridged to its anchor.
const TARGETS = [
  { make: hermesBitExactQuadCopy, id: RENDERER_ID, other: RENDERER_OTHER_ID,
    code: 'for (let j = 0; j < 20; j++) {\n  scratch[j] = f[slot + j];\n}\n',
    pkg: '@lightningjs/renderer', target: '@lightningjs/renderer/dist/src/core/renderers/webgl/WebGlRenderer.js' },
  { make: blitsFitsTheDrawable, id: BLITS_ID, other: BLITS_OTHER_ID, code: BLITS_RATIO,
    pkg: '@lightningjs/blits', target: '@lightningjs/blits/src/engines/L3/launch.js' },
]

describe('a fix whose target module moved fails the build at buildEnd', () => {
  for (const { make, id, other, code, pkg, target } of TARGETS) {
    const name = make().name

    test(`${name}: fires when ${pkg} was built but its target never was, naming the fix and the path`, () => {
      const fix = make()
      assert.equal(fix.transform.call(context(), 'export default 1', other), null)
      // Where the module went: same file, new directory. Not matched.
      assert.equal(fix.transform.call(context(), code, id.replace('/src/', '/lib/').replace('/dist/src/', '/dist/lib/')), null)
      assert.throws(
        () => fix.buildEnd.call(context()),
        (err) => err.message.includes(name) && err.message.includes(`${target} was never transformed`),
      )
    })

    test(`${name}: does not fire when the target was transformed`, () => {
      const fix = make()
      fix.transform.call(context(), 'export default 1', other)
      assert.notEqual(fix.transform.call(context(), code, id), null)
      assert.doesNotThrow(() => fix.buildEnd.call(context()))
    })

    test(`${name}: does not bury a build that already failed`, () => {
      const fix = make()
      fix.transform.call(context(), 'export default 1', other)
      assert.doesNotThrow(() => fix.buildEnd.call(context(), new Error('an earlier failure')))
    })
  }

  test('an app without Lightning builds: every fix is inert', () => {
    for (const fix of lightningOnScreenKit()) {
      for (const moduleId of ['/app/src/main.js', '/app/node_modules/lodash/index.js', '\0vite/legacy-polyfills']) {
        assert.equal(fix.transform.call(context(), 'export default 1', moduleId), null)
      }
      assert.doesNotThrow(() => fix.buildEnd.call(context()))
    }
  })

  test('a Windows path and a query string still match', () => {
    const fix = hermesBitExactQuadCopy()
    const windowsId = 'C:\\app\\node_modules\\@lightningjs\\renderer\\dist\\src\\core\\renderers\\webgl\\WebGlRenderer.js?v=1'
    assert.notEqual(fix.transform.call(context(), 'for (let j = 0; j < 20; j++) {\n  scratch[j] = f[slot + j];\n}\n', windowsId), null)
    assert.doesNotThrow(() => fix.buildEnd.call(context()))
  })
})

describe('the ES built-ins Hermes lacks', () => {
  test('every gap has a core-js polyfill or says why it cannot have one', () => {
    assert.ok(HERMES_BUILTINS.missing.length > 0)
    for (const gap of HERMES_BUILTINS.missing) {
      assert.match(gap.builtin, /\S/)
      assert.match(gap.edition, /^ES20(1[5-9]|2[0-3])( Annex B)?$/, gap.builtin)
      if (gap.polyfill === null) assert.ok(gap.why && gap.why.length > 20, `${gap.builtin} needs a why`)
      else assert.match(gap.polyfill, /^es\.[a-z-]+\.[a-z-]+$/, gap.builtin)
    }
  })

  test('each polyfill goes into the legacy polyfills bundle as a core-js module', () => {
    const expected = HERMES_BUILTINS.missing.filter((gap) => gap.polyfill).map((gap) => `core-js/modules/${gap.polyfill}.js`)
    assert.deepEqual([...HERMES_POLYFILLS], expected)
    assert.ok(HERMES_POLYFILLS.includes('core-js/modules/es.array.to-sorted.js'))
  })
})

describe('shaka-player resolves to @screenkit/shaka', () => {
  const sibling = new URL('../../shaka/src/index.js', import.meta.url).pathname

  test('is in every screenkit() build, after the Lightning fixes', () => {
    const list = plugins()
    const at = list.findIndex((p) => p?.name === 'screenkit:shaka-player')
    assert.ok(at > 0)
    assert.equal(list[at].enforce, 'pre')
    assert.deepEqual(list[at].config(), { optimizeDeps: { exclude: ['shaka-player'] } })
  })

  test('takes the package and every subpath, and nothing else', () => {
    const plugin = shakaOnScreenKit(() => [emptyRoot])
    for (const id of ['shaka-player', 'shaka-player/dist/shaka-player.compiled.js',
                      'shaka-player/dist/shaka-player.ui.js', 'shaka-player/dist/shaka-player.compiled.debug.js']) {
      assert.equal(plugin.resolveId.call(context(), id), sibling, id)
    }
    for (const id of ['shaka-players', 'my-shaka-player', 'shaka', './shaka-player', '@screenkit/shaka']) {
      assert.equal(plugin.resolveId.call(context(), id), null, id)
    }
    assert.ok(SHAKA_IMPORT.test('shaka-player/ui'))
  })

  test('a stylesheet subpath is an empty module: there is no DOM to style', () => {
    const plugin = shakaOnScreenKit(() => [emptyRoot])
    const id = plugin.resolveId.call(context(), 'shaka-player/dist/controls.css')
    assert.notEqual(id, sibling)
    assert.equal(plugin.load(id), 'export default ""')
    assert.equal(plugin.load(sibling), null)
  })

  test('the app\'s own @screenkit/shaka wins over the monorepo sibling', () => {
    const app = mkdtempSync(join(tmpdir(), 'screenkit-shaka-app-'))
    try {
      const pkg = join(app, 'node_modules', '@screenkit', 'shaka')
      mkdirSync(join(pkg, 'src'), { recursive: true })
      writeFileSync(join(pkg, 'package.json'), JSON.stringify({ name: '@screenkit/shaka', main: './src/index.js' }))
      writeFileSync(join(pkg, 'src', 'index.js'), 'export default {}')
      writeFileSync(join(app, 'package.json'), '{}')
      const found = resolveScreenkitShaka([app])
      assert.ok(found.startsWith(pkg) || found.includes('node_modules/@screenkit/shaka'), found)
      assert.equal(resolveScreenkitShaka([emptyRoot]), sibling)
      // Installed in this workspace, the plugin's own node_modules answers
      // before the sibling fallback is ever reached -- and answers with the
      // same file, since npm links the workspace package rather than copying
      // it. So disabling the fallback changes nothing here. The case where
      // nothing is found anywhere is the next test, which injects a finder.
      assert.equal(resolveScreenkitShaka([emptyRoot], null), sibling)
    } finally {
      rmSync(app, { recursive: true, force: true })
    }
  })

  test('fails the build, naming the package, when there is no @screenkit/shaka at all', () => {
    const plugin = shakaOnScreenKit(() => [emptyRoot], () => null)
    assert.throws(() => plugin.resolveId.call(context(), 'shaka-player'), /@screenkit\/shaka is not installed/)
  })

  test('the entry it resolves to is Shaka-shaped', async () => {
    const shaka = (await import(sibling)).default
    assert.equal(typeof shaka.Player, 'function')
    assert.equal(typeof shaka.polyfill.installAll, 'function')
    assert.equal(shaka.util.Error.Code.LOAD_INTERRUPTED, 7000)
    assert.equal(shaka.net.NetworkingEngine.RequestType.LICENSE, 2)
  })
})
