#!/usr/bin/env node
// Fetch prebuilt third-party binaries described by manifest.json.
//
//   tools/prebuilts/fetch.mjs                      # every dep, target matching this host
//   tools/prebuilts/fetch.mjs apple-tvos-simulator-arm64
//   tools/prebuilts/fetch.mjs android-arm64 android-arm32   # the AARs the Android build consumes
//   tools/prebuilts/fetch.mjs --dep hermes         # one dep only (angle | sdl3 | sdl3_ttf | hermes)
//   tools/prebuilts/fetch.mjs --all
//   tools/prebuilts/fetch.mjs --list
//   tools/prebuilts/fetch.mjs --hermesc            # prints the host hermesc path on stdout
//
// Every archive is checksum-verified against the manifest, and the ANGLE headers
// are pinned to a commit id. A fresh clone must reach
// a running app without compiling any third-party code.

import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import {
  cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, renameSync, statSync, writeFileSync, rmSync,
} from 'node:fs';
import { inflateRawSync } from 'node:zlib';
import { homedir, tmpdir } from 'node:os';
import { basename, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const MANIFEST = JSON.parse(readFileSync(join(HERE, 'manifest.json'), 'utf8'));
const ROOT = process.env.SCREENKIT_PREBUILTS ?? join(homedir(), '.screenkit', 'prebuilts');

const angle = MANIFEST.angle;
const sdl3 = MANIFEST.sdl3;
const sdl3Ttf = MANIFEST.sdl3_ttf;
const hermes = MANIFEST.hermes;

// "chromium/7578" -> "chromium-7578"; Hermes versions have no slash but the
// rule is the same, so the on-disk shape is <root>/<dep>/<version>/<target>/.
const versionDir = (v) => v.replaceAll('/', '-');
const ANGLE_BASE = join(ROOT, 'angle', versionDir(angle.version));
const SDL3_BASE = join(ROOT, 'sdl3', versionDir(sdl3.version));
const SDL3_TTF_BASE = join(ROOT, 'sdl3_ttf', versionDir(sdl3Ttf.version));
const HERMES_BASE = join(ROOT, 'hermes', versionDir(hermes.version));

// Log to stderr so --hermesc can put a bare path on stdout.
const say = (...a) => console.error(...a);

// Wrap rather than cut at the first period: these reasons carry version numbers
// like "v0.13.0", and truncating at "." turned the Linux explanation into
// nonsense.
function wrap(text, width, indent) {
  const lines = [];
  let line = '';
  for (const word of text.split(/\s+/)) {
    if (line && line.length + 1 + word.length > width) {
      lines.push(line);
      line = word;
    } else {
      line = line ? `${line} ${word}` : word;
    }
  }
  if (line) lines.push(line);
  return lines.map((l, i) => (i === 0 ? l : indent + l));
}

function printReason(key, why) {
  const indent = ' '.repeat(4 + 30);
  const [first, ...rest] = wrap(why, 88, indent);
  console.log(`    ${key.padEnd(30)}${first}`);
  for (const line of rest) console.log(line);
}

/// First sentence only, for one-line progress output where the full reason
/// would drown the log. Keeps decimals intact.
function firstSentence(text) {
  const match = /[.!?](\s|$)/.exec(text.replace(/(\d)\.(\d)/g, '$1\u0000$2'));
  const cut = match ? match.index + 1 : text.length;
  return text.slice(0, cut);
}

function hostTarget() {
  if (process.platform === 'darwin') {
    return process.arch === 'arm64' ? 'apple-macos-arm64' : 'apple-macos-x86_64';
  }
  if (process.platform === 'linux') {
    return process.arch === 'arm64' ? 'linux-arm64' : 'linux-x86_64';
  }
  if (process.platform === 'win32') {
    return process.arch === 'arm64' ? 'windows-arm64' : 'windows-x86_64';
  }
  throw new Error(`no default target for ${process.platform}/${process.arch}`);
}

function sha256(buf) {
  return createHash('sha256').update(buf).digest('hex');
}

// ---- the cache ----------------------------------------------------------------
//
// An entry is trusted when its stamp matches the manifest *and* what a build
// reads out of it is still there: a stamp survives someone deleting a framework
// inside the entry, and trusting it alone reported "cached" and then failed with
// "missing after unpack" on every run, never repairing anything.
//
// Two fetches of one entry at once -- CMake's auto-fetch in one build tree and a
// build script in another -- must not unpack over each other, share a temporary
// archive, or delete a tree the other is still writing. So an entry is fetched
// under its own lock, built in a staging directory, and swapped in only once it
// is complete, retagged and stamped. A second fetch waits for the lock and then
// finds the entry cached.

function cached(dir, want, isWhole = () => true) {
  const stampFile = join(dir, '.verified');
  return existsSync(stampFile) && readFileSync(stampFile, 'utf8').trim() === want && isWhole(dir);
}

function stamp(dir, value) {
  writeFileSync(join(dir, '.verified'), value + '\n');
}

// Every path must exist under the entry.
const hasAll = (...paths) => (dir) => paths.every(path => existsSync(join(dir, path)));

const LOCK_TIMEOUT_MS = Number(process.env.SCREENKIT_FETCH_LOCK_TIMEOUT_MS ?? 30 * 60_000);
const sleep = (ms) => new Promise(resolve => setTimeout(resolve, ms));

// A lock whose owner is gone: its process has exited, or it never got as far as
// writing its pid and the lock is older than any such gap could be.
function lockAbandoned(lock) {
  let pid;
  try {
    pid = Number(readFileSync(join(lock, 'pid'), 'utf8'));
  } catch {
    try {
      return Date.now() - statSync(lock).mtimeMs > 10_000;
    } catch {
      return false;  // released while we looked
    }
  }
  try {
    process.kill(pid, 0);
    return false;
  } catch (err) {
    return err.code === 'ESRCH';
  }
}

// Run `fn` holding `<dir>.lock`. mkdir is the lock: it either creates the
// directory or fails because someone else did, atomically, on every filesystem
// this runs on.
async function withLock(dir, fn) {
  const lock = `${dir}.lock`;
  mkdirSync(dirname(dir), { recursive: true });
  const deadline = Date.now() + LOCK_TIMEOUT_MS;
  let announced = false;
  for (;;) {
    try {
      mkdirSync(lock);
      writeFileSync(join(lock, 'pid'), String(process.pid));
      break;
    } catch (err) {
      if (err.code !== 'EEXIST') throw err;
      if (lockAbandoned(lock)) {
        rmSync(lock, { recursive: true, force: true });
        continue;
      }
      if (Date.now() > deadline) {
        throw new Error(`timed out waiting for ${lock}; if no other fetch is running, delete it`);
      }
      if (!announced) say(`>>> waiting for another fetch of ${dir}`);
      announced = true;
      await sleep(250);
    }
  }
  try {
    return await fn();
  } finally {
    rmSync(lock, { recursive: true, force: true });
  }
}

// Make `dir` a whole, stamped entry: nothing to do when it already is;
// otherwise, under its lock, `produce(staging)` fills a staging directory that
// then replaces the entry.
async function ensureEntry(dir, want, isWhole, label, produce) {
  if (cached(dir, want, isWhole)) {
    say(`>>> ${label}: cached`);
    return;
  }
  await withLock(dir, async () => {
    if (cached(dir, want, isWhole)) {
      say(`>>> ${label}: cached (fetched by another process)`);
      return;
    }
    // Staging directories a crashed fetch left behind. Nobody else can be using
    // one: they are only written under this lock.
    const prefix = `${basename(dir)}.staging-`;
    for (const name of readdirSync(dirname(dir))) {
      if (name.startsWith(prefix)) rmSync(join(dirname(dir), name), { recursive: true, force: true });
    }
    if (existsSync(dir)) say(`>>> ${label}: cache entry is stale or incomplete -- refetching`);
    const staging = join(dirname(dir), `${prefix}${process.pid}`);
    try {
      mkdirSync(staging, { recursive: true });
      await produce(staging);
      stamp(staging, want);
      if (!isWhole(staging)) throw new Error(`${label}: the fetched entry is incomplete -- the asset changed shape`);
      rmSync(dir, { recursive: true, force: true });
      renameSync(staging, dir);
    } finally {
      rmSync(staging, { recursive: true, force: true });
    }
  });
}

// Download, verify against the manifest sha256, unpack. `spec` carries
// { asset, sha256, size, format?, stripComponents? }; format defaults to zip
// because that is what ANGLE ships.
const DOWNLOAD_TIMEOUT_MS = Number(process.env.SCREENKIT_FETCH_TIMEOUT_MS ?? 10 * 60_000);

// Download (or read) `url` and check it against spec.sha256. The bytes, in memory.
async function download(url, spec, label) {
  const mb = (spec.size / 1048576).toFixed(1);
  const local = !/^https?:\/\//.test(url);
  say(`>>> ${label}: ${local ? `reading ${url}` : `downloading ${spec.asset}`} (${mb} MB)`);
  // Without a timeout a stalled connection hangs the build forever with no
  // output, which is indistinguishable from a slow download.
  let buf;
  if (local) {
    // A local file -- a mirror directory, or an archive a recipe just built. Verified exactly
    // like a download.
    buf = readFileSync(url.replace(/^file:\/\//, ''));
  } else {
    let res;
    try {
      res = await fetch(url, { signal: AbortSignal.timeout(DOWNLOAD_TIMEOUT_MS) });
    } catch (err) {
      if (err?.name === 'TimeoutError' || err?.name === 'AbortError') {
        throw new Error(`download timed out after ${DOWNLOAD_TIMEOUT_MS} ms: ${url}`);
      }
      throw err;
    }
    if (!res.ok) throw new Error(`download failed: ${res.status} ${url}`);
    buf = Buffer.from(await res.arrayBuffer());
  }

  const got = sha256(buf);
  if (got !== spec.sha256) {
    throw new Error(`checksum mismatch for ${spec.asset}\n  expected ${spec.sha256}\n  got      ${got}`);
  }
  say(`    sha256 ok`);
  return buf;
}

async function fetchArchive(url, spec, outDir, label) {
  const buf = await download(url, spec, label);

  // Inside the staging directory's parent under a name of its own, so no other
  // fetch can be writing or deleting the same file.
  mkdirSync(outDir, { recursive: true });
  const archive = `${outDir}.${spec.asset}`;
  writeFileSync(archive, buf);
  try {
    const format = spec.format ?? 'zip';
    if (format === 'tar.gz') {
      const strip = spec.stripComponents ?? 0;
      const args = ['-xzf', archive, '-C', outDir];
      if (strip > 0) args.push(`--strip-components=${strip}`);
      execFileSync('tar', args, { stdio: 'inherit' });
    } else if (format === 'dmg') {
      unpackDmg(archive, spec, outDir);
    } else {
      execFileSync('unzip', ['-oq', archive, '-d', outDir]);
    }
  } finally {
    rmSync(archive, { force: true });
  }
  return outDir;
}

// SDL ships its Apple binaries as a .dmg, so unpacking means mounting one.
// Mount read-only, outside the cache (a mount point inside outDir would be in
// the way of the copy), take only the entries the manifest names, detach.
// `ditto` rather than `cp -R`: the frameworks are code-signed, and ditto is the
// one copy on macOS that keeps a signature and its extended attributes intact.
function unpackDmg(archive, spec, outDir) {
  if (process.platform !== 'darwin') {
    throw new Error(
      `${spec.asset} is a .dmg and can only be unpacked on macOS -- this is a ${process.platform} host`);
  }
  const mountParent = mkdtempSync(join(tmpdir(), 'screenkit-dmg-'));
  let mount;
  try {
    const out = execFileSync(
      'hdiutil',
      ['attach', '-nobrowse', '-readonly', '-noverify', '-mountrandom', mountParent, archive],
      { encoding: 'utf8' });
    // hdiutil prints tab-separated columns; the mount point is the last field of
    // the last non-empty line.
    const line = out.trimEnd().split('\n').pop();
    mount = line.split('\t').pop().trim();
    if (!mount || !existsSync(mount)) {
      throw new Error(`could not read a mount point out of hdiutil output:\n${out}`);
    }
    for (const entry of spec.copy ?? []) {
      const from = join(mount, entry);
      if (!existsSync(from)) {
        throw new Error(`${spec.asset} has no "${entry}" -- the asset changed shape`);
      }
      execFileSync('ditto', [from, join(outDir, entry)]);
    }
  } finally {
    if (mount) execFileSync('hdiutil', ['detach', '-quiet', mount]);
    rmSync(mountParent, { recursive: true, force: true });
  }
}

// ---- zip, in memory ------------------------------------------------------------
//
// Android's artifacts are zips -- an .aar is one -- and only a few entries of each
// are wanted: SDL's .aar out of its release zip, libjsi.so out of React Native's
// 164 MB .aar. Reading them out of the verified bytes in memory means the whole
// archive is never written to disk, and no `unzip` is needed. Plain zip only (no
// zip64), which every one of these is.

function zipEntries(buf, label) {
  let eocd = -1;
  for (let i = buf.length - 22; i >= Math.max(0, buf.length - 65557); i--) {
    if (buf.readUInt32LE(i) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error(`${label}: not a zip archive`);
  const count = buf.readUInt16LE(eocd + 10);
  let at = buf.readUInt32LE(eocd + 16);
  if (count === 0xffff || at === 0xffffffff) throw new Error(`${label}: zip64 archives are not supported`);
  const entries = new Map();
  for (let n = 0; n < count; n++) {
    if (buf.readUInt32LE(at) !== 0x02014b50) throw new Error(`${label}: corrupt zip central directory`);
    const nameLength = buf.readUInt16LE(at + 28);
    entries.set(buf.toString('utf8', at + 46, at + 46 + nameLength), {
      method: buf.readUInt16LE(at + 10),
      compressedSize: buf.readUInt32LE(at + 20),
      localOffset: buf.readUInt32LE(at + 42),
    });
    at += 46 + nameLength + buf.readUInt16LE(at + 30) + buf.readUInt16LE(at + 32);
  }
  return entries;
}

// The bytes of `name`, or an error naming the archive: a missing entry means the
// asset changed shape.
function zipRead(buf, entries, name, label) {
  const entry = entries.get(name);
  if (!entry) throw new Error(`${label} has no "${name}" -- the asset changed shape`);
  const at = entry.localOffset;
  if (buf.readUInt32LE(at) !== 0x04034b50) throw new Error(`${label}: corrupt zip entry ${name}`);
  const start = at + 30 + buf.readUInt16LE(at + 26) + buf.readUInt16LE(at + 28);
  const data = buf.subarray(start, start + entry.compressedSize);
  if (entry.method === 0) return data;
  if (entry.method === 8) return inflateRawSync(data);
  throw new Error(`${label}: ${name} uses zip compression method ${entry.method}`);
}

function zipExtract(buf, names, outDir, label) {
  const entries = zipEntries(buf, label);
  for (const name of names) {
    const target = join(outDir, name);
    mkdirSync(dirname(target), { recursive: true });
    writeFileSync(target, zipRead(buf, entries, name, label));
  }
}

// ---- ANGLE ------------------------------------------------------------------

// The headers are pinned to the commit the libraries were built from. Cloning
// the default branch instead gave headers from whatever google/angle's tip was
// on the day of the fetch -- measured 22 files away from the libraries, including
// eglext_angle.h, gl2ext.h and gl3.h, which is what we compile against. A commit
// id is content-addressed, so fetching by it is the verification: git refuses
// objects that do not hash to what they claim.
//
// The cache is trusted only when HEAD is that commit and include/ is unmodified,
// so a checkout left behind by the old unpinned fetch is replaced, not reused.
function angleHeadersAtRef(dir) {
  if (!existsSync(join(dir, 'include', 'EGL', 'egl.h'))) return false;
  try {
    const head = execFileSync('git', ['-C', dir, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
    if (head !== angle.headers.ref) return false;
    execFileSync('git', ['-C', dir, 'diff', '--quiet', 'HEAD', '--', 'include']);
    return true;
  } catch {
    return false;
  }
}

async function fetchAngleHeaders() {
  const dir = join(ANGLE_BASE, 'include-src');
  const { ref, repo, sparse } = angle.headers;
  if (!/^[0-9a-f]{40}$/.test(ref ?? '')) {
    throw new Error('manifest angle.headers.ref must be a full 40-hex commit id, not a branch or tag');
  }
  // A checkout from before the stamp existed is at the ref but unstamped; give
  // it the stamp CMake reads rather than refetching it.
  if (angleHeadersAtRef(dir) && !cached(dir, ref)) {
    await withLock(dir, async () => {
      if (angleHeadersAtRef(dir)) stamp(dir, ref);
    });
  }
  await ensureEntry(dir, ref, angleHeadersAtRef, `ANGLE headers @ ${ref.slice(0, 12)}`, async (staging) => {
    say(`>>> ANGLE headers @ ${ref.slice(0, 12)} (sparse checkout of ${sparse.join(', ')})`);
    const git = (...args) => execFileSync('git', ['-C', staging, ...args], { stdio: 'inherit' });
    git('init', '--quiet');
    git('remote', 'add', 'origin', repo);
    git('sparse-checkout', 'set', ...sparse);
    git('fetch', '--quiet', '--depth', '1', '--filter=blob:none', 'origin', ref);
    git('-c', 'advice.detachedHead=false', 'checkout', '--quiet', ref);
    if (!angleHeadersAtRef(staging)) {
      throw new Error(`ANGLE headers checkout did not land on ${ref}`);
    }
  });
  return join(dir, 'include');
}

// The three archives AnglePrebuilt.cmake links, by the prefix it globs for:
// Godot names each per slice (libANGLE.macos.arm64.a, ...).
function angleLibsPresent(dir) {
  let names;
  try {
    names = readdirSync(dir);
  } catch {
    return false;
  }
  return ['libEGL.', 'libGLES.', 'libANGLE.'].every(prefix =>
    names.some(name => name.startsWith(prefix) && name.endsWith('.a')));
}

async function fetchAngleTarget(key) {
  const spec = angle.targets[key];
  if (!spec) {
    const why = angle.unavailable[key];
    if (why) throw new Error(`angle target "${key}" is unavailable: ${why}`);
    throw new Error(`unknown angle target "${key}" (try --list)`);
  }

  const outDir = join(ANGLE_BASE, key);
  const url = `https://github.com/${angle.source}/releases/download/${angle.version}/${spec.asset}`;
  await ensureEntry(outDir, spec.sha256, angleLibsPresent, `angle ${key}`, async (staging) => {
    await fetchArchive(url, spec, staging, `angle ${key}`);
    if (spec.retag) {
      say(`    retagging ${spec.retag.from} -> ${spec.retag.to}`);
      // readdirSync, not `sh -c "ls <dir>/*.a"`: the cache root is user-supplied
      // via SCREENKIT_PREBUILTS and a space or a glob character in it would
      // either break the command or silently match the wrong files.
      const libs = readdirSync(staging)
        .filter(name => name.endsWith('.a'))
        .map(name => join(staging, name));
      if (libs.length === 0) {
        throw new Error(`no .a archives in ${staging} to retag -- the ${key} asset changed shape`);
      }
      execFileSync('python3', [join(HERE, 'retag-macho.py'),
                               '--from', spec.retag.from, '--to', spec.retag.to, ...libs],
                   { stdio: 'inherit' });
    }
  });
  return outDir;
}

// ---- SDL3 -------------------------------------------------------------------
//
// Same shape as Hermes: every Apple target is a slice of ONE archive, so the
// cache holds a single unpacked SDL3.xcframework at <version>/apple/ and a
// target key only selects which framework inside it the build links.

const SDL3_APPLE_DIR = join(SDL3_BASE, 'apple');
const SDL3_ANDROID_DIR = join(SDL3_BASE, 'android');

function sdl3Url(template) {
  return template.replaceAll('{version}', sdl3.version);
}

async function fetchSdl3Target(key) {
  const spec = sdl3.targets[key];
  if (!spec) {
    const why = sdl3.unavailable[key];
    if (why) throw new Error(`sdl3 target "${key}" is unavailable: ${why}`);
    throw new Error(`unknown sdl3 target "${key}" (try --list)`);
  }
  if (spec.aar) return fetchSdlAndroid(sdl3, SDL3_ANDROID_DIR, 'sdl3');

  const art = sdl3.apple;
  await ensureEntry(SDL3_APPLE_DIR, art.sha256, hasAll(join(spec.framework, 'SDL3')), `sdl3 ${sdl3.version}`,
                    (staging) => fetchArchive(sdl3Url(art.url), art, staging, `sdl3 ${sdl3.version}`));

  const framework = join(SDL3_APPLE_DIR, spec.framework);
  if (!existsSync(framework)) {
    throw new Error(`sdl3 slice missing after unpack: ${framework}`);
  }
  say(`    ${key}: ${spec.slice}`);
  return framework;
}

// SDL3_ttf ships exactly the way SDL3 does: one .dmg, one xcframework, a
// target key picking the slice. Its frameworks link SDL3 through @rpath, so they
// load whichever SDL3 the host carries.
const SDL3_TTF_APPLE_DIR = join(SDL3_TTF_BASE, 'apple');
const SDL3_TTF_ANDROID_DIR = join(SDL3_TTF_BASE, 'android');

async function fetchSdl3TtfTarget(key) {
  const spec = sdl3Ttf.targets[key];
  if (!spec) {
    const why = sdl3Ttf.unavailable[key];
    if (why) throw new Error(`sdl3_ttf target "${key}" is unavailable: ${why}`);
    throw new Error(`unknown sdl3_ttf target "${key}" (try --list)`);
  }
  if (spec.aar) return fetchSdlAndroid(sdl3Ttf, SDL3_TTF_ANDROID_DIR, 'sdl3_ttf');

  const art = sdl3Ttf.apple;
  const label = `sdl3_ttf ${sdl3Ttf.version}`;
  await ensureEntry(SDL3_TTF_APPLE_DIR, art.sha256, hasAll(join(spec.framework, 'SDL3_ttf')), label,
                    (staging) => fetchArchive(art.url.replaceAll('{version}', sdl3Ttf.version), art, staging, label));

  const framework = join(SDL3_TTF_APPLE_DIR, spec.framework);
  if (!existsSync(framework)) {
    throw new Error(`sdl3_ttf slice missing after unpack: ${framework}`);
  }
  say(`    ${key}: ${spec.slice}`);
  return framework;
}

// Android: SDL and SDL_ttf each publish one release zip holding one .aar, every
// ABI inside it, so a target key picks nothing -- both Android targets are that
// file. Only the .aar and the licence come out of the zip, into <version>/android/;
// Gradle takes the .aar from there (runtime/android/app/build.gradle).
async function fetchSdlAndroid(dep, dir, name) {
  const art = dep.android;
  const label = `${name} ${dep.version} android`;
  await ensureEntry(dir, art.sha256, hasAll(art.aar), label, async (staging) => {
    zipExtract(await download(art.url.replaceAll('{version}', dep.version), art, label), art.copy, staging, art.asset);
  });
  say(`    android: ${art.aar}`);
  return join(dir, art.aar);
}

// ---- Hermes -----------------------------------------------------------------
//
// Every Apple target is a slice of ONE archive (destroot/ holds the macOS
// framework plus the universal xcframework), so the runtime unpacks once into
// <version>/apple/ and a target key only selects which framework path inside it
// the build should link. manifest.json owns that mapping.

const HERMES_RUNTIME_DIR = join(HERMES_BASE, 'apple');
const HERMES_COMPILER_DIR = join(HERMES_BASE, 'compiler');

function hermesUrl(template) {
  return template.replaceAll('{version}', hermes.version);
}

async function fetchHermesRuntime(key) {
  const spec = hermes.targets[key];
  if (!spec) {
    const why = hermes.unavailable[key];
    if (why) throw new Error(`hermes target "${key}" is unavailable: ${why}`);
    throw new Error(`unknown hermes target "${key}" (try --list)`);
  }
  if (spec.archive) return fetchHermesLinux(key, spec);
  if (spec.aar) return fetchHermesAndroid(key, spec);

  await fetchHermesApple(join(spec.framework, 'hermesvm'));

  const framework = join(HERMES_RUNTIME_DIR, spec.framework);
  if (!existsSync(framework)) {
    throw new Error(`hermes slice missing after unpack: ${framework}`);
  }
  say(`    ${key}: ${spec.slice}`);
  return framework;
}

// The Apple destroot, trusted while `paths` (and the headers) are in it.
async function fetchHermesApple(...paths) {
  const art = hermes.runtime;
  await ensureEntry(HERMES_RUNTIME_DIR, art.sha256, hasAll(...paths, join(hermes.include, 'hermes', 'hermes.h')),
                    `hermes ${hermes.version}`,
                    (staging) => fetchArchive(hermesUrl(art.url), art, staging, `hermes ${hermes.version}`));
}

// ---- Hermes on Android ----------------------------------------------------------
//
// <version>/android/ holds what the Android build takes, both ABIs together:
//
//   hermes-android-<version>-release.aar   as published; Gradle unpacks its prefab
//   include/jsi/                           the JSI headers, which the prefab excludes --
//                                          from this Hermes's Apple destroot
//   jni/<abi>/libjsi.so                    the JSI implementation hermesvm links on
//                                          Android, out of React Native's .aar
//
// Three sources, one entry: the stamp is all three sha256s, so moving any pin
// refetches it.

const HERMES_ANDROID_DIR = join(HERMES_BASE, 'android');

async function fetchHermesAndroid(key, spec) {
  const art = hermes.android;
  const jsi = art.jsi;
  const label = `hermes ${hermes.version} android`;
  const want = [art.sha256, jsi.sha256, hermes.runtime.sha256].join('+');
  const whole = hasAll(art.asset, join(spec.include, 'jsi', 'jsi.h'), ...Object.keys(jsi.extract));
  if (!cached(HERMES_ANDROID_DIR, want, whole)) {
    await fetchHermesApple(join(hermes.include, 'jsi', 'jsi.h'));
  }
  await ensureEntry(HERMES_ANDROID_DIR, want, whole, label, async (staging) => {
    writeFileSync(join(staging, art.asset), await download(hermesUrl(art.url), art, label));

    const reactNative = await download(jsi.url, jsi, `${label}: libjsi.so`);
    const entries = zipEntries(reactNative, jsi.asset);
    for (const [name, sum] of Object.entries(jsi.extract)) {
      const data = zipRead(reactNative, entries, name, jsi.asset);
      if (sha256(data) !== sum) {
        throw new Error(`checksum mismatch for ${name} in ${jsi.asset}\n  expected ${sum}\n  got      ${sha256(data)}`);
      }
      mkdirSync(dirname(join(staging, name)), { recursive: true });
      writeFileSync(join(staging, name), data);
    }

    cpSync(join(HERMES_RUNTIME_DIR, hermes.include, 'jsi'), join(staging, spec.include, 'jsi'), { recursive: true });
  });
  say(`    ${key}: ${spec.slice}`);
  return join(HERMES_ANDROID_DIR, art.asset);
}

// ---- Hermes on Linux ------------------------------------------------------------
//
// No upstream publishes a Linux runtime, so these archives are ours:
// recipes/hermes-linux/build.sh builds them from hermes.linux.commit and manifest.json pins each
// by sha256. Where one comes from, first match wins:
//
//   1. hermes.linux.url, once the archives are hosted ({asset} is the file name);
//   2. $SCREENKIT_PREBUILTS_MIRROR/<asset> -- a directory or a base URL;
//   3. tools/prebuilts/dist/<asset>, where the recipe writes them.
//
// Every source is checked against the same sha256, so a rebuilt archive that differs from the
// pinned one is refused rather than silently swapped in.

class Unpublished extends Error {}

function hermesLinuxSource(asset) {
  if (hermes.linux.url) return hermes.linux.url.replaceAll('{asset}', asset).replaceAll('{version}', hermes.version);
  const mirror = process.env.SCREENKIT_PREBUILTS_MIRROR;
  if (mirror) return /^https?:\/\//.test(mirror) ? `${mirror.replace(/\/$/, '')}/${asset}` : join(mirror, asset);
  const built = join(HERE, 'dist', asset);
  if (existsSync(built)) return built;
  throw new Unpublished(
    `${asset} is not published anywhere yet. Build it -- tools/prebuilts/recipes/hermes-linux/build.sh -- ` +
    'or point SCREENKIT_PREBUILTS_MIRROR at a directory or URL that has it.');
}

async function fetchHermesLinux(key, spec) {
  const dir = join(HERMES_BASE, key);
  const label = `hermes ${hermes.version} ${key}`;
  const art = { ...spec.archive, format: 'tar.gz', stripComponents: 1 };
  await ensureEntry(dir, art.sha256, hasAll(spec.lib, join(spec.include, 'hermes', 'hermes.h')), label,
                    (staging) => fetchArchive(hermesLinuxSource(art.asset), art, staging, label));
  say(`    ${key}: ${spec.slice}`);
  return join(dir, spec.lib);
}

// hermesc ships in the npm package `hermes-compiler`, versioned identically to
// the engine -- that identity is the whole point, since a .hbc from a different
// hermesc is refused by the loader.
async function fetchHermesCompiler() {
  const spec = hermes.compiler;
  const rel = spec.hosts[process.platform];
  if (!rel) throw new Error(`no hermesc for ${process.platform} in ${spec.package}`);

  await ensureEntry(HERMES_COMPILER_DIR, spec.sha256, hasAll(rel), `${spec.package} ${hermes.version}`,
                    (staging) => fetchArchive(hermesUrl(spec.url), spec, staging,
                                              `${spec.package} ${hermes.version}`));

  const bin = join(HERMES_COMPILER_DIR, rel);
  if (!existsSync(bin)) throw new Error(`hermesc missing after unpack: ${bin}`);
  return bin;
}

// ---- CLI --------------------------------------------------------------------

const args = process.argv.slice(2);
const flag = (name) => args.includes(`--${name}`);
function opt(name) {
  const i = args.indexOf(`--${name}`);
  if (i >= 0 && args[i + 1] && !args[i + 1].startsWith('--')) return args[i + 1];
  const eq = args.find(a => a.startsWith(`--${name}=`));
  return eq ? eq.slice(name.length + 3) : undefined;
}

if (flag('list')) {
  console.log(`angle ${angle.version}  (${angle.source})\n`);
  for (const [k, v] of Object.entries(angle.targets)) {
    console.log(`  ${k.padEnd(30)} ${v.retag ? `derived: retag -> ${v.retag.to}` : ''}`);
  }
  console.log('\n  unavailable:');
  for (const [k, why] of Object.entries(angle.unavailable)) {
    printReason(k, why);
  }

  console.log(`\nsdl3 ${sdl3.version}  (${sdl3.apple.source})`);
  console.log(`  one ${sdl3.apple.format} -- SDL3.xcframework, sliced per target; one .aar for Android:\n`);
  for (const [k, v] of Object.entries(sdl3.targets)) {
    console.log(`  ${k.padEnd(30)} ${v.slice}`);
  }
  console.log('\n  unavailable:');
  for (const [k, why] of Object.entries(sdl3.unavailable)) {
    printReason(k, why);
  }

  console.log(`\nsdl3_ttf ${sdl3Ttf.version}  (${sdl3Ttf.apple.source})`);
  console.log(`  one ${sdl3Ttf.apple.format} -- SDL3_ttf.xcframework, sliced per target; one .aar for Android:\n`);
  for (const [k, v] of Object.entries(sdl3Ttf.targets)) {
    console.log(`  ${k.padEnd(30)} ${v.slice}`);
  }
  console.log('\n  unavailable:');
  for (const [k, why] of Object.entries(sdl3Ttf.unavailable)) {
    printReason(k, why);
  }

  console.log(`\nhermes ${hermes.version}  (${hermes.runtime.source})`);
  console.log(`  hbc bytecode version ${hermes.bytecodeVersion} · compiler: npm ${hermes.compiler.package}@${hermes.version}`);
  console.log(`  one archive per platform family, sliced per target (Android: ${hermes.android.source}):\n`);
  for (const [k, v] of Object.entries(hermes.targets)) {
    console.log(`  ${k.padEnd(30)} ${v.slice}`);
  }
  console.log('\n  unavailable:');
  for (const [k, why] of Object.entries(hermes.unavailable)) {
    printReason(k, why);
  }
  process.exit(0);
}

if (flag('hermesc')) {
  const bin = await fetchHermesCompiler();
  say(`\nhermesc: ${bin}`);
  console.log(bin);                                  // stdout: the path alone
  process.exit(0);
}

const DEPS = { angle, sdl3, sdl3_ttf: sdl3Ttf, hermes };

const dep = opt('dep');
if (dep && !DEPS[dep]) {
  throw new Error(`unknown dep "${dep}" (${Object.keys(DEPS).join(' | ')})`);
}

const explicit = args.filter(a => !a.startsWith('--') && a !== dep);
const targets = flag('all')
  ? [...new Set(Object.values(DEPS).flatMap(d => Object.keys(d.targets)))]
  : (explicit.length ? explicit : [hostTarget()]);

// When several deps are fetched at once, a target one dep does not publish is
// skipped with its reason rather than failing the run -- Hermes has no Linux or
// Windows runtime, ANGLE has no Android one. Asking for a dep by name is exact.
async function run(name, targetsForDep, fn) {
  for (const t of targetsForDep) {
    if (!dep && !DEPS[name].targets[t]) {
      const why = DEPS[name].unavailable[t];
      say(`>>> ${name} ${t}: skipped -- ${why ? firstSentence(why) : 'not published'}`);
      continue;
    }
    try {
      await fn(t);
    } catch (err) {
      // A target we build ourselves and have not published yet is a skip in a broad run, and
      // the error it is when asked for by name.
      if (dep || !(err instanceof Unpublished)) throw err;
      say(`>>> ${name} ${t}: skipped -- ${firstSentence(err.message)}`);
    }
  }
}

if (dep === undefined || dep === 'angle') {
  const include = await fetchAngleHeaders();
  await run('angle', targets, fetchAngleTarget);
  say(`\nangle headers: ${include}`);
  say(`angle libs:    ${ANGLE_BASE}/<target>/`);
}

if (dep === undefined || dep === 'sdl3') {
  await run('sdl3', targets, fetchSdl3Target);
  if (targets.some(t => sdl3.targets[t]?.framework)) {
    say(`\nsdl3 xcframework: ${join(SDL3_APPLE_DIR, 'SDL3.xcframework')}`);
  }
  if (targets.some(t => sdl3.targets[t]?.aar)) {
    say(`sdl3 aar:         ${join(SDL3_ANDROID_DIR, sdl3.android.aar)}`);
  }
}

if (dep === undefined || dep === 'sdl3_ttf') {
  await run('sdl3_ttf', targets, fetchSdl3TtfTarget);
  if (targets.some(t => sdl3Ttf.targets[t]?.framework)) {
    say(`\nsdl3_ttf xcframework: ${join(SDL3_TTF_APPLE_DIR, 'SDL3_ttf.xcframework')}`);
  }
  if (targets.some(t => sdl3Ttf.targets[t]?.aar)) {
    say(`sdl3_ttf aar:         ${join(SDL3_TTF_ANDROID_DIR, sdl3Ttf.android.aar)}`);
  }
}

if (dep === undefined || dep === 'hermes') {
  await run('hermes', targets, fetchHermesRuntime);
  if (targets.some(t => hermes.targets[t]?.framework)) {
    say(`\nhermes headers:    ${join(HERMES_RUNTIME_DIR, hermes.include)}`);
    say(`hermes frameworks: ${join(HERMES_RUNTIME_DIR, 'destroot', 'Library', 'Frameworks')}/`);
  }
  if (targets.some(t => hermes.targets[t]?.aar)) {
    say(`hermes android:    ${HERMES_ANDROID_DIR}/`);
  }
  say(`hermesc:           ${await fetchHermesCompiler()}`);
}
