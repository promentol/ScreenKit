// Which ES2015-ES2023 built-ins (Annex B included) this Hermes lacks, probed the
// way an app meets them: compiled to bytecode by the pinned hermesc.
//
// The hermes-builtins row compares the result with
// packages/@screenkit/vite-plugin/src/hermes-builtins.json, the list the Vite
// plugin polyfills from. Completion value: the missing names, sorted, one per
// line.

var G = globalThis;
var TypedArray = Object.getPrototypeOf(Int8Array);
var missing = [];

// A dotted path, resolved property by property. `%TypedArray%` is the intrinsic;
// a trailing `[@@name]` is a well-known symbol. Only the last step is tested
// with `in`, so an accessor that throws on the wrong receiver (Symbol.prototype.
// description) is not called.
function present(path) {
  var parts = path.replace(/\[@@(\w+)\]/g, '.@@$1').split('.');
  var object = parts[0] === '%TypedArray%' ? TypedArray : G[parts[0]];
  if (parts.length === 1) return parts[0] in G;
  for (var i = 1; i < parts.length; i++) {
    if (object === undefined || object === null) return false;
    var key = parts[i];
    if (key.indexOf('@@') === 0) {
      key = Symbol[key.slice(2)];
      if (key === undefined) return false;
    }
    if (!(key in Object(object))) return false;
    if (i < parts.length - 1) object = object[key];
  }
  return true;
}

function probe(paths) {
  paths.forEach(function (path) { if (!present(path)) missing.push(path); });
}

// Members of objects that may themselves be missing: an absent object is its
// own gap, not one more per member.
function probeMembers(paths) {
  probe(paths.filter(function (path) {
    var root = path.split(/[.[]/)[0];
    return root === '%TypedArray%' || G[root] !== undefined;
  }));
}

function behaves(name, check) {
  var ok;
  try { ok = check(); } catch (e) { ok = false; }
  if (!ok) missing.push(name);
}

var TYPED_ARRAYS = ['Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array', 'Uint16Array', 'Int32Array',
                    'Uint32Array', 'Float32Array', 'Float64Array'];

// ES2015
probe(['Symbol', 'Map', 'Set', 'WeakMap', 'WeakSet', 'Proxy', 'Reflect', 'Promise', 'ArrayBuffer', 'DataView']
  .concat(TYPED_ARRAYS));
probe([
  'Object.assign', 'Object.is', 'Object.setPrototypeOf', 'Object.getOwnPropertySymbols',
  'Array.from', 'Array.of', 'Array.prototype.copyWithin', 'Array.prototype.fill', 'Array.prototype.find',
  'Array.prototype.findIndex', 'Array.prototype.entries', 'Array.prototype.keys', 'Array.prototype.values',
  'String.fromCodePoint', 'String.raw', 'String.prototype.codePointAt', 'String.prototype.normalize',
  'String.prototype.repeat', 'String.prototype.startsWith', 'String.prototype.endsWith', 'String.prototype.includes',
  'Number.EPSILON', 'Number.MAX_SAFE_INTEGER', 'Number.MIN_SAFE_INTEGER', 'Number.isFinite', 'Number.isInteger',
  'Number.isNaN', 'Number.isSafeInteger', 'Number.parseFloat', 'Number.parseInt',
  'Math.acosh', 'Math.asinh', 'Math.atanh', 'Math.cbrt', 'Math.clz32', 'Math.cosh', 'Math.expm1', 'Math.fround',
  'Math.hypot', 'Math.imul', 'Math.log10', 'Math.log1p', 'Math.log2', 'Math.sign', 'Math.sinh', 'Math.tanh',
  'Math.trunc',
  'RegExp.prototype.flags', 'RegExp.prototype.sticky', 'RegExp.prototype.unicode',
  'Symbol.hasInstance', 'Symbol.isConcatSpreadable', 'Symbol.iterator', 'Symbol.match', 'Symbol.replace',
  'Symbol.search', 'Symbol.species', 'Symbol.split', 'Symbol.toPrimitive', 'Symbol.toStringTag',
  'Symbol.unscopables', 'Symbol.for', 'Symbol.keyFor',
  'Promise.all', 'Promise.race', 'Promise.reject', 'Promise.resolve', 'ArrayBuffer.isView',
  'ArrayBuffer.prototype.slice',
  'Reflect.apply', 'Reflect.construct', 'Reflect.defineProperty', 'Reflect.deleteProperty', 'Reflect.get',
  'Reflect.getOwnPropertyDescriptor', 'Reflect.getPrototypeOf', 'Reflect.has', 'Reflect.isExtensible',
  'Reflect.ownKeys', 'Reflect.preventExtensions', 'Reflect.set', 'Reflect.setPrototypeOf', 'Proxy.revocable',
  '%TypedArray%.from', '%TypedArray%.of'
]);
// Symbol-keyed members, only where the symbol itself exists: a missing symbol is
// one gap, not one per member.
if (Symbol.iterator) {
  probe(['Array.prototype[@@iterator]', 'String.prototype[@@iterator]', '%TypedArray%.prototype[@@iterator]',
         'Map.prototype[@@iterator]', 'Set.prototype[@@iterator]']);
}
if (Symbol.toPrimitive) probe(['Date.prototype[@@toPrimitive]', 'Symbol.prototype[@@toPrimitive]']);
if (Symbol.hasInstance) probe(['Function.prototype[@@hasInstance]']);
if (Symbol.toStringTag) {
  probeMembers(['Math[@@toStringTag]', 'JSON[@@toStringTag]', 'Promise.prototype[@@toStringTag]', 'Map.prototype[@@toStringTag]',
         'Set.prototype[@@toStringTag]', 'WeakMap.prototype[@@toStringTag]', 'WeakSet.prototype[@@toStringTag]',
         'ArrayBuffer.prototype[@@toStringTag]', 'DataView.prototype[@@toStringTag]', 'Symbol.prototype[@@toStringTag]',
         '%TypedArray%.prototype[@@toStringTag]', 'BigInt.prototype[@@toStringTag]', 'WeakRef.prototype[@@toStringTag]',
         'FinalizationRegistry.prototype[@@toStringTag]', 'Reflect[@@toStringTag]', 'Atomics[@@toStringTag]']);
}
if (Symbol.match) {
  probe(['RegExp.prototype[@@match]', 'RegExp.prototype[@@replace]', 'RegExp.prototype[@@search]',
         'RegExp.prototype[@@split]']);
}
if (Symbol.species) {
  probe(['Array[@@species]', 'Map[@@species]', 'Set[@@species]', 'Promise[@@species]', 'RegExp[@@species]',
         'ArrayBuffer[@@species]', '%TypedArray%[@@species]']);
}
if (Symbol.unscopables) probe(['Array.prototype[@@unscopables]']);
probe(['copyWithin', 'entries', 'every', 'fill', 'filter', 'find', 'findIndex', 'forEach', 'indexOf', 'join',
       'keys', 'lastIndexOf', 'map', 'reduce', 'reduceRight', 'reverse', 'set', 'slice', 'some', 'sort',
       'subarray', 'values', 'toLocaleString'].map(function (m) { return '%TypedArray%.prototype.' + m; }));

// ES2016
probe(['Array.prototype.includes', '%TypedArray%.prototype.includes']);

// ES2017
probe(['Object.values', 'Object.entries', 'Object.getOwnPropertyDescriptors', 'String.prototype.padStart',
       'String.prototype.padEnd', 'SharedArrayBuffer', 'Atomics']);

// ES2018
probe(['Promise.prototype.finally', 'RegExp.prototype.dotAll', 'Symbol.asyncIterator']);
behaves('RegExp named capture groups', function () { return new RegExp('(?<y>\\d+)').exec('x42').groups.y === '42'; });
behaves('RegExp lookbehind', function () { return new RegExp('(?<=\\$)\\d').exec('$5')[0] === '5'; });
behaves('RegExp Unicode property escapes', function () { return new RegExp('\\p{L}', 'u').test('a'); });

// ES2019
probe(['Array.prototype.flat', 'Array.prototype.flatMap', 'Object.fromEntries', 'String.prototype.trimStart',
       'String.prototype.trimEnd', 'Symbol.prototype.description']);
behaves('Array.prototype.sort stability', function () {
  var a = [];
  for (var i = 0; i < 40; i++) a.push({ k: i % 3, i: i });
  a.sort(function (x, y) { return x.k - y.k; });
  for (var j = 1; j < a.length; j++) if (a[j].k === a[j - 1].k && a[j].i < a[j - 1].i) return false;
  return true;
});
behaves('JSON.stringify well-formed', function () { return JSON.stringify('\uD800') === '"\\ud800"'; });
behaves('Function.prototype.toString source text', function () {
  return (function f(a) { /* kept */ return a; }).toString().indexOf('/* kept */') >= 0;
});

// ES2020
probe(['BigInt', 'BigInt.asIntN', 'BigInt.asUintN', 'BigInt64Array', 'BigUint64Array',
       'DataView.prototype.getBigInt64', 'DataView.prototype.setBigInt64', 'DataView.prototype.getBigUint64',
       'DataView.prototype.setBigUint64', 'Promise.allSettled', 'globalThis', 'String.prototype.matchAll',
       'Symbol.matchAll']);
if (Symbol.matchAll) probe(['RegExp.prototype[@@matchAll]']);

// ES2021
probe(['Promise.any', 'AggregateError', 'String.prototype.replaceAll', 'WeakRef', 'FinalizationRegistry']);

// ES2022
probe(['Array.prototype.at', 'String.prototype.at', '%TypedArray%.prototype.at', 'Object.hasOwn',
       'RegExp.prototype.hasIndices']);
behaves('Error cause', function () { return new Error('x', { cause: 1 }).cause === 1; });
behaves('RegExp match indices', function () { return new RegExp('b', 'd').exec('ab').indices[0][0] === 1; });

// ES2023
probe(['Array.prototype.findLast', 'Array.prototype.findLastIndex', 'Array.prototype.toReversed',
       'Array.prototype.toSorted', 'Array.prototype.toSpliced', 'Array.prototype.with',
       '%TypedArray%.prototype.findLast', '%TypedArray%.prototype.findLastIndex',
       '%TypedArray%.prototype.toReversed', '%TypedArray%.prototype.toSorted', '%TypedArray%.prototype.with']);
behaves('Symbols as WeakMap keys', function () {
  var map = new WeakMap(), key = Symbol('k');
  map.set(key, 1);
  return map.get(key) === 1;
});

// Annex B: normative for web browsers, so part of what a web build may use.
probe(['escape', 'unescape', 'String.prototype.substr', 'String.prototype.trimLeft', 'String.prototype.trimRight',
       'Date.prototype.getYear', 'Date.prototype.setYear', 'Date.prototype.toGMTString', 'RegExp.prototype.compile',
       'Object.prototype.__defineGetter__', 'Object.prototype.__defineSetter__', 'Object.prototype.__lookupGetter__',
       'Object.prototype.__lookupSetter__', 'Object.prototype.__proto__']);
probe(['anchor', 'big', 'blink', 'bold', 'fixed', 'fontcolor', 'fontsize', 'italics', 'link', 'small', 'strike',
       'sub', 'sup'].map(function (m) { return 'String.prototype.' + m; }));

missing.sort().join('\n');
