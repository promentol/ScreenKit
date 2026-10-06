// Pack a @vitejs/plugin-legacy build into ONE script Hermes can compile.
//
// Legacy emits SystemJS chunks and boots them from index.html with
// `System.import(url)`. SystemJS then loads each chunk by injecting a <script>
// tag -- and a TV runtime has no script tags and no module loader.
//
// So instead of teaching the runtime to load scripts, resolve the whole graph
// here, at build time:
//
//   1. the polyfills bundle, which defines `System`
//   2. every chunk, each run inside capture(), which takes its anonymous
//      `System.register([...])` straight back out with `System.getRegister()`
//   3. `System.instantiate` overridden to serve those captured registrations
//   4. the entry `System.import`, exactly as index.html would have made it --
//      except that it logs "entry ... ready" when it resolves, and a rejection
//      is reported to the host as fatal (__screenkit.reportFailure, runtime 2)
//
// `instantiate` is SystemJS's documented extension point for "where does the
// code for this URL come from", so the loader itself is untouched. Chunk order
// does not matter: nothing executes until import, and import walks the graph.
// That includes a dynamic import(): legacy renders it as `context.import(url)`,
// which lands in the same instantiate -- which is why graph.js insists every
// such URL is a chunk packed here.

import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs'
import { join, posix } from 'node:path'

import { BundleError } from './errors.js'

/**
 * Read what index.html says to boot, and which chunks travel with it. Paths are
 * posix and relative to `dist`.
 */
export function readLegacyLayout(dist) {
  const indexPath = join(dist, 'index.html')
  if (!existsSync(indexPath)) {
    throw new BundleError(
      `${dist} has no index.html -- expected the output of \`vite build\` with @screenkit/vite-plugin`,
    )
  }
  const html = readFileSync(indexPath, 'utf8')
  const polyfillSrc = /id="vite-legacy-polyfill"[^>]*src="([^"]+)"/.exec(html)?.[1]
  const entrySrc = /id="vite-legacy-entry"[^>]*data-src="([^"]+)"/.exec(html)?.[1]
  if (!entrySrc) {
    throw new BundleError(
      `${indexPath} has no vite-legacy-entry script, so this is not a ScreenKit build. ` +
        'Add @screenkit/vite-plugin to the Vite config (plugins: [screenkit()]) and rebuild; ' +
        'it produces the legacy SystemJS output this packs.',
    )
  }
  if (!polyfillSrc) {
    throw new BundleError(
      `${indexPath} has a vite-legacy-entry but no vite-legacy-polyfill script. The polyfills ` +
        'bundle is what defines System; build with @screenkit/vite-plugin, which keeps polyfills: true.',
    )
  }

  const polyfill = distPath(polyfillSrc)
  const entry = distPath(entrySrc)
  for (const [path, src, role] of [[polyfill, polyfillSrc, 'loads it as vite-legacy-polyfill'], [entry, entrySrc, 'boots it as vite-legacy-entry']]) {
    if (isFile(join(dist, path))) continue
    throw new BundleError(`missing chunk ${path}: index.html ${role}, but it is not in ${dist}${baseHint(dist, src)}`)
  }

  // Every legacy chunk beside the entry. Chunk file names are what the runtime
  // registry is keyed by, so they are taken from one directory only. The entry
  // is always one of them, whatever it is called; a polyfills bundle never is --
  // the current one is loaded separately, and an older one is stale.
  const chunkDir = posix.dirname(entry)
  const chunks = [
    ...new Set([
      posix.basename(entry),
      ...readdirSync(join(dist, chunkDir)).filter(
        (f) => f.endsWith('.js') && f.includes('-legacy') && !isPolyfillBundle(f) && f !== posix.basename(polyfill),
      ),
    ]),
  ].sort()

  return { entrySrc, polyfill, entry, chunkDir, chunks }
}

/** A plugin-legacy polyfills bundle, current or left over from an earlier build. */
export function isPolyfillBundle(file) {
  return /^polyfills-legacy[-.].*\.js$/.test(file)
}

/**
 * When index.html names `/myApp/assets/x.js` and dist has `assets/x.js`, the
 * build used a non-root Vite `base`. Say so, rather than only "missing".
 */
function baseHint(dist, src) {
  const segments = src.replace(/^\/+/, '').split('/')
  for (let i = 1; i < segments.length; i++) {
    if (isFile(join(dist, ...segments.slice(i)))) {
      const base = `/${segments.slice(0, i).join('/')}/`
      return (
        ` -- it is there as ${segments.slice(i).join('/')}, so this build used Vite base ${JSON.stringify(base)}. ` +
        'A package is served from its own root: build with base "/" (@screenkit/vite-plugin sets it)'
      )
    }
  }
  return ''
}

/** The first line of every packed script -- how a later bundle recognises one and never ships it as an asset. */
export const PACKED_BANNER = '// Packed by screenkit bundle -- do not edit; rebuild instead.\n'

/**
 * The packed script, and where each input landed in it (1-based, inclusive
 * lines) so a hermesc diagnostic against app.js can name the chunk it came from.
 */
export function packScript(dist, layout) {
  const { polyfill, chunkDir, chunks, entrySrc } = layout
  const polyfillCode = readFileSync(join(dist, polyfill), 'utf8')
  const chunkCode = chunks.map((file) => readFileSync(join(dist, chunkDir, file), 'utf8'))

  // A chunk's source is dropped inside a function body. A trailing line comment
  // (a sourceMappingURL, say) would swallow the closing brace, so each body is
  // ended on its own line.
  const capture = (file, i) => `__capture(${JSON.stringify(file)}, function () {\n${chunkCode[i]}\n});`

  const script = `${PACKED_BANNER}${polyfillCode}
;(function () {
  if (typeof System === 'undefined') {
    throw new Error('screenkit bundle: the polyfills bundle did not define System');
  }
  var registry = Object.create(null);
  function __capture(name, run) {
    run();
    var reg = System.getRegister();
    if (!reg) throw new Error('screenkit bundle: ' + name + ' did not call System.register');
    registry[name] = reg;
  }
${chunks.map(capture).join('\n')}

  // Chunks are keyed by file name: SystemJS resolves URLs against location.href,
  // and what that base looks like is the runtime's business, not the packer's.
  var instantiate = System.instantiate;
  System.instantiate = function (url) {
    var reg = registry[String(url).split('/').pop()];
    return reg ? Promise.resolve(reg) : instantiate.apply(this, arguments);
  };

  // A rejected entry is fatal, not merely logged: it settles long after the
  // host's evaluate returned, so reportFailure is how the host learns of it --
  // and exits non-zero instead of idling on a blank screen or passing a CI run.
  // A resolved one says so, which is what a launch check waits for: the entry
  // module and everything it imports have executed.
  var entry = ${JSON.stringify(entrySrc)};
  System.import(entry).then(function () {
    // The document has loaded: DOMContentLoaded and load, which the runtime's DOM
    // shim fires once (an older runtime has no hook, and needs none).
    if (typeof __screenkitDocumentLoaded === 'function') __screenkitDocumentLoaded();
    console.log('screenkit bundle: entry ' + JSON.stringify(entry) + ' ready');
  }, function (err) {
    var message = 'screenkit bundle: entry ' + JSON.stringify(entry) + ' failed: ' + ((err && err.stack) || String(err));
    console.error(message);
    __screenkit.reportFailure(message);
  });
})();
`

  // The polyfills open on line 2, straight after the banner. Each chunk opens
  // on the line after its __capture marker; markers are in chunk order, so one
  // forward scan counts the lines to all of them.
  const lines = (text) => text.split('\n').length
  const segments = [{ file: polyfill, start: 2, end: 1 + lines(polyfillCode) }]
  let line = 1
  let scanned = 0
  chunks.forEach((file, i) => {
    const marker = `__capture(${JSON.stringify(file)}, function () {\n`
    const at = script.indexOf(marker, scanned) + marker.length
    for (let n = script.indexOf('\n', scanned); n !== -1 && n < at; n = script.indexOf('\n', n + 1)) line++
    scanned = at
    segments.push({ file: posix.join(chunkDir, file), start: line, end: line + lines(chunkCode[i]) - 1 })
  })

  return { script, segments }
}

/** `/assets/x.js` or `assets/x.js` -> `assets/x.js`, posix, relative to dist. */
function distPath(src) {
  return posix.normalize(src.replace(/^\/+/, ''))
}

function isFile(path) {
  try {
    return statSync(path).isFile()
  } catch {
    return false
  }
}
