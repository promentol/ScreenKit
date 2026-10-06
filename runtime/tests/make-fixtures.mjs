#!/usr/bin/env node
// Build the bytecode fixtures the runtime tests need.
//
//   node runtime/tests/make-fixtures.mjs <hermesc> <out-dir>
//
// Output goes to the build tree, never the source tree: a .hbc is only
// meaningful next to the Hermes it was pinned against, so it is derived, not
// committed. Three of the seven outputs are deliberately broken
// (version-mismatch.hbc, truncated.hbc, stub.hbc) plus one unparseable source
// (garbage.js) -- they exist to prove the loader refuses them before the VM
// ever sees them.

import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { deflateSync } from 'node:zlib';
import { copyFileSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

// The packer `screenkit bundle` uses, so the packed-entry packages below carry
// the real wrapper -- a change to how it handles a rejected entry changes them.
import { packScript, readLegacyLayout } from '../../packages/@screenkit/cli/src/pack.js';

const [hermesc, outDir] = process.argv.slice(2);
if (!hermesc || !outDir) {
  console.error('usage: make-fixtures.mjs <hermesc> <out-dir>');
  process.exit(64);
}

const SRC = join(dirname(fileURLToPath(import.meta.url)), 'fixtures');
mkdirSync(outDir, { recursive: true });

// The HBC header: 8-byte magic, then a uint32 bytecode version.
const VERSION_OFFSET = 8;

for (const name of readdirSync(SRC).filter(f => f.endsWith('.js'))) {
  const stem = name.slice(0, -3);
  copyFileSync(join(SRC, name), join(outDir, name));          // the source-eval path
  // -Xes6-block-scoping: real `let`/`const`, matching the runtime's source path.
  execFileSync(hermesc, ['-emit-binary', '-O', '-Xes6-block-scoping',
                         '-out', join(outDir, `${stem}.hbc`), join(SRC, name)], { stdio: 'inherit' });
}

const hello = readFileSync(join(outDir, 'hello.hbc'));

// A .hbc from another Hermes: same magic, a version this engine does not speak.
// Patching the field is exactly what a stale build produces, and it is the only
// way to get one without keeping a second Hermes around.
const mismatched = Buffer.from(hello);
const realVersion = mismatched.readUInt32LE(VERSION_OFFSET);
mismatched.writeUInt32LE(realVersion + 1, VERSION_OFFSET);
writeFileSync(join(outDir, 'version-mismatch.hbc'), mismatched);

// A truncated .hbc: header intact, body cut off. isHermesBytecode still says
// yes -- it only reads magic and version -- so this is what hermesBytecodeSanityCheck
// is for.
writeFileSync(join(outDir, 'truncated.hbc'), hello.subarray(0, 64));

// Truncated so hard the version field itself is incomplete: 10 bytes, magic
// intact. The loader must still recognise this as bytecode and refuse it, not
// shrug and hand it to the parser as "source".
writeFileSync(join(outDir, 'stub.hbc'), hello.subarray(0, 10));

// Not bytecode and not valid source either.
writeFileSync(join(outDir, 'garbage.js'), 'this is ( not javascript\n');


// Assets for the DOM shim's loader rows. Generated, like every fixture, so a
// test never passes against a stale checked-in copy. The PNG is a known flat
// colour so a texture upload can be proven by reading one pixel back.
const assets = join(outDir, 'assets');
mkdirSync(join(assets, 'img'), { recursive: true });
mkdirSync(join(outDir, 'assets-evil'), { recursive: true });

function crc32(buf) {
  let c, crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
// An RGBA PNG whose pixel (x, y) is `rgba(x, y)`.
function png(w, h, rgba) {
  const rows = [];
  for (let y = 0; y < h; y++) {
    const row = [0];
    for (let x = 0; x < w; x++) row.push(...rgba(x, y));
    rows.push(Buffer.from(row));
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr), chunk('IDAT', deflateSync(Buffer.concat(rows))),
    chunk('IEND', Buffer.alloc(0))]);
}
function flatPng(w, h, color) {
  return png(w, h, () => color);
}
writeFileSync(join(assets, 'img', 'tex.png'), flatPng(17, 9, [200, 60, 90, 255]));
// A sprite sheet in miniature, for the crop rows: every pixel is its own colour,
// (x, y) -> 10 + 60x, 20 + 100y, 7, 255, so a crop reads back as coordinates.
writeFileSync(join(assets, 'img', 'sheet.png'), png(4, 2, (x, y) => [10 + 60 * x, 20 + 100 * y, 7, 255]));
// Large enough that decoding it takes longer than a 0 ms timer: the
// dom-image-decode-async row tells a decode off the JS thread from one on it.
writeFileSync(join(assets, 'img', 'large.png'), flatPng(2048, 2048, [30, 60, 90, 255]));
// Half-transparent, for the premultiplied-alpha rows: stored straight, it reads
// back as 200,100,50,128; premultiplied, as 100,50,25,128 (Chromium's rounding).
writeFileSync(join(assets, 'img', 'alpha.png'), flatPng(1, 1, [200, 100, 50, 128]));
writeFileSync(join(assets, 'hello.txt'), 'hi from an asset');
writeFileSync(join(assets, 'utf8.txt'), 'caf\u00e9 \u2713 \u65e5\u672c');
writeFileSync(join(assets, 'data.json'), JSON.stringify({ shim: 'loader', n: 3 }));
writeFileSync(join(outDir, 'assets-evil', 'secret.txt'), 'SHOULD NOT BE READABLE');


// .skpkg packages for the host's package rows. One that runs, and one per way
// the host must refuse a package before evaluating anything in it -- each of
// those still carries a valid entry, so a host that skipped the gate would run
// it and print the entry's line, which the refusal rows assert is absent.
//
// runtimeVersion comes from runtime/VERSION, the file the host is compiled
// against, exactly as `screenkit bundle` writes it.
const runtimeVersion = Number(readFileSync(join(dirname(fileURLToPath(import.meta.url)), '..', 'VERSION'), 'utf8').trim());
const packages = join(outDir, 'packages');
rmSync(packages, { recursive: true, force: true });

function writePackage(name, { manifest, manifestText, withEntry = true, entry = 'package-entry.hbc' }) {
  const dir = join(packages, `${name}.skpkg`);
  mkdirSync(join(dir, 'fonts'), { recursive: true });
  // `entry` names which compiled fixture becomes this package's app.hbc: the
  // plain one that prints a line for the gate rows, or a real second app for the
  // `iframe-*` rows to embed.
  if (withEntry) copyFileSync(join(outDir, entry), join(dir, 'app.hbc'));
  writeFileSync(join(dir, 'fonts', 'probe.txt'), `inside ${name}.skpkg`);
  const files = {};
  for (const file of withEntry ? ['app.hbc', 'fonts/probe.txt'] : ['fonts/probe.txt']) {
    files[file] = createHash('sha256').update(readFileSync(join(dir, file))).digest('hex');
  }
  if (manifest || manifestText !== undefined) {
    writeFileSync(join(dir, 'manifest.json'),
                  manifestText ?? JSON.stringify({ ...manifest, files }, null, 2) + '\n');
  }
}

const good = { format: 1, runtimeVersion, hermesBytecodeVersion: realVersion, entry: 'app.hbc' };
writePackage('hello', { manifest: good });
writePackage('bytecode-mismatch', { manifest: { ...good, hermesBytecodeVersion: realVersion + 1 } });
writePackage('runtime-too-new', { manifest: { ...good, runtimeVersion: runtimeVersion + 1 } });
writePackage('missing-entry', { manifest: good, withEntry: false });
writePackage('entry-escapes', { manifest: { ...good, entry: '../../hello.hbc' } });
writePackage('bad-manifest', { manifestText: '{ "format": 1, "entry": "app.hbc", ' });
writePackage('no-manifest', {});
writePackage('format-2', { manifest: { ...good, format: 2 } });
writePackage('not-an-object', { manifestText: '[1, "app.hbc"]\n' });
writePackage('runtime-zero', { manifest: { ...good, runtimeVersion: 0 } });
writePackage('entry-directory', { manifest: { ...good, entry: 'fonts' } });
// The app a launcher embeds in an <iframe>: a real second app that paints, keeps
// counters and talks to its parent (fixtures/iframe-child.js).
writePackage('iframe-child', { manifest: good, entry: 'iframe-child.hbc' });
// A .skpkg that is a file, not a directory: runnable bytecode under a package's
// name, which the host must refuse as a package rather than run as a bundle.
copyFileSync(join(outDir, 'package-entry.hbc'), join(packages, 'file.skpkg'));

// Packages whose entry module fails, packed by the real packer from a small
// plugin-legacy-shaped dist. Evaluating app.hbc succeeds in both -- the failure
// is a rejected System.import that settles afterwards, which only the packed
// wrapper's reportFailure turns into a host exit.
//
//   entry-throws  execute throws at once
//   entry-rejects execute keeps an interval running (so the app never goes
//                 idle) and rejects 200 ms later
const MINI_SYSTEM = `(function (global) {
  var last = null, modules = {};
  global.System = {
    register: function (deps, declare) { last = [deps, declare]; },
    getRegister: function () { var r = last; last = null; return r; },
    instantiate: function (url) { return Promise.reject(new Error('no loader for ' + url)); },
    import: function (url) {
      if (!modules[url]) {
        modules[url] = Promise.resolve(global.System.instantiate(url)).then(function (reg) {
          var decl = reg[1](function () {}, { import: global.System.import, meta: { url: url } });
          return Promise.resolve(decl.execute && decl.execute());
        });
      }
      return modules[url];
    }
  };
})(globalThis);
`;
function writePackedPackage(name, executeBody) {
  const dist = join(outDir, 'packed-dists', name);
  rmSync(dist, { recursive: true, force: true });
  mkdirSync(join(dist, 'assets'), { recursive: true });
  writeFileSync(join(dist, 'index.html'),
    '<script id="vite-legacy-polyfill" src="/assets/polyfills-legacy-P.js"></script>\n' +
    '<script id="vite-legacy-entry" data-src="/assets/index-legacy-A.js"></script>\n');
  writeFileSync(join(dist, 'assets', 'polyfills-legacy-P.js'), MINI_SYSTEM);
  writeFileSync(join(dist, 'assets', 'index-legacy-A.js'),
    `System.register([], function () {\n  return { execute: function () {\n${executeBody}\n  } };\n});\n`);

  const pkg = join(packages, `${name}.skpkg`);
  mkdirSync(pkg, { recursive: true });
  const script = join(dist, 'app.js');
  writeFileSync(script, packScript(dist, readLegacyLayout(dist)).script);
  execFileSync(hermesc, ['-emit-binary', '-O', '-Xes6-block-scoping', '-out', join(pkg, 'app.hbc'), script],
               { stdio: 'inherit' });
  const files = { 'app.hbc': createHash('sha256').update(readFileSync(join(pkg, 'app.hbc'))).digest('hex') };
  writeFileSync(join(pkg, 'manifest.json'), JSON.stringify({ ...good, files }, null, 2) + '\n');
}
writePackedPackage('entry-throws',
  "    console.log('entry started');\n    throw new Error('boom from the entry');");
writePackedPackage('entry-rejects',
  "    console.log('entry started');\n" +
  "    setInterval(function () {}, 1000);\n" +
  "    return new Promise(function (resolve, reject) {\n" +
  "      setTimeout(function () { reject(new Error('late boom from the entry')); }, 200);\n" +
  "    });");

console.log(`fixtures in ${outDir} (hbc version ${realVersion}, mismatch fixture ${realVersion + 1}, runtimeVersion ${runtimeVersion})`);
