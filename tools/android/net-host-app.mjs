#!/usr/bin/env node
// A one-page .skpkg for the `host` row of `tools/android/android.sh test`.
//
//   node tools/android/net-host-app.mjs <hermesc> <out-dir> <http-base>
//
// Every other row runs inside libscreenkit-net-tests.so, which has a JNI_OnLoad
// of its own -- so nothing there executes the *shipping* host's, in
// runtime/android/jni/HostMain.cpp, and deleting that wiring would leave the
// suite green while every real app lost networking. This package is what closes
// that: it runs in ScreenKitActivity, on libscreenkit.so, fetches the fixture
// over the adb-reversed port and says in logcat what it got.
//
// Built here rather than kept in poc/: it is three lines of JS, it has to carry
// the port the fixture happened to bind to, and a .hbc only means anything next
// to the Hermes that compiled it.
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const [hermesc, outDir, base] = process.argv.slice(2);
if (!hermesc || !outDir || !base) {
  console.error('usage: net-host-app.mjs <hermesc> <out-dir> <http-base>');
  process.exit(64);
}

const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');

// One line per outcome, and `window.close()` either way so the activity finishes
// instead of sitting on screen until the row times out.
const source = `
var base = ${JSON.stringify(base)};
function say(line) { console.log('host-net ' + line); }
fetch(base + '/data.json').then(function (r) {
  return r.json().then(function (j) { say('ok ' + r.status + ' ' + j.n); });
}, function (e) {
  say('FAIL ' + (e && e.name) + ' ' + (e && e.cause) + ' ' + (e && e.message));
}).then(function () { window.close(); }, function () { window.close(); });
`;

rmSync(outDir, { recursive: true, force: true });
mkdirSync(outDir, { recursive: true });
const entryJs = join(outDir, 'app.js');
writeFileSync(entryJs, source);
// -Xes6-block-scoping, as every compile in the tree: see runtime/CMakeLists.txt.
execFileSync(hermesc, ['-emit-binary', '-O', '-Xes6-block-scoping',
                       '-out', join(outDir, 'app.hbc'), entryJs], { stdio: 'inherit' });
rmSync(entryJs);

const hbc = readFileSync(join(outDir, 'app.hbc'));
// The HBC header: 8-byte magic, then a uint32 bytecode version. Read from the
// artifact rather than pinned anywhere, so this cannot drift from the hermesc
// that just ran (runtime/tests/make-fixtures.mjs does the same).
const bytecodeVersion = hbc.readUInt32LE(8);

writeFileSync(join(outDir, 'manifest.json'), JSON.stringify({
  format: 1,
  runtimeVersion: Number(readFileSync(join(root, 'runtime', 'VERSION'), 'utf8').trim()),
  hermesBytecodeVersion: bytecodeVersion,
  entry: 'app.hbc',
  files: { 'app.hbc': createHash('sha256').update(hbc).digest('hex') },
}, null, 2) + '\n');
