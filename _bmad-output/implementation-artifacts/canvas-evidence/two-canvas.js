// A game canvas (the page's frame) with a HUD canvas over it: the HUD clears
// transparent and fills its left half, so the game shows through the rest.
var game = document.createElement('canvas');
document.body.appendChild(game);
var gg = game.getContext('webgl');

var hud = document.createElement('canvas');
hud.style.cssText = 'position:absolute; left:10%; top:10%; width:50%; height:50%; z-index:2';
document.body.appendChild(hud);
var hg = hud.getContext('webgl');
console.log('two-canvas: hud context = ' + (hg !== null) + ', distinct = ' + (hg !== gg) +
            ', hud buffer = ' + hud.width + 'x' + hud.height +
            ', drawable = ' + gg.drawingBufferWidth + 'x' + gg.drawingBufferHeight);

var t = 0;
(function frame() {
  t += 0.02;
  gg.clearColor(0.1, 0.2, 0.5 + 0.3 * Math.sin(t), 1);
  gg.clear(gg.COLOR_BUFFER_BIT);
  if (hg) {
    hg.clearColor(0, 0, 0, 0);
    hg.clear(hg.COLOR_BUFFER_BIT);
    hg.enable(hg.SCISSOR_TEST);
    hg.scissor(0, 0, Math.round(hg.drawingBufferWidth / 2), hg.drawingBufferHeight);
    hg.clearColor(1, 0.6, 0, 1);
    hg.clear(hg.COLOR_BUFFER_BIT);
    hg.disable(hg.SCISSOR_TEST);
  }
  requestAnimationFrame(frame);
})();
