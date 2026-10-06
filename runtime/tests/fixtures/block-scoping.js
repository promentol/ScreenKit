// ES6 block scoping, which this Hermes leaves off unless asked. With it off,
// `let` is function-scoped: the three closures share one `i` and all read 3.
// Blits builds its reactive effects this way, so a wrong answer here is a
// Lightning app that renders once and throws on its first state change.
var fromFor = [];
for (let i = 0; i < 3; i++) fromFor.push(function () { return i; });

var fromBlock = [];
for (var j = 0; j < 3; j++) {
  const k = j * 10;
  fromBlock.push(function () { return k; });
}

var read = function (f) { return f(); };
fromFor.map(read).join(',') + '|' + fromBlock.map(read).join(',');
