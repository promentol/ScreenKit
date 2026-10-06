// The workload the JIT measurement runs (tools/prebuilts/recipes/hermes-linux/build.sh compiles
// it with the pinned hermesc, as a device's package is compiled). Shaped like game code --
// objects, a prototype method, array indexing -- because Hermes' baseline JIT speeds that up
// (about 3.8x here) where an arithmetic loop is no faster, or slower. The value is the result,
// so the smoke can check the JIT and the interpreter agree.
(function () {
  function Sprite(x, y) { this.x = x; this.y = y; this.vx = 1.5; this.vy = -0.5; }
  Sprite.prototype.step = function (dt, w, h) {
    this.x += this.vx * dt; this.y += this.vy * dt;
    if (this.x > w || this.x < 0) this.vx = -this.vx;
    if (this.y > h || this.y < 0) this.vy = -this.vy;
  };
  var sprites = [];
  for (var i = 0; i < 2000; i++) sprites.push(new Sprite(i % 800, (i * 7) % 600));
  for (var f = 0; f < 400; f++) { for (i = 0; i < sprites.length; i++) sprites[i].step(1, 800, 600); }
  var sum = 0; for (i = 0; i < sprites.length; i++) sum += sprites[i].x + sprites[i].y;
  return Math.round(sum);
})();
