// `screenkit bundle` -- the packaging half of the M5 pipeline.
//
//   dist-screenkit/  (vite build + @screenkit/vite-plugin)
//     -> check the chunk graph, dynamic import() included     graph.js
//     -> pack every chunk into one script                     pack.js
//     -> compile it with the pinned hermesc                   hermesc.js
//     -> copy the assets, hash everything,
//        write the manifest
//   app.skpkg/  manifest.json · app.hbc · the app's assets
//
// A .skpkg is a directory. Its root is the app's asset root on device, so a
// `/fonts/x.png` the app asks for is `app.skpkg/fonts/x.png`.
//
// The package is assembled in a staging directory beside `out` and renamed into
// place only once it is complete, and any previous package at `out` is removed
// first -- so a failed bundle leaves no package rather than a stale or half-built
// one that a later host build would embed without complaint.
//
// The packed script is kept beside the package as `<out>.js` (app.skpkg.js), not
// in it: on device a stack trace says `app.js:line:col`, and that file is where
// those lines are.

import { createHash } from 'node:crypto'
import {
  chmodSync,
  closeSync,
  copyFileSync,
  existsSync,
  lstatSync,
  mkdirSync,
  mkdtempSync,
  openSync,
  readFileSync,
  readSync,
  readdirSync,
  realpathSync,
  renameSync,
  rmSync,
  statSync,
  writeFileSync,
} from 'node:fs'
import { execFileSync } from 'node:child_process'
import { basename, dirname, join, posix, relative, resolve, sep } from 'node:path'

import { BundleError } from './errors.js'
import { checkChunkGraph } from './graph.js'
import { REPO_ROOT, compile, pinnedHermesc, readBytecodeVersion } from './hermesc.js'
import { PACKED_BANNER, isPolyfillBundle, packScript, readLegacyLayout } from './pack.js'

export { BundleError } from './errors.js'

export const PACKAGE_FORMAT = 1
export const DEFAULT_DIST = 'dist-screenkit'
export const DEFAULT_OUT = 'app.skpkg'
const ENTRY = 'app.hbc'
const MANIFEST = 'manifest.json'
// A SpiderMonkey host (runtime/core/src/spidermonkey) runs the packed source, or
// a stencil precompiled from it for exactly the engine build the device loads.
const SPIDERMONKEY_SOURCE = 'app.js'
const SPIDERMONKEY_STENCIL = 'app.stencil'
/**
 * What a package carries for SpiderMonkey, besides app.hbc for Hermes:
 *   ''       nothing (the default)
 *   source   app.js, the packed script -- parsed on the device at every launch
 *   lazy     + app.stencil, compiled ahead; an inner function is compiled from
 *            the source it carries on its first call
 *   eager    + app.stencil with every function compiled ahead: nothing is parsed
 *            on the device
 */
export const SPIDERMONKEY_MODES = Object.freeze(['source', 'lazy', 'eager'])

/** runtime/VERSION: the one number both this and the host's gate are built from. */
export function runtimeVersion() {
  const file = join(REPO_ROOT, 'runtime', 'VERSION')
  let text
  try {
    text = readFileSync(file, 'utf8')
  } catch (err) {
    throw new BundleError(`cannot read the runtime version from ${file}: ${err.message}`)
  }
  return parseRuntimeVersion(text, file)
}

// The host compiles the version into a uint32 and CMake enforces the same range,
// so a manifest can never carry a number the gate would truncate.
export const MAX_RUNTIME_VERSION = 0xffffffff

export function parseRuntimeVersion(text, file = 'runtime/VERSION') {
  const trimmed = String(text).trim()
  if (!/^[1-9][0-9]{0,9}$/.test(trimmed) || Number(trimmed) > MAX_RUNTIME_VERSION) {
    throw new BundleError(
      `${file} must hold one integer from 1 to ${MAX_RUNTIME_VERSION}, found "${trimmed}"`,
    )
  }
  return Number(trimmed)
}

/**
 * @param {{ dist?: string, out?: string, cwd?: string, log?: (line: string) => void }} [options]
 * @returns {{ out: string, manifest: object }}
 */
export function bundle({
  dist = DEFAULT_DIST,
  out = DEFAULT_OUT,
  cwd = process.cwd(),
  log = () => {},
  spidermonkey = '',
} = {}) {
  if (spidermonkey && !SPIDERMONKEY_MODES.includes(spidermonkey)) {
    throw new BundleError(`--spidermonkey takes ${SPIDERMONKEY_MODES.join(', ')}, not "${spidermonkey}"`)
  }
  const distDir = resolve(cwd, dist)
  const outDir = resolve(cwd, out)

  if (!isDirectory(distDir)) {
    throw new BundleError(
      `no build output at ${distDir} -- run \`vite build\` with @screenkit/vite-plugin first ` +
        `(it writes ${DEFAULT_DIST}/), or name the directory: screenkit bundle <dist>`,
    )
  }
  if (isInside(outDir, distDir) || isInside(distDir, outDir)) {
    throw new BundleError(`--out ${outDir} and the build output ${distDir} must not contain one another`)
  }
  removePreviousPackage(outDir)

  const version = runtimeVersion()
  const layout = readLegacyLayout(distDir)
  const hermesc = pinnedHermesc()
  const { edges } = checkChunkGraph(hermesc, distDir, layout)
  const { script, segments } = packScript(distDir, layout)

  log(`packed ${layout.chunks.length} chunk(s) + polyfills (${(script.length / 1024).toFixed(0)} KB)`)
  log(`  entry: ${layout.entry}`)
  for (const chunk of layout.chunks) {
    const file = posix.join(layout.chunkDir, chunk)
    const lazy = edges.some((e) => e.dynamic && e.to === file)
    log(`  chunk: ${file}${lazy ? '  (dynamic import)' : ''}`)
  }

  mkdirSync(dirname(outDir), { recursive: true })
  const staging = mkdtempSync(join(dirname(outDir), `.${basename(outDir)}-`))
  try {
    // Compiled as `app.js` from inside the staging directory, because the name
    // hermesc is given is the name a stack trace on device shows. The source
    // itself does not ship.
    writeFileSync(join(staging, 'app.js'), script)
    compile(hermesc, staging, 'app.js', ENTRY, (line) => {
      const segment = segments.find((s) => line >= s.start && line <= s.end)
      if (!segment) return null
      const at = line - segment.start + 1
      const text = readFileSync(join(distDir, segment.file), 'utf8').split('\n')[at - 1] ?? ''
      return { file: segment.file, line: at, text }
    })
    const packedScript = `${outDir}.js`
    renameSync(join(staging, 'app.js'), packedScript)
    log(`packed script -> ${packedScript} (what app.js:line:col in a stack trace refers to)`)

    const hermesBytecodeVersion = readBytecodeVersion(join(staging, ENTRY))
    log(`compiled -> ${ENTRY} (${(statSync(join(staging, ENTRY)).size / 1024).toFixed(0)} KB, bytecode version ${hermesBytecodeVersion})`)

    const engineFiles = []
    if (spidermonkey) {
      copyFileSync(packedScript, join(staging, SPIDERMONKEY_SOURCE))
      engineFiles.push(SPIDERMONKEY_SOURCE)
      if (spidermonkey !== 'source') {
        compileStencil(join(staging, SPIDERMONKEY_SOURCE), join(staging, SPIDERMONKEY_STENCIL), spidermonkey === 'eager')
        engineFiles.push(SPIDERMONKEY_STENCIL)
        log(`compiled -> ${SPIDERMONKEY_STENCIL} (${(statSync(join(staging, SPIDERMONKEY_STENCIL)).size / 1024).toFixed(0)} KB, SpiderMonkey, ${spidermonkey})`)
      }
    }

    const assets = copyAssets(distDir, staging, layout)
    log(`copied ${assets.length} asset(s)`)

    const files = {}
    for (const file of [ENTRY, ...engineFiles, ...assets].sort()) {
      files[file] = createHash('sha256').update(readFileSync(join(staging, file))).digest('hex')
    }
    const manifest = {
      format: PACKAGE_FORMAT,
      runtimeVersion: version,
      hermesBytecodeVersion,
      entry: ENTRY,
      files,
    }
    if (spidermonkey) {
      manifest.engines = {
        spidermonkey: {
          source: SPIDERMONKEY_SOURCE,
          ...(engineFiles.includes(SPIDERMONKEY_STENCIL) ? { stencil: SPIDERMONKEY_STENCIL } : {}),
        },
      }
    }
    writeFileSync(join(staging, MANIFEST), `${JSON.stringify(manifest, null, 2)}\n`)

    // mkdtemp makes the directory 0700; a package is read by whoever runs it.
    chmodSync(staging, 0o755)
    renameSync(staging, outDir)
    log(`package -> ${outDir} (runtimeVersion ${version}, hermesBytecodeVersion ${hermesBytecodeVersion})`)
    return { out: outDir, manifest }
  } catch (err) {
    rmSync(staging, { recursive: true, force: true })
    throw err
  }
}

/**
 * app.js -> app.stencil with screenkit-smc, in the container holding the pinned
 * libmozjs (tools/spidermonkey/smc.sh): a stencil loads only in the engine build
 * that wrote it.
 */
function compileStencil(source, stencil, eager) {
  const smc = join(REPO_ROOT, 'tools', 'spidermonkey', 'smc.sh')
  try {
    execFileSync('sh', [smc, ...(eager ? ['--eager'] : []), source, stencil], {
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'pipe'],
    })
  } catch (err) {
    const why = String(err.stderr || err.message).trim().split('\n').slice(-5).join('\n  ')
    throw new BundleError(`screenkit-smc could not compile ${source} to a stencil:\n  ${why}`)
  }
}

/**
 * Everything in dist the app can load at runtime: not index.html (a manifest of
 * entry points, never rendered) and not the legacy chunks, which are inside
 * app.hbc now. Returns package-relative posix paths.
 *
 * A symlink is followed only while it stays inside the build output: a package
 * carries the app's files, not whatever a link happens to reach, and a link back
 * up the tree would otherwise recurse until the path is too long.
 */
function copyAssets(distDir, staging, layout) {
  const packed = new Set(['index.html', layout.polyfill, ...layout.chunks.map((c) => posix.join(layout.chunkDir, c))])
  for (const file of [...packed]) packed.add(`${file}.map`)
  const realDist = realpathSync(distDir)

  const copied = []
  // `ancestors`: the real directories on the way down, which a loop comes back to.
  const walk = (dir, ancestors) => {
    for (const entry of readdirSync(join(distDir, dir), { withFileTypes: true })) {
      const rel = dir ? posix.join(dir, entry.name) : entry.name
      const from = join(distDir, rel)
      let stat = entry
      if (entry.isSymbolicLink()) {
        let real
        try {
          real = realpathSync(from)
        } catch (err) {
          throw new BundleError(`${from} is a symlink to nothing (${err.code}); remove it or point it at a file in the build`)
        }
        if (!isInside(real, realDist)) {
          throw new BundleError(
            `${from} is a symlink out of the build output, to ${real}; a package carries only what is in ${distDir}`,
          )
        }
        stat = statSync(real)
      }
      if (stat.isDirectory()) {
        const real = realpathSync(from)
        if (ancestors.has(real)) {
          throw new BundleError(`${from} is a symlink loop: it leads back to ${real}, which contains it`)
        }
        walk(rel, new Set([...ancestors, real]))
        continue
      }
      const stalePolyfill = dir === layout.chunkDir && isPolyfillBundle(entry.name)
      if (!stat.isFile() || packed.has(rel) || stalePolyfill || isPackedScript(from)) continue
      // Case-insensitively: on APFS `App.hbc` is the same file as the entry. The
      // SpiderMonkey names are reserved whether or not this package carries them.
      if ([ENTRY, MANIFEST, SPIDERMONKEY_SOURCE, SPIDERMONKEY_STENCIL].includes(rel.toLowerCase())) {
        throw new BundleError(
          `${from} collides with the package's own ${rel.toLowerCase()}; rename it, or rebuild if it is ` +
            'left over from an earlier build (vite build empties the directory)',
        )
      }
      mkdirSync(dirname(join(staging, rel)), { recursive: true })
      copyFileSync(from, join(staging, rel))
      copied.push(rel)
    }
  }
  walk('', new Set([realDist]))
  return copied
}

/**
 * A package from an earlier run goes before anything else can fail. Anything
 * else at `out` is not ours to delete -- including a directory that merely has
 * a manifest.json, like a PWA's public/. An empty directory may be replaced.
 */
function removePreviousPackage(outDir) {
  if (!existsSync(outDir) && !isSymlink(outDir)) return
  const stat = lstatSync(outDir)
  if (!stat.isDirectory() || !(readdirSync(outDir).length === 0 || hasPackageManifest(outDir))) {
    throw new BundleError(
      `refusing to replace ${outDir}: it exists and is not a .skpkg ` +
        `(no ${MANIFEST} with format ${PACKAGE_FORMAT}, an entry and a runtimeVersion)`,
    )
  }
  rmSync(outDir, { recursive: true, force: true })
}

function hasPackageManifest(dir) {
  try {
    const manifest = JSON.parse(readFileSync(join(dir, MANIFEST), 'utf8'))
    return (
      manifest?.format === PACKAGE_FORMAT &&
      typeof manifest.entry === 'string' &&
      typeof manifest.runtimeVersion === 'number'
    )
  } catch {
    return false
  }
}

/** A script `screenkit bundle` packed on an earlier run, wherever it ended up. Never an asset. */
function isPackedScript(path) {
  if (!path.endsWith('.js')) return false
  const banner = Buffer.from(PACKED_BANNER)
  const head = Buffer.alloc(banner.length)
  const fd = openSync(path, 'r')
  try {
    return readSync(fd, head, 0, head.length, 0) === head.length && head.equals(banner)
  } finally {
    closeSync(fd)
  }
}

function isDirectory(path) {
  try {
    return statSync(path).isDirectory()
  } catch {
    return false
  }
}

function isSymlink(path) {
  try {
    return lstatSync(path).isSymbolicLink()
  } catch {
    return false
  }
}

/** Whether `child` is `parent` or somewhere under it. */
function isInside(child, parent) {
  const rel = relative(parent, child)
  return rel === '' || (!rel.startsWith(`..${sep}`) && rel !== '..' && !rel.startsWith(sep))
}
