// M3: console.log AND timers from an .hbc.
// Ordering is the point: microtasks drain before the next macrotask, and a
// 0ms timer still runs after an already-resolved promise.
const order = [];

setTimeout(() => {
  order.push('timeout-10');
  console.log('order: ' + order.join(','));
}, 10);

setTimeout(() => order.push('timeout-0'), 0);

Promise.resolve().then(() => order.push('microtask'));

queueMicrotask(() => order.push('queued-microtask'));

let ticks = 0;
const interval = setInterval(() => {
  ticks += 1;
  order.push('interval-' + ticks);
  if (ticks === 2) clearInterval(interval);
}, 1);

// Cancelled before it can run: must never appear in the order.
const doomed = setTimeout(() => order.push('CANCELLED'), 0);
clearTimeout(doomed);

console.log('sync done');
