#!/usr/bin/env node
// What does this bundle expect the host to provide?
//
// Finds *free variables* -- identifiers the bundle reads but never declares --
// and reports the property and call shapes used on each. That is the honest
// definition of "what must be shimmed": grepping for `document.` misses aliased
// and destructured access, and over-reports names that turn out to be local.
//
//   node tools/dom-usage/analyze.mjs <bundle.js> [--json] [--all]
//
// Exit is 0 whatever it finds; this reports, it does not gate.

import { readFileSync } from 'node:fs';
import { basename } from 'node:path';
import * as acorn from 'acorn';
import * as walk from 'acorn-walk';
import { analyze as analyzeScopes } from 'eslint-scope';

// Things every JS engine already has. A free reference to one of these is not a
// shim -- Hermes provides it. Anything NOT here is the host's problem.
const ECMASCRIPT = new Set([
  'Object', 'Function', 'Boolean', 'Symbol', 'Error', 'AggregateError', 'EvalError',
  'RangeError', 'ReferenceError', 'SyntaxError', 'TypeError', 'URIError', 'Number',
  'BigInt', 'Math', 'Date', 'String', 'RegExp', 'Array', 'Int8Array', 'Uint8Array',
  'Uint8ClampedArray', 'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array',
  'Float32Array', 'Float64Array', 'BigInt64Array', 'BigUint64Array', 'Map', 'Set',
  'WeakMap', 'WeakSet', 'WeakRef', 'FinalizationRegistry', 'ArrayBuffer',
  'SharedArrayBuffer', 'DataView', 'Atomics', 'JSON', 'Promise', 'Reflect', 'Proxy',
  'globalThis', 'Infinity', 'NaN', 'undefined', 'eval', 'isFinite', 'isNaN',
  'parseFloat', 'parseInt', 'decodeURI', 'decodeURIComponent', 'encodeURI',
  'encodeURIComponent', 'escape', 'unescape', 'arguments', 'Intl',
]);

// Known host surfaces, so the report can say *what kind* of shim each one needs
// rather than listing 40 names flat.
const BUCKETS = [
  ['DOM core', ['document', 'Node', 'Element', 'HTMLElement', 'DocumentFragment', 'Text',
                'Comment', 'DOMParser', 'XMLSerializer', 'NodeFilter', 'Range',
                'MutationObserver', 'CustomEvent', 'Event', 'EventTarget', 'DOMException']],
  ['Canvas / graphics', ['HTMLCanvasElement', 'CanvasRenderingContext2D', 'WebGLRenderingContext',
                         'WebGL2RenderingContext', 'ImageBitmap', 'ImageData', 'OffscreenCanvas',
                         'createImageBitmap', 'Path2D']],
  ['Images / media', ['Image', 'HTMLImageElement', 'HTMLVideoElement', 'Audio', 'VideoFrame']],
  ['Timers / scheduling', ['setTimeout', 'clearTimeout', 'setInterval', 'clearInterval',
                           'requestAnimationFrame', 'cancelAnimationFrame', 'queueMicrotask',
                           'requestIdleCallback', 'cancelIdleCallback', 'setImmediate']],
  ['Window / environment', ['window', 'self', 'navigator', 'screen', 'location', 'history',
                            'devicePixelRatio', 'matchMedia', 'getComputedStyle', 'alert']],
  ['Networking', ['fetch', 'XMLHttpRequest', 'WebSocket', 'Request', 'Response', 'Headers',
                  'AbortController', 'AbortSignal', 'EventSource']],
  ['Storage', ['localStorage', 'sessionStorage', 'indexedDB', 'caches']],
  ['Encoding / binary', ['TextEncoder', 'TextDecoder', 'Blob', 'File', 'FileReader', 'URL',
                         'URLSearchParams', 'btoa', 'atob', 'structuredClone', 'crypto']],
  ['Workers', ['Worker', 'SharedWorker', 'MessageChannel', 'MessagePort', 'postMessage',
               'importScripts']],
  ['Console / diagnostics', ['console', 'performance', 'reportError']],
  ['Node-only (should not appear)', ['process', 'require', 'module', 'exports', '__dirname',
                                     '__filename', 'Buffer', 'global']],
];

const bucketOf = (name) => {
  for (const [label, names] of BUCKETS) if (names.includes(name)) return label;
  return 'Unclassified';
};

// --- free variables --------------------------------------------------------
// Scope resolution comes from `eslint-scope` -- the same analyzer ESLint uses
// for `no-undef` -- rather than a hand-rolled pass. Collecting every binding
// into one flat set (the obvious shortcut) silently suppresses a global the
// moment any function declares a local of the same name, which on a minified
// bundle is a coin flip. `globalScope.through` is the unresolved references,
// which is exactly the question being asked.


// --- instance tracking ------------------------------------------------------
// A global's own members are only half the surface. `document.createElement` is
// interesting mostly because of what gets called on the result, and the result
// is a *local variable* -- so `canvas.getContext`, `ctx.drawImage`, `img.onload`
// are invisible if you only watch the globals.
//
// So: recognise expressions whose value is a host object, and then follow that
// variable's references (resolved by eslint-scope, so shadowing is handled) to
// see what is called on it. One level deep, which is where the useful signal is.

const ELEMENT_TYPES = {
  canvas: 'HTMLCanvasElement', img: 'HTMLImageElement', image: 'HTMLImageElement',
  video: 'HTMLVideoElement', div: 'HTMLDivElement', span: 'HTMLSpanElement',
  link: 'HTMLLinkElement', style: 'HTMLStyleElement', script: 'HTMLScriptElement',
};
const CONTEXT_TYPES = {
  '2d': 'CanvasRenderingContext2D',
  webgl: 'WebGLRenderingContext', 'experimental-webgl': 'WebGLRenderingContext',
  webgl2: 'WebGL2RenderingContext',
};

/// String literals an argument can evaluate to. A ternary counts: minified code
/// writes `getContext(x ? "webgl2" : "webgl")`, and treating that as "not a
/// literal" silently drops the single most important surface in the bundle.
const literalArgs = (node, i = 0) => {
  const a = node.arguments?.[i];
  const out = [];
  const visit = (n) => {
    if (!n) return;
    if (n.type === 'Literal' && typeof n.value === 'string') out.push(n.value);
    else if (n.type === 'ConditionalExpression') { visit(n.consequent); visit(n.alternate); }
    else if (n.type === 'LogicalExpression') { visit(n.left); visit(n.right); }
  };
  visit(a);
  return out;
};
const literalArg = (node, i = 0) => literalArgs(node, i)[0] ?? null;

/// What host type does this expression evaluate to, if any?
function originType(node, isFree) {
  if (!node) return null;
  if (node.type === 'NewExpression' && node.callee.type === 'Identifier' && isFree(node.callee.name)) {
    return node.callee.name;
  }
  if (node.type === 'CallExpression' && node.callee.type === 'MemberExpression') {
    const prop = node.callee.computed ? null : node.callee.property.name;
    const obj = node.callee.object;
    if (prop === 'createElement' && obj.type === 'Identifier' && isFree(obj.name)) {
      const tag = (literalArg(node) || '').toLowerCase();
      return ELEMENT_TYPES[tag] || (tag ? `HTMLElement<${tag}>` : 'HTMLElement');
    }
    if (prop === 'getContext') {
      const kinds = literalArgs(node).map((k) => k.toLowerCase());
      // A ternary yields several; prefer the most capable, and never return null
      // just because the argument was computed -- an untyped context still needs
      // shimming, and silence is the worst answer.
      for (const want of ['webgl2', 'webgl', 'experimental-webgl', '2d']) {
        if (kinds.includes(want)) return CONTEXT_TYPES[want];
      }
      return kinds.length ? `RenderingContext<${kinds[0]}>` : 'RenderingContext<computed>';
    }
    if ((prop === 'getElementById' || prop === 'querySelector') &&
        obj.type === 'Identifier' && isFree(obj.name)) {
      return 'HTMLElement';
    }
  }
  return null;
}

/// `make()` where make is a known factory.
function factoryType(node, factories) {
  if (!node || !factories) return null;
  const call = node.type === 'LogicalExpression' ? node.left : node;
  if (call?.type !== 'CallExpression') return null;
  if (call.callee.type === 'Identifier') return factories.get(call.callee.name) ?? null;
  if (call.callee.type === 'MemberExpression' && !call.callee.computed) {
    return factories.get(call.callee.property.name) ?? null;
  }
  return null;
}

/// Functions whose return value is a host object. Lightning builds its GL
/// context in exactly this shape -- `function make(){ ... return canvas.getContext(...) }`
/// -- so without this hop the largest surface in the bundle stays invisible.
function collectFactories(ast, isFree) {
  const byName = new Map();
  walk.full(ast, (node) => {
    const isFn = node.type === 'FunctionDeclaration' || node.type === 'FunctionExpression' ||
                 node.type === 'ArrowFunctionExpression';
    if (!isFn) return;
    let name = node.id?.name;
    if (!name && node.__parent?.type === 'VariableDeclarator' && node.__parent.id.type === 'Identifier') {
      name = node.__parent.id.name;
    }
    if (!name) return;
    let produced = null;
    if (node.body.type !== 'BlockStatement') {
      produced = originType(node.body, isFree);
    } else {
      walk.full(node.body, (n) => {
        if (produced || n.type !== 'ReturnStatement' || !n.argument) return;
        // `a || b` is the common fallback shape: webgl2 then experimental-webgl.
        const arg = n.argument.type === 'LogicalExpression' ? n.argument.left : n.argument;
        produced = originType(arg, isFree);
      });
    }
    if (produced) byName.set(name, produced);
  });
  return byName;
}

/// Follow a resolved variable's references and record members touched on it.
function trackInstances(scopes, isFree, record, factories) {
  for (const scope of scopes.scopes) {
    for (const variable of scope.variables) {
      // A host value reaches a variable two ways: `const c = ...getContext()`
      // and a plain `c = ...getContext()` later. Minified bundles use the second
      // constantly, and looking only at declarator inits misses them entirely --
      // which is how the WebGL context, the single largest surface here, stayed
      // invisible. eslint-scope hands back the written expression per reference.
      let type = null;
      const def = variable.defs[0];
      if (def && def.node.type === 'VariableDeclarator') {
        type = originType(def.node.init, isFree);
      }
      if (!type) {
        for (const ref of variable.references) {
          if (!ref.writeExpr) continue;
          type = originType(ref.writeExpr, isFree) || factoryType(ref.writeExpr, factories);
          if (type) break;
        }
      }
      if (!type && def && def.node.type === 'VariableDeclarator') {
        type = factoryType(def.node.init, factories);
      }
      if (!type) continue;
      for (const ref of variable.references) {
        const id = ref.identifier;
        const parent = id.__parent;
        if (!parent || parent.type !== 'MemberExpression' || parent.object !== id) continue;
        const prop = parent.computed
          ? (parent.property.type === 'Literal' ? String(parent.property.value) : '[computed]')
          : parent.property.name;
        const call = parent.__parent;
        const args = call && call.type === 'CallExpression' && call.callee === parent
          ? call.arguments.filter((a) => a.type === 'Literal' && typeof a.value === 'string')
                          .map((a) => a.value)
          : [];
        record(type, prop, args);
      }
    }
  }
}

function analyze(source, filename) {
  const ast = acorn.parse(source, {
    ecmaVersion: 'latest',
    sourceType: 'module',
    allowHashBang: true,
    ranges: true,  // eslint-scope reads node.range when resolving
    allowAwaitOutsideFunction: true,
  });

  // Parent links: eslint-scope hands back reference identifiers, and the useful
  // question is always what encloses them.
  walk.full(ast, (node) => {
    for (const key of Object.keys(node)) {
      const child = node[key];
      if (Array.isArray(child)) {
        for (const c of child) if (c && typeof c.type === 'string') c.__parent = node;
      } else if (child && typeof child.type === 'string') {
        child.__parent = node;
      }
    }
  });

  // Names the bundle references but never binds, per real scope resolution.
  const scopes = analyzeScopes(ast, { ecmaVersion: 2024, sourceType: 'module' });
  const free = new Set(scopes.globalScope.through.map((ref) => ref.identifier.name));

  // Pass 2: references to names nothing declares, plus how they are used.
  const usage = new Map();  // name -> { reads, props:Map<prop,count>, calls:Map<prop,Set<arg>> }
  const note = (name) => {
    if (!usage.has(name)) usage.set(name, { reads: 0, props: new Map(), calls: new Map() });
    return usage.get(name);
  };
  const isFree = (name) => free.has(name) && !ECMASCRIPT.has(name);

  walk.full(ast, (node) => {
    // `foo.bar` / `foo['bar']` where foo is free
    if (node.type === 'MemberExpression' && node.object.type === 'Identifier' && isFree(node.object.name)) {
      const entry = note(node.object.name);
      const prop = node.computed
        ? (node.property.type === 'Literal' ? String(node.property.value) : '[computed]')
        : node.property.name;
      entry.props.set(prop, (entry.props.get(prop) ?? 0) + 1);
    }
    // `foo.bar('literal')` -- the argument is often the thing worth knowing,
    // e.g. which elements createElement is actually asked for.
    if (node.type === 'CallExpression' && node.callee.type === 'MemberExpression' &&
        node.callee.object.type === 'Identifier' && isFree(node.callee.object.name)) {
      const entry = note(node.callee.object.name);
      const prop = node.callee.computed ? '[computed]' : node.callee.property.name;
      const lits = node.arguments
        .filter((a) => a.type === 'Literal' && typeof a.value === 'string')
        .map((a) => a.value);
      if (lits.length) {
        if (!entry.calls.has(prop)) entry.calls.set(prop, new Set());
        for (const l of lits) entry.calls.get(prop).add(l);
      }
    }
    if (node.type === 'Identifier' && isFree(node.name)) note(node.name).reads += 1;
    // `new Foo()` where Foo is free
    if (node.type === 'NewExpression' && node.callee.type === 'Identifier' && isFree(node.callee.name)) {
      note(node.callee.name).props.set('[constructed]', 1);
    }
  });

  // Members touched on values that came *from* the host, keyed by type.
  const instances = new Map();
  const factories = collectFactories(ast, isFree);
  trackInstances(scopes, isFree, (type, prop, args) => {
    if (!instances.has(type)) instances.set(type, { props: new Map(), calls: new Map() });
    const e = instances.get(type);
    e.props.set(prop, (e.props.get(prop) ?? 0) + 1);
    if (args.length) {
      if (!e.calls.has(prop)) e.calls.set(prop, new Set());
      for (const a of args) e.calls.get(prop).add(a);
    }
  }, factories);

  return { filename, bytes: source.length, names: usage, instances };
}

// --- report -----------------------------------------------------------------
const args = process.argv.slice(2);
const files = args.filter((a) => !a.startsWith('--'));
const asJson = args.includes('--json');
const showAll = args.includes('--all');

if (files.length === 0) {
  console.error('usage: analyze.mjs <bundle.js> [more.js...] [--json] [--all]');
  process.exit(2);
}

const merged = new Map();
const mergedInstances = new Map();
let totalBytes = 0;
for (const f of files) {
  const r = analyze(readFileSync(f, 'utf8'), basename(f));
  totalBytes += r.bytes;
  for (const [name, u] of r.names) {
    if (!merged.has(name)) merged.set(name, { reads: 0, props: new Map(), calls: new Map() });
    const m = merged.get(name);
    m.reads += u.reads;
    for (const [p, c] of u.props) m.props.set(p, (m.props.get(p) ?? 0) + c);
    for (const [p, s] of u.calls) {
      if (!m.calls.has(p)) m.calls.set(p, new Set());
      for (const v of s) m.calls.get(p).add(v);
    }
  }
  for (const [type, u] of r.instances) {
    if (!mergedInstances.has(type)) mergedInstances.set(type, { props: new Map(), calls: new Map() });
    const m = mergedInstances.get(type);
    for (const [p, c] of u.props) m.props.set(p, (m.props.get(p) ?? 0) + c);
    for (const [p, s] of u.calls) {
      if (!m.calls.has(p)) m.calls.set(p, new Set());
      for (const v of s) m.calls.get(p).add(v);
    }
  }
}

if (asJson) {
  const out = {};
  for (const [name, u] of merged) {
    out[name] = {
      bucket: bucketOf(name),
      reads: u.reads,
      properties: Object.fromEntries([...u.props].sort((a, b) => b[1] - a[1])),
      callArguments: Object.fromEntries([...u.calls].map(([k, v]) => [k, [...v]])),
    };
  }
  const inst = {};
  for (const [type, u] of mergedInstances) {
    inst[type] = {
      members: Object.fromEntries([...u.props].sort((a, b) => b[1] - a[1])),
      callArguments: Object.fromEntries([...u.calls].map(([k, v]) => [k, [...v]])),
    };
  }
  console.log(JSON.stringify({ files, bytes: totalBytes, globals: out, instances: inst }, null, 2));
  process.exit(0);
}

const byBucket = new Map();
for (const [name, u] of merged) {
  const b = bucketOf(name);
  if (!byBucket.has(b)) byBucket.set(b, []);
  byBucket.get(b).push([name, u]);
}

console.log(`\n${files.map((f) => basename(f)).join(', ')} — ${(totalBytes / 1024).toFixed(0)} KB`);
console.log(`${merged.size} free globals referenced\n`);

const order = [...BUCKETS.map(([l]) => l), 'Unclassified'];
const printInstances = () => {
  if (mergedInstances.size === 0) return;
  console.log('## Members used on host objects');
  console.log('   (what is called on values the host hands back -- the other half of the shim)\n');
  const rows = [...mergedInstances].sort((a, b) => b[1].props.size - a[1].props.size);
  for (const [type, u] of rows) {
    const props = [...u.props].sort((a, b) => b[1] - a[1]);
    console.log(`  ${type}  (${props.length} members)`);
    const line = props.map(([p, c]) => `${p}${c > 2 ? `\u00d7${c}` : ''}`).join(', ');
    console.log(`    ${line}`);
    for (const [prop, vals] of u.calls) {
      const v = [...vals].slice(0, 8).map((x) => JSON.stringify(x)).join(', ');
      console.log(`    ${prop}( ${v}${vals.size > 8 ? ', \u2026' : ''} )`);
    }
    console.log();
  }
};
for (const bucket of order) {
  const entries = byBucket.get(bucket);
  if (!entries) continue;
  // Unclassified is mostly minifier noise; hide it unless asked.
  if (bucket === 'Unclassified' && !showAll) {
    console.log(`${bucket}: ${entries.length} names (--all to list)\n`);
    continue;
  }
  console.log(`## ${bucket}`);
  for (const [name, u] of entries.sort((a, b) => b[1].reads - a[1].reads)) {
    const props = [...u.props].sort((a, b) => b[1] - a[1]).map(([p]) => p);
    const shown = props.slice(0, 12).join(', ');
    const more = props.length > 12 ? ` (+${props.length - 12})` : '';
    console.log(`  ${name}  ${u.reads}x${props.length ? `\n    .${shown}${more}` : ''}`);
    for (const [prop, vals] of u.calls) {
      const v = [...vals].slice(0, 8).map((s) => JSON.stringify(s)).join(', ');
      console.log(`    ${prop}( ${v}${vals.size > 8 ? ', …' : ''} )`);
    }
  }
  console.log();
}

printInstances();
