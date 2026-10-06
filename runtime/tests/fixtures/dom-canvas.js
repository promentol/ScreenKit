// Every row of the DOM-shim spec's I/O matrix, driven from JS.
//
// This is the bundle the verification command runs windowed:
//
//   screenkit-host --window runtime/build/macos/fixtures/dom-canvas.hbc
//
// It prints the renderer string obtained *through* document.createElement(),
// which is the whole point: the GL context was already there, and the only new
// thing is that an unmodified app can now reach it the way the web reaches it.
//
// It assumes the prelude has already been evaluated -- the host does that after
// the graphics bootstrap -- and schedules nothing, so the host exits as soon as
// it has run.

var passed = 0;
var failed = 0;

function row(name, actual, expected) {
  if (String(actual) === String(expected)) {
    passed += 1;
    console.log('dom-canvas: ' + name + ' ok');
  } else {
    failed += 1;
    console.error('dom-canvas: ' + name + ' FAILED -- got ' + actual + ', want ' + expected);
  }
}

// --- row: canvas creation ----------------------------------------------------
var canvas = document.createElement('canvas');
row('canvas-creation',
    [typeof canvas, typeof canvas.width, typeof canvas.height, typeof canvas.getContext].join(','),
    'object,number,number,function');

// --- row: GL handoff ---------------------------------------------------------
// The same object as the `gl` global, not a new one and not a wrapper.
var context = canvas.getContext('webgl');
row('gl-handoff', context === gl, true);

// --- row: WebGL2 request -----------------------------------------------------
// ANGLE on Apple grants ES 3.0, which is what WebGL2 is defined against, so one
// context answers every spelling.
row('webgl2', canvas.getContext('webgl2') === gl && canvas.getContext('experimental-webgl') === gl,
    true);

// --- row: dimensions ---------------------------------------------------------
// Read before any assignment: the real drawable, straight from the GL surface.
row('dimensions',
    canvas.width === gl.drawingBufferWidth && canvas.height === gl.drawingBufferHeight &&
      canvas.width > 0 && canvas.height > 0,
    true);

// --- row: dimension write ----------------------------------------------------
// Legal and inert. A read after the write still returns the true size, so
// canvas.width and gl.drawingBufferWidth can never disagree.
var realWidth = canvas.width;
var realHeight = canvas.height;
canvas.width = 640;
canvas.height = 480;
row('dimension-write',
    canvas.width === realWidth && canvas.height === realHeight &&
      canvas.width === gl.drawingBufferWidth,
    true);

// --- row: second GL canvas ---------------------------------------------------
// The first canvas is on the page, so it is the frame. A second one gets a real
// GL context of its own -- its own state, its own drawing buffer -- and a layer
// composited at the rect the CSS subset gives it, which is also what
// `canvas.width` then reads. (A canvas never attached still hands the frame on
// rather than spending a context on itself.)
document.body.appendChild(canvas);
var second = document.createElement('canvas');
second.style.cssText = 'position: absolute; left: 0; top: 0; width: 50%; height: 50%';
document.body.appendChild(second);
var secondGl = second.getContext('webgl');
row('second-gl-canvas',
    [secondGl !== null, secondGl !== gl, secondGl.canvas === second,
     second.width === Math.round(gl.drawingBufferWidth / 2),
     second.width === secondGl.drawingBufferWidth].join(','),
    'true,true,true,true,true');

// --- row: unknown element ----------------------------------------------------
// Inert, and tolerant of property set/get: Lightning's loader builds a <link>
// this way and must not take the app down with it.
var link = document.createElement('link');
link.rel = 'modulepreload';
link.href = 'assets/index.js';
link.crossOrigin = '';
row('unknown-element',
    [link.rel, link.href, typeof link.style, typeof link.appendChild,
     typeof link.neverSet].join(','),
    'modulepreload,assets/index.js,object,function,undefined');

// --- row: root lookup --------------------------------------------------------
var app = document.getElementById('app');
row('root-lookup',
    [typeof app.appendChild, typeof app.style, document.getElementById('app') === app,
     document.getElementById('not-app') === null].join(','),
    'function,object,true,true');
app.appendChild(canvas);

// --- row: context kinds --------------------------------------------------------
// A canvas has one kind of context: a GL canvas answers '2d' with null, as a
// browser does; a fresh canvas gets the software 2D context, and then answers
// 'webgl' with null. An unknown kind is null and warned once.
var scratch = document.createElement('canvas');
var twod = document.createElement('canvas');
row('context-kinds',
    [canvas.getContext('2d') === null, second.getContext('2d') === null,
     twod.getContext('2d') instanceof CanvasRenderingContext2D,
     twod.getContext('webgl') === null, scratch.getContext('bitmaprenderer') === null].join(','),
    'true,true,true,true,true');

// --- the evidence ------------------------------------------------------------
// The renderer string, reached through the shim rather than through the `gl`
// global. This is the line that says the handoff is real.
console.log('dom-canvas: GL_RENDERER = ' + context.getParameter(context.RENDERER));
console.log('dom-canvas: drawable = ' + canvas.width + 'x' + canvas.height);

if (failed === 0) {
  console.log('dom-canvas: ' + passed + '/' + passed + ' matrix rows ok');
} else {
  console.error('dom-canvas: ' + failed + ' of ' + (passed + failed) + ' matrix rows FAILED');
  throw new Error('dom-canvas: ' + failed + ' matrix rows failed');
}

'dom-canvas-ok';
