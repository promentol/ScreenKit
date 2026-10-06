// `screenkit bundle`, one test per bundle row of the M5 I/O matrix
// (_bmad-output/implementation-artifacts/spec-m5-vite-plugin-skpkg-pipeline.md).
//
// Each fixture is a small dist/ tree written out by the test itself, shaped like
// @vitejs/plugin-legacy output: an index.html naming the polyfills bundle and the
// legacy entry, and System.register chunks. They are generated rather than
// checked in so each tree sits next to the assertion it exists for -- and
// because node --test would otherwise run a checked-in chunk as a test file.
//
// Every run goes through the real CLI binary and the pinned hermesc. With
// SCREENKIT_HOST naming a screenkit-host binary (the cli-bundle ctest row sets
// it), the resolvable-import and entry-rejects rows also run the package on that
// host.

import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, symlinkSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { runInNewContext } from 'node:vm'
import { after, describe, test } from 'node:test'
import { fileURLToPath } from 'node:url'

import { packScript, readLegacyLayout } from '../src/pack.js'
import { MAX_RUNTIME_VERSION, parseRuntimeVersion } from '../src/bundle.js'

const BIN = fileURLToPath(new URL('../bin/screenkit.js', import.meta.url))
const REPO = fileURLToPath(new URL('../../../../', import.meta.url))
const RUNTIME_VERSION = Number(readFileSync(join(REPO, 'runtime/VERSION'), 'utf8').trim())
const PINNED_BYTECODE = JSON.parse(readFileSync(join(REPO, 'tools/prebuilts/manifest.json'), 'utf8')).hermes
  .bytecodeVersion
const HOST = process.env.SCREENKIT_HOST

const scratch = mkdtempSync(join(tmpdir(), 'screenkit-bundle-test-'))
after(() => rmSync(scratch, { recursive: true, force: true }))

// ---------------------------------------------------------------------------
// Fixture pieces
// ---------------------------------------------------------------------------

// Enough of SystemJS to boot a packed fixture: register/getRegister, an
// instantiate hook with no loader behind it (so a chunk the packer did not serve
// fails loudly), and context.import for dynamic imports. ES5, because it runs on
// Hermes as well as in node's vm.
const MINI_SYSTEM = `(function (global) {
  var modules = {};
  var last = null;
  function resolve(id, parent) {
    if (id.charAt(0) === '/') return id;
    var base = parent ? parent.slice(0, parent.lastIndexOf('/') + 1) : '/';
    var parts = (base + id).split('/'), out = [];
    for (var i = 0; i < parts.length; i++) {
      if (parts[i] === '..') out.pop(); else if (parts[i] !== '.') out.push(parts[i]);
    }
    return out.join('/');
  }
  var System = {
    register: function (deps, declare) { last = [deps, declare]; },
    getRegister: function () { var r = last; last = null; return r; },
    instantiate: function (url) { return Promise.reject(new Error('fixture System has no loader for ' + url)); },
    import: function (id, parent) {
      var url = resolve(id, parent);
      if (!modules[url]) modules[url] = load(url);
      return modules[url];
    }
  };
  function load(url) {
    var ns = {};
    return Promise.resolve(System.instantiate(url)).then(function (reg) {
      var decl = reg[1](function (name, value) { ns[name] = value; return value; },
                        { import: function (id) { return System.import(id, url); }, meta: { url: url } });
      return Promise.all(reg[0].map(function (d) { return System.import(d, url); })).then(function (deps) {
        (decl.setters || []).forEach(function (set, i) { set(deps[i]); });
        if (decl.execute) decl.execute();
        return ns;
      });
    });
  }
  global.System = System;
})(typeof globalThis !== 'undefined' ? globalThis : this);
`

function legacyIndexHtml(entry = 'index-legacy-A.js') {
  return `<!doctype html>
<html>
  <body>
    <div id="app"></div>
    <script crossorigin id="vite-legacy-polyfill" src="/assets/polyfills-legacy-P.js"></script>
    <script crossorigin id="vite-legacy-entry" data-src="/assets/${entry}">System.import(document.getElementById('vite-legacy-entry').getAttribute('data-src'))</script>
  </body>
</html>
`
}

/** The shape of the blits-example-app build: a static shared chunk, and a lazy page that imports its importer back. */
function lazyAppFiles({ entryBody } = {}) {
  return {
    'index.html': legacyIndexHtml(),
    'assets/polyfills-legacy-P.js': MINI_SYSTEM,
    'assets/index-legacy-A.js': `System.register(['./shared-legacy-C.js'], function (exports, module) {
  'use strict';
  var greet;
  return {
    setters: [function (m) { greet = m.greet; }],
    execute: function () {
      exports('ready', true);
      // A minifier reusing the context's name in an inner scope. Not a dynamic
      // import of this chunk, and must not be read as one.
      function inner(module) { return module.import(somewhere); }
      console.log(greet('index'));
      ${entryBody ?? "module.import(`./Loading-legacy-B.js`).then(function (page) { console.log(page.message); });"}
      // Closures made in a for (let ...) each keep their own i -- only when
      // hermesc compiles with -Xes6-block-scoping. Without it: 3,3,3.
      var perIteration = [];
      for (let i = 0; i < 3; i++) perIteration.push(function () { return i; });
      console.log('block scoping: ' + perIteration.map(function (f) { return f(); }).join(','));
    }
  };
});
//# sourceMappingURL=index-legacy-A.js.map
`,
    'assets/index-legacy-A.js.map': '{"version":3,"sources":[],"mappings":""}',
    'assets/Loading-legacy-B.js': `System.register(['./index-legacy-A.js'], function (exports) {
  'use strict';
  var index;
  return {
    setters: [function (m) { index = m; }],
    execute: function () { exports('message', 'lazy chunk loaded, index ready: ' + index.ready); }
  };
});
`,
    'assets/shared-legacy-C.js': `System.register([], function (exports) {
  'use strict';
  return { execute: function () { exports('greet', function (who) { return 'hello from ' + who; }); } };
});
`,
    'assets/logo.png': Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 1, 2, 3]),
    'fonts/probe.msdf.json': '{"font":"probe"}\n',
  }
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

let fixtureCount = 0
function makeApp(files) {
  const app = join(scratch, `app-${++fixtureCount}`)
  for (const [rel, content] of Object.entries(files)) {
    const path = join(app, 'dist-screenkit', rel)
    mkdirSync(dirname(path), { recursive: true })
    writeFileSync(path, content)
  }
  return app
}

function runBundle(app, ...args) {
  const run = spawnSync(process.execPath, [BIN, 'bundle', ...args], { cwd: app, encoding: 'utf8' })
  return { ...run, output: `${run.stdout}${run.stderr}` }
}

function assertRefused(run, app, ...needles) {
  assert.notEqual(run.status, 0, `expected a non-zero exit, got 0:\n${run.output}`)
  assert.equal(run.status, 1, run.output)
  for (const needle of needles) assert.ok(run.stderr.includes(needle), `stderr should mention ${JSON.stringify(needle)}:\n${run.stderr}`)
  assert.ok(!existsSync(join(app, 'app.skpkg')), 'a refused bundle must leave no package behind')
  const staging = readdirSync(app).filter((name) => name.startsWith('.app.skpkg-'))
  assert.deepEqual(staging, [], 'a refused bundle must leave no staging directory behind')
}

const sha256 = (path) => createHash('sha256').update(readFileSync(path)).digest('hex')

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

describe('screenkit bundle', () => {
  test('Bundle: a legacy dist becomes app.skpkg with manifest.json, app.hbc and the assets', () => {
    const app = makeApp(lazyAppFiles())
    const run = runBundle(app)
    assert.equal(run.status, 0, run.output)

    const pkg = join(app, 'app.skpkg')
    const manifest = JSON.parse(readFileSync(join(pkg, 'manifest.json'), 'utf8'))
    const hbc = readFileSync(join(pkg, 'app.hbc'))

    assert.equal(manifest.format, 1)
    assert.equal(manifest.runtimeVersion, RUNTIME_VERSION)
    assert.equal(manifest.entry, 'app.hbc')
    // Read from the header hermesc produced, and it is the pinned engine's.
    assert.equal(manifest.hermesBytecodeVersion, hbc.readUInt32LE(8))
    assert.equal(manifest.hermesBytecodeVersion, PINNED_BYTECODE)

    // Assets travel; the markup, the chunks (now inside app.hbc), their maps
    // and the packer's intermediate source do not.
    assert.deepEqual(Object.keys(manifest.files).sort(), ['app.hbc', 'assets/logo.png', 'fonts/probe.msdf.json'])
    for (const gone of ['index.html', 'app.js', 'assets/index-legacy-A.js', 'assets/index-legacy-A.js.map',
      'assets/Loading-legacy-B.js', 'assets/shared-legacy-C.js', 'assets/polyfills-legacy-P.js']) {
      assert.ok(!existsSync(join(pkg, gone)), `${gone} must not be in the package`)
    }
    for (const [file, hash] of Object.entries(manifest.files)) assert.equal(sha256(join(pkg, file)), hash, file)
    assert.ok((statSync(pkg).mode & 0o777) === 0o755, 'the package directory is world-readable')

    // The packed script stays findable beside the package -- it is what an
    // on-device app.js:line:col refers to -- and never becomes an asset, even
    // when it ends up inside a build output that is bundled again.
    const packed = join(app, 'app.skpkg.js')
    assert.ok(readFileSync(packed, 'utf8').startsWith('// Packed by screenkit bundle'))
    cpSync(packed, join(app, 'dist-screenkit/stray.js'))
    const again = runBundle(app)
    assert.equal(again.status, 0, again.output)
    assert.ok(!('stray.js' in JSON.parse(readFileSync(join(pkg, 'manifest.json'), 'utf8')).files))
  })

  test('--spidermonkey=source: the package also carries app.js and says so under engines.spidermonkey', () => {
    const app = makeApp(lazyAppFiles())
    const run = runBundle(app, '--spidermonkey=source')
    assert.equal(run.status, 0, run.output)
    const pkg = join(app, 'app.skpkg')
    const manifest = JSON.parse(readFileSync(join(pkg, 'manifest.json'), 'utf8'))
    // Hermes' entry is untouched; the source is the packed script, byte for byte.
    assert.equal(manifest.entry, 'app.hbc')
    assert.deepEqual(manifest.engines, { spidermonkey: { source: 'app.js' } })
    assert.equal(readFileSync(join(pkg, 'app.js'), 'utf8'), readFileSync(join(app, 'app.skpkg.js'), 'utf8'))
    assert.equal(manifest.files['app.js'], sha256(join(pkg, 'app.js')))
    assert.ok(!existsSync(join(pkg, 'app.stencil')), 'source mode compiles no stencil')
  })

  test('--spidermonkey takes source, lazy or eager', () => {
    const app = makeApp(lazyAppFiles())
    const run = runBundle(app, '--spidermonkey=fast')
    assert.equal(run.status, 2, run.output)
    assert.ok(run.stderr.includes('--spidermonkey takes source, lazy, eager'), run.stderr)
  })

  test("an asset named like SpiderMonkey's entries is refused, with or without --spidermonkey", () => {
    for (const name of ['app.js', 'App.Stencil']) {
      const app = makeApp({ ...lazyAppFiles(), [name]: 'x' })
      assertRefused(runBundle(app), app, 'collides with the package', name.toLowerCase())
    }
  })

  test('Not a ScreenKit build: plain vite build output is refused, pointing at the plugin', () => {
    const app = makeApp({
      'index.html': '<!doctype html><html><head><script type="module" crossorigin src="/assets/index-Bx1.js"></script></head><body></body></html>\n',
      'assets/index-Bx1.js': 'console.log("modern ESM")\n',
    })
    // A package from an earlier, good run must not survive a refused one.
    mkdirSync(join(app, 'app.skpkg'))
    writeFileSync(join(app, 'app.skpkg/manifest.json'), JSON.stringify({ format: 1, runtimeVersion: 1, hermesBytecodeVersion: 99, entry: 'app.hbc', files: {} }))
    assertRefused(runBundle(app), app, 'not a ScreenKit build', '@screenkit/vite-plugin')
  })

  test('Missing chunk: a registered dependency absent from dist/ is named', () => {
    const files = lazyAppFiles()
    delete files['assets/shared-legacy-C.js']
    const app = makeApp(files)
    assertRefused(runBundle(app), app, 'missing chunk assets/shared-legacy-C.js', 'assets/index-legacy-A.js')
  })

  test('Resolvable import(): a literal import of a chunk in dist/ is packed and loads from the package', async (t) => {
    const app = makeApp(lazyAppFiles())
    const run = runBundle(app)
    assert.equal(run.status, 0, run.output)
    assert.match(run.stdout, /chunk: assets\/Loading-legacy-B\.js {2}\(dynamic import\)/)

    // The packed script, booted the way the host boots app.hbc: only the
    // registry the packer built can serve the lazy chunk, because the fixture
    // System has no loader of its own.
    const dist = join(app, 'dist-screenkit')
    const { script } = packScript(dist, readLegacyLayout(dist))
    const logged = []
    const errors = []
    runInNewContext(script, { console: { log: (m) => logged.push(m), error: (m) => errors.push(m) } })
    await new Promise((resolve) => setTimeout(resolve, 50))
    assert.deepEqual(errors, [])
    const ready = 'screenkit bundle: entry "/assets/index-legacy-A.js" ready'
    assert.deepEqual(logged.filter((line) => line !== ready), ['hello from index', 'block scoping: 0,1,2', 'lazy chunk loaded, index ready: true'])
    assert.equal(logged.filter((line) => line === ready).length, 1, logged.join('\n'))

    // And the real thing: the package's app.hbc on Hermes, through the host's
    // manifest gate. The block-scoping line is what proves the compile flags --
    // node's vm above has block scoping whatever hermesc was told.
    if (!HOST) {
      t.skip('SCREENKIT_HOST is not set; the package was not run on a host')
      return
    }
    const host = spawnSync(HOST, [join(app, 'app.skpkg')], { encoding: 'utf8', timeout: 60_000 })
    const out = `${host.stdout}${host.stderr}`
    assert.equal(host.status, 0, out)
    assert.ok(out.includes('block scoping: 0,1,2'), out)
    assert.ok(out.includes('lazy chunk loaded, index ready: true'), out)
    assert.ok(out.includes(ready), out)
  })

  test('Unresolvable import(): a non-literal import() names the chunk and the call site', () => {
    const app = makeApp(lazyAppFiles({ entryBody: "var page = './Loading-legacy-B.js'; module.import(page);" }))
    const run = runBundle(app)
    assertRefused(run, app, 'unresolvable import()', 'module.import(page)')
    assert.match(run.stderr, /assets\/index-legacy-A\.js:12:\d+/)
  })

  test('Unresolvable import(): a literal import of a chunk not in dist/ names the chunk and the call site', () => {
    const app = makeApp(lazyAppFiles({ entryBody: "module.import('./pages/Gone-legacy-X.js');" }))
    const run = runBundle(app)
    assertRefused(run, app, 'assets/pages/Gone-legacy-X.js', "module.import('./pages/Gone-legacy-X.js')")
    assert.match(run.stderr, /assets\/index-legacy-A\.js:12:\d+/)
  })

  test('Missing chunk: a chunk outside the entry\'s directory is named as misplaced, not absent', () => {
    const files = lazyAppFiles()
    files['vendor/shared-legacy-C.js'] = files['assets/shared-legacy-C.js']
    delete files['assets/shared-legacy-C.js']
    files['assets/index-legacy-A.js'] = files['assets/index-legacy-A.js'].replace("'./shared-legacy-C.js'", "'../vendor/shared-legacy-C.js'")
    const app = makeApp(files)
    assertRefused(runBundle(app), app, 'missing chunk vendor/shared-legacy-C.js', "not in the entry's directory, assets/")
  })

  test("hermesc rejects code: the chunk it cannot compile fails the bundle with hermesc's first diagnostic", () => {
    // Valid in every browser; Hermes has no async generators.
    const app = makeApp(lazyAppFiles({ entryBody: 'async function* pages() { yield 1; }' }))
    const run = runBundle(app)
    assertRefused(run, app, 'hermesc rejected', 'error: async generators are unsupported')
    // Named against the chunk it came from, not the packed script it failed in.
    assert.match(run.stderr, /assets\/index-legacy-A\.js:12:\d+: error: async generators are unsupported/)
  })

  // Refusals that protect what is already on disk.

  test('--out naming a directory that is not a package is refused and left intact, even with a manifest.json', () => {
    const app = makeApp(lazyAppFiles())
    const site = join(app, 'public-site')
    mkdirSync(site)
    const webManifest = '{ "name": "My PWA", "icons": [] }\n'
    writeFileSync(join(site, 'manifest.json'), webManifest)
    writeFileSync(join(site, 'index.html'), '<!doctype html>')
    assertRefused(runBundle(app, '--out', 'public-site'), app, 'refusing to replace', 'is not a .skpkg')
    assert.equal(readFileSync(join(site, 'manifest.json'), 'utf8'), webManifest)
    assert.ok(existsSync(join(site, 'index.html')))
  })

  test('--out inside the build output is refused and the build output left intact', () => {
    const app = makeApp(lazyAppFiles())
    assertRefused(runBundle(app, '--out', 'dist-screenkit/app.skpkg'), app, 'must not contain one another')
    assert.ok(!existsSync(join(app, 'dist-screenkit/app.skpkg')))
    assert.deepEqual(readdirSync(join(app, 'dist-screenkit')).sort(), ['assets', 'fonts', 'index.html'])
  })

  for (const name of ['app.hbc', 'App.hbc']) {
    test(`a build output containing ${name} is refused: it would overwrite the compiled entry`, () => {
      const app = makeApp({ ...lazyAppFiles(), [name]: 'not bytecode' })
      assertRefused(runBundle(app), app, `dist-screenkit/${name} collides with the package's own app.hbc`)
      assert.equal(readFileSync(join(app, 'dist-screenkit', name), 'utf8'), 'not bytecode')
    })
  }

  // ---------------------------------------------------------------------------
  // Symlinks in the build output
  // ---------------------------------------------------------------------------

  test('a symlink to a file inside the build output ships as that file', () => {
    const app = makeApp(lazyAppFiles())
    symlinkSync('logo.png', join(app, 'dist-screenkit/assets/logo-link.png'))
    const run = runBundle(app)
    assert.equal(run.status, 0, run.output)
    const shipped = join(app, 'app.skpkg/assets/logo-link.png')
    assert.ok(statSync(shipped).isFile())
    assert.deepEqual(readFileSync(shipped), readFileSync(join(app, 'dist-screenkit/assets/logo.png')))
  })

  test('a dangling symlink is refused by name, not with a stack trace', () => {
    const app = makeApp(lazyAppFiles())
    symlinkSync('nowhere.png', join(app, 'dist-screenkit/assets/broken.png'))
    const run = runBundle(app)
    assertRefused(run, app, 'dist-screenkit/assets/broken.png is a symlink to nothing')
    assert.ok(!run.stderr.includes('    at '), run.stderr)
  })

  test('a symlink back up the tree is refused as a loop', () => {
    const app = makeApp(lazyAppFiles())
    symlinkSync('..', join(app, 'dist-screenkit/assets/up'))
    assertRefused(runBundle(app), app, 'dist-screenkit/assets/up is a symlink loop')
  })

  test('a symlink out of the build output is refused, so nothing outside it ships', () => {
    const app = makeApp(lazyAppFiles())
    const outside = join(app, 'secrets')
    mkdirSync(outside)
    writeFileSync(join(outside, 'key.txt'), 'not for the package')
    symlinkSync(outside, join(app, 'dist-screenkit/assets/ext'))
    assertRefused(runBundle(app), app, 'dist-screenkit/assets/ext is a symlink out of the build output')
  })

  // ---------------------------------------------------------------------------
  // Import shapes the check follows, and ones it refuses
  // ---------------------------------------------------------------------------

  test('an optional-call import() of a literal is followed like a plain one', () => {
    const app = makeApp(lazyAppFiles({
      entryBody: "module.import?.('./Loading-legacy-B.js').then(function (page) { console.log(page.message); });",
    }))
    const run = runBundle(app)
    assert.equal(run.status, 0, run.output)
    assert.match(run.stdout, /chunk: assets\/Loading-legacy-B\.js {2}\(dynamic import\)/)
  })

  test('the import context used as a value is refused: an import() through it could not be checked', () => {
    const app = makeApp(lazyAppFiles({
      entryBody: "function load(ctx) { return ctx.import('./Gone-legacy-X.js'); } load(module);",
    }))
    const run = runBundle(app)
    assertRefused(run, app, 'the import context `module` is used as a value', 'load(module)')
    assert.match(run.stderr, /assets\/index-legacy-A\.js:12:\d+/)
  })

  test('module.meta and a property that merely shares the name are not uses of the context', () => {
    const app = makeApp(lazyAppFiles({
      entryBody: "var url = module.meta.url; var o = { module: 1 }; o.module; module: for (;;) { break module; }",
    }))
    const run = runBundle(app)
    assert.equal(run.status, 0, run.output)
  })

  test('a destructured context parameter is refused', () => {
    const files = lazyAppFiles({ entryBody: 'load;' })
    files['assets/index-legacy-A.js'] = files['assets/index-legacy-A.js']
      .replace('function (exports, module) {', 'function (exports, { import: load }) {')
      .replace('function inner(module) { return module.import(somewhere); }', '')
    const app = makeApp(files)
    assertRefused(runBundle(app), app, "the register factory's context parameter is destructured")
  })

  test('a named System.register is refused: the packed loader serves anonymous chunks only', () => {
    const files = lazyAppFiles()
    files['assets/shared-legacy-C.js'] = files['assets/shared-legacy-C.js'].replace('System.register([]', "System.register('shared', []")
    const app = makeApp(files)
    assertRefused(runBundle(app), app, 'assets/shared-legacy-C.js:1:1: System.register("shared", ...) is a named registration')
  })

  // ---------------------------------------------------------------------------
  // Layout
  // ---------------------------------------------------------------------------

  test('an entry whose name lacks -legacy is still packed, and boots', async () => {
    const files = lazyAppFiles()
    files['assets/main.js'] = files['assets/index-legacy-A.js']
    delete files['assets/index-legacy-A.js']
    delete files['assets/index-legacy-A.js.map']
    files['assets/Loading-legacy-B.js'] = files['assets/Loading-legacy-B.js'].replace('./index-legacy-A.js', './main.js')
    files['index.html'] = legacyIndexHtml('main.js')
    const app = makeApp(files)
    const run = runBundle(app)
    assert.equal(run.status, 0, run.output)
    assert.match(run.stdout, /chunk: assets\/main\.js\n/)

    const dist = join(app, 'dist-screenkit')
    const logged = []
    const errors = []
    runInNewContext(packScript(dist, readLegacyLayout(dist)).script, {
      console: { log: (m) => logged.push(m), error: (m) => errors.push(m) },
    })
    await new Promise((resolve) => setTimeout(resolve, 50))
    assert.deepEqual(errors, [])
    assert.ok(logged.includes('lazy chunk loaded, index ready: true'), logged.join('\n'))
  })

  test('a stale polyfills bundle from an earlier build is neither packed nor shipped', () => {
    const app = makeApp({ ...lazyAppFiles(), 'assets/polyfills-legacy-OLD.js': MINI_SYSTEM })
    const run = runBundle(app)
    assert.equal(run.status, 0, run.output)
    assert.ok(!run.stdout.includes('polyfills-legacy-OLD'), run.stdout)
    assert.ok(!existsSync(join(app, 'app.skpkg/assets/polyfills-legacy-OLD.js')))
  })

  test('a build with a non-root Vite base is refused, naming the base', () => {
    const app = makeApp({
      ...lazyAppFiles(),
      'index.html': legacyIndexHtml().replaceAll('"/assets/', '"/myApp/assets/'),
    })
    assertRefused(runBundle(app), app, 'this build used Vite base "/myApp/"', 'build with base "/"')
  })

  // ---------------------------------------------------------------------------
  // Fonts ship as they are: an MSDF atlas is optional, since the 2D canvas
  // draws text from the font file itself
  // ---------------------------------------------------------------------------

  test('a font ships with or without an atlas beside it', () => {
    const app = makeApp({
      ...lazyAppFiles(),
      'fonts/Lato-Regular.ttf': 'font',
      'fonts/Kalam.woff': 'font',
      'fonts/Kalam.msdf.json': '{}',
      'fonts/Kalam.msdf.png': Buffer.from([0x89, 0x50, 0x4e, 0x47]),
    })
    const run = runBundle(app)
    assert.equal(run.status, 0, run.output)
    const files = Object.keys(JSON.parse(readFileSync(join(app, 'app.skpkg/manifest.json'), 'utf8')).files)
    for (const file of ['fonts/Lato-Regular.ttf', 'fonts/Kalam.woff', 'fonts/Kalam.msdf.json', 'fonts/Kalam.msdf.png']) {
      assert.ok(files.includes(file), file)
    }
  })

  // ---------------------------------------------------------------------------
  // A rejected entry is fatal
  // ---------------------------------------------------------------------------

  test('Entry rejects: the packed script reports the failure to the host, naming the entry and the error', async (t) => {
    const app = makeApp(lazyAppFiles({ entryBody: "throw new Error('boom from the entry');" }))
    const run = runBundle(app)
    assert.equal(run.status, 0, run.output)

    // In node's vm, with a stand-in for the host binding.
    const dist = join(app, 'dist-screenkit')
    const errors = []
    const reported = []
    const logged = []
    runInNewContext(packScript(dist, readLegacyLayout(dist)).script, {
      console: { log: (m) => logged.push(m), error: (m) => errors.push(m) },
      __screenkit: { reportFailure: (m) => reported.push(m) },
    })
    await new Promise((resolve) => setTimeout(resolve, 50))
    assert.equal(reported.length, 1, errors.join('\n'))
    assert.match(reported[0], /^screenkit bundle: entry "\/assets\/index-legacy-A\.js" failed: .*boom from the entry/s)
    assert.deepEqual(errors, reported)
    // Not ready: a launch check waiting for that line must not see it.
    assert.ok(!logged.some((line) => String(line).includes('" ready')), logged.join('\n'))

    // And on the host: a non-zero exit, not a clean one.
    if (!HOST) {
      t.skip('SCREENKIT_HOST is not set; the package was not run on a host')
      return
    }
    const host = spawnSync(HOST, [join(app, 'app.skpkg')], { encoding: 'utf8', timeout: 60_000 })
    const out = `${host.stdout}${host.stderr}`
    assert.equal(host.status, 65, out)
    assert.ok(out.includes('screenkit bundle: entry "/assets/index-legacy-A.js" failed'), out)
    assert.ok(out.includes('boom from the entry'), out)
    assert.ok(out.includes('the app reported a fatal failure; exiting with 65'), out)
    assert.ok(!out.includes('" ready'), out)
  })

  test('runtime/VERSION must be an integer from 1 to 4294967295, the host gate\'s uint32 range', () => {
    assert.equal(parseRuntimeVersion(' 7\n'), 7)
    assert.equal(parseRuntimeVersion(String(MAX_RUNTIME_VERSION)), 4294967295)
    for (const bad of ['0', '4294967296', '99999999999', '-1', '1.5', 'one', '']) {
      assert.throws(() => parseRuntimeVersion(bad), /must hold one integer from 1 to 4294967295/, bad)
    }
  })
})
