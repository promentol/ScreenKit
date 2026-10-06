#!/usr/bin/env node
// Runtime counterpart to analyze.mjs.
//
// Static analysis follows host values into variables but not into object
// properties or parameters, so anything a bundle stashes on `this` disappears
// from that report -- for Lightning that is the entire WebGL surface. This runs
// the app for real in Chromium with recording proxies installed *before* any
// app code, so what gets reported is what actually executed.
//
//   node tools/dom-usage/trace.mjs <url> [--seconds 6] [--json]
//
// Trade-off, stated plainly: this sees only the paths that ran. Static analysis
// over-approximates, this under-approximates. They are complements.

import { readFileSync } from 'node:fs';
import { chromium } from 'playwright';

const args = process.argv.slice(2);
const url = args.find((a) => !a.startsWith('--'));
const asJson = args.includes('--json');
const seconds = Number(args[args.indexOf('--seconds') + 1]) || 6;

if (!url) {
  console.error('usage: trace.mjs <url> [--seconds N] [--from static.json] [--json]');
  process.exit(2);
}

// Runs in the page before anything else. Everything here is stringified, so it
// cannot close over anything from this file.
const recorder = (names) => {
  const hits = Object.create(null);
  const argsSeen = Object.create(null);
  const note = (type, prop, callArgs) => {
    const key = type + '.' + String(prop);
    hits[key] = (hits[key] || 0) + 1;
    if (callArgs && callArgs.length) {
      const strs = callArgs.filter((a) => typeof a === 'string');
      if (strs.length) {
        argsSeen[key] = argsSeen[key] || new Set();
        for (const s of strs) argsSeen[key].add(s);
      }
    }
  };

  // Instrument the PROTOTYPE, never the instance.
  //
  // Wrapping instances in a Proxy was the obvious first move and it is wrong:
  // Web IDL validates internal slots, so a proxied Blob fails
  // `URL.createObjectURL` overload resolution and the app dies before it draws.
  // Replacing prototype members leaves every object genuinely itself, and
  // catches calls no matter where the value is stored -- which is the whole
  // reason this exists, since Lightning keeps its context on `this.gl`.
  const instrument = (typeName, proto) => {
    if (!proto) return;
    for (const key of Object.getOwnPropertyNames(proto)) {
      if (key === 'constructor') continue;
      let d;
      try { d = Object.getOwnPropertyDescriptor(proto, key); } catch { continue; }
      if (!d || !d.configurable) continue;
      try {
        if (typeof d.value === 'function') {
          const orig = d.value;
          Object.defineProperty(proto, key, {
            configurable: true, enumerable: d.enumerable, writable: true,
            value: function (...callArgs) {
              note(typeName, key, callArgs);
              return orig.apply(this, callArgs);
            },
          });
        } else if (d.get || d.set) {
          const g = d.get;
          const st = d.set;
          Object.defineProperty(proto, key, {
            configurable: true, enumerable: d.enumerable,
            get: g ? function () { note(typeName, key); return g.call(this); } : undefined,
            set: st ? function (v) { note(typeName, key + ' ='); return st.call(this, v); } : undefined,
          });
        }
      } catch { /* some members refuse redefinition; skip them */ }
    }
  };

  // Instrument whatever the caller asks for, plus the types those globals
  // actually produce. A hardcoded list was the earlier shape and it silently
  // under-reported: a global nobody instruments looks identical to a global
  // nobody calls.
  const ALWAYS = [
    'WebGL2RenderingContext', 'WebGLRenderingContext', 'CanvasRenderingContext2D',
    'HTMLCanvasElement', 'OffscreenCanvas', 'HTMLImageElement', 'HTMLVideoElement',
    'Document', 'HTMLElement', 'Element', 'Node', 'EventTarget', 'Performance',
  ];
  // The global object itself is too broad to instrument wholesale, and its
  // interesting members are reached through the specific globals anyway.
  const SKIP = new Set(['window', 'self', 'globalThis', 'document', 'performance']);

  const seen = new Set();
  for (const n of [...ALWAYS, ...(names || [])]) {
    if (seen.has(n) || SKIP.has(n)) continue;
    seen.add(n);
    const g = globalThis[n];
    if (typeof g === 'function') {
      if (g.prototype && Object.getOwnPropertyNames(g.prototype).length > 1) {
        instrument(n, g.prototype);           // a class: watch its instances
      } else {
        const orig = g;                        // a bare function: fetch, atob, …
        try {
          globalThis[n] = function (...a) { note('globalThis', n, a); return orig.apply(this, a); };
        } catch { /* non-writable */ }
      }
    } else if (g && typeof g === 'object') {
      instrument(n, g);                        // console, location, navigator, screen
    }
  }

  // Statics that are not on any prototype.
  for (const [owner, name] of [[globalThis.URL, 'URL'], [globalThis.Object, null]]) {
    if (!owner || !name) continue;
    for (const key of ['createObjectURL', 'revokeObjectURL']) {
      const orig = owner[key];
      if (typeof orig !== 'function') continue;
      owner[key] = function (...a) { note(name, key, a); return orig.apply(this, a); };
    }
  }
  for (const key of ['createImageBitmap', 'fetch', 'requestAnimationFrame', 'queueMicrotask',
                     'setTimeout', 'setInterval', 'atob', 'btoa']) {
    const orig = globalThis[key];
    if (typeof orig !== 'function') continue;
    globalThis[key] = function (...a) { note('globalThis', key, a); return orig.apply(this, a); };
  }

  globalThis.__domTrace = () => ({
    hits,
    args: Object.fromEntries(Object.entries(argsSeen).map(([k, v]) => [k, [...v]])),
  });
};

const browser = await chromium.launch({
  args: ['--use-gl=angle', '--use-angle=metal', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
});
const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
const errors = [];
page.on('pageerror', (e) => errors.push(String(e)));
const fromArg = args.indexOf('--from');
const wanted = fromArg > -1 && args[fromArg + 1]
  ? Object.keys(JSON.parse(readFileSync(args[fromArg + 1], 'utf8')).globals ?? {})
  : [];
await page.addInitScript(recorder, wanted);
await page.goto(url, { waitUntil: 'load' });
await page.waitForTimeout(seconds * 1000);

const trace = await page.evaluate(() => (globalThis.__domTrace ? globalThis.__domTrace() : null));
await browser.close();

if (!trace) {
  console.error('trace.mjs: recorder never installed -- did the page load?');
  process.exit(1);
}

// --- report -----------------------------------------------------------------
const byType = new Map();
for (const [key, count] of Object.entries(trace.hits)) {
  const dot = key.indexOf('.');
  const type = key.slice(0, dot);
  const prop = key.slice(dot + 1);
  if (!byType.has(type)) byType.set(type, []);
  byType.get(type).push([prop, count, trace.args[key]]);
}

if (asJson) {
  const out = {};
  for (const [type, props] of byType) {
    out[type] = Object.fromEntries(props.sort((a, b) => b[1] - a[1])
      .map(([p, c, a]) => [p, a ? { count: c, args: a } : c]));
  }
  console.log(JSON.stringify({ url, seconds, pageErrors: errors, types: out }, null, 2));
  process.exit(0);
}

console.log(`\n${url} — traced ${seconds}s`);
console.log(`${byType.size} host types touched, ${Object.keys(trace.hits).length} distinct members\n`);
for (const [type, props] of [...byType].sort((a, b) => b[1].length - a[1].length)) {
  console.log(`## ${type}  (${props.length} members)`);
  const sorted = props.sort((a, b) => b[1] - a[1]);
  console.log('  ' + sorted.map(([p, c]) => (c > 3 ? `${p}×${c}` : p)).join(', '));
  // Shader sources arrive here as multi-KB strings; the useful part of a call
  // argument is almost always its head.
  const brief = (x) => {
    const one = String(x).replace(/\s+/g, ' ').trim();
    return JSON.stringify(one.length > 60 ? one.slice(0, 57) + '\u2026' : one);
  };
  for (const [p, , a] of sorted) {
    if (!a) continue;
    console.log(`  ${p}( ${a.slice(0, 10).map(brief).join(', ')}${a.length > 10 ? `, \u2026+${a.length - 10}` : ''} )`);
  }
  console.log();
}
if (errors.length) {
  console.log(`## page errors (${errors.length})`);
  for (const e of errors.slice(0, 5)) console.log('  ' + e.split('\n')[0]);
}
