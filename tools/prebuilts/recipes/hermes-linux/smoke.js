// The smoke test's program (smoke.cpp): features a web build leans on, as one line.
var out = [];
out.push([1, 2, 3].map(function (x) { return x * 2; }).join(','));
out.push(typeof Symbol.iterator, typeof Promise, typeof WeakRef, typeof Proxy);
out.push((2n ** 64n).toString());
out.push(/(\p{L}+)/u.exec('héllo wörld')[1]);
out.push(JSON.stringify({ a: [1, { b: null }] }));
out.push(new Date(0).toISOString());
out.push(new Uint8Array([1, 2, 3]).reduce(function (a, b) { return a + b; }, 0));
// Intl through ICU: collation and date formatting are real on Linux; Hermes' ICU backend leaves
// NumberFormat and toLocaleUpperCase as placeholders, so those are not asserted here.
out.push(['b', 'C', 'a'].sort(new Intl.Collator('en').compare).join(''),
         new Intl.DateTimeFormat('en-US', { timeZone: 'UTC' }).format(new Date(0)));
out.push('ok');
out.join('|');
