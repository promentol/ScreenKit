// A triangle, drawn through the vendored expo-gl WebGLRenderingContext.
//
// This is real WebGL, not GLES: createBuffer returns a WebGLBuffer object,
// getShaderParameter replaces glGetShaderiv, getParameter replaces glGetString.
// The earlier hand-rolled binding exposed GLES names directly, which is why
// this fixture had to change when the GL path moved to the vendored tree.

function compile(type, src, label) {
  var sh = gl.createShader(type);
  gl.shaderSource(sh, src);
  gl.compileShader(sh);
  if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
    console.log('triangle: ' + label + ' compile failed: ' + gl.getShaderInfoLog(sh));
    gl.deleteShader(sh);
    return null;
  }
  return sh;
}

var VERT = [
  'attribute vec2 a_pos;',
  'attribute vec3 a_color;',
  'varying vec3 v_color;',
  'uniform float u_time;',
  'void main() {',
  '  float s = sin(u_time), c = cos(u_time);',
  '  vec2 p = vec2(a_pos.x * c - a_pos.y * s, a_pos.x * s + a_pos.y * c);',
  '  v_color = a_color;',
  '  gl_Position = vec4(p, 0.0, 1.0);',
  '}'
].join('\n');

var FRAG = [
  'precision mediump float;',
  'varying vec3 v_color;',
  'void main() { gl_FragColor = vec4(v_color, 1.0); }'
].join('\n');

console.log('triangle: GL_RENDERER = ' + gl.getParameter(gl.RENDERER));
console.log('triangle: GL_VERSION = ' + gl.getParameter(gl.VERSION));

var vs = compile(gl.VERTEX_SHADER, VERT, 'vertex');
var fs = compile(gl.FRAGMENT_SHADER, FRAG, 'fragment');
var prog = gl.createProgram();
gl.attachShader(prog, vs);
gl.attachShader(prog, fs);
gl.linkProgram(prog);
if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
  console.log('triangle: link failed: ' + gl.getProgramInfoLog(prog));
}
gl.deleteShader(vs);
gl.deleteShader(fs);

// Interleaved: x, y, r, g, b.
var verts = new Float32Array([
   0.0,  0.8,  0.2, 0.9, 0.4,
  -0.8, -0.6,  0.95, 0.3, 0.3,
   0.8, -0.6,  0.3, 0.5, 1.0
]);
var buf = gl.createBuffer();
gl.bindBuffer(gl.ARRAY_BUFFER, buf);
gl.bufferData(gl.ARRAY_BUFFER, verts, gl.STATIC_DRAW);

var aPos = gl.getAttribLocation(prog, 'a_pos');
var aColor = gl.getAttribLocation(prog, 'a_color');
var uTime = gl.getUniformLocation(prog, 'u_time');
var STRIDE = 5 * 4;
gl.enableVertexAttribArray(aPos);
gl.vertexAttribPointer(aPos, 2, gl.FLOAT, false, STRIDE, 0);
gl.enableVertexAttribArray(aColor);
gl.vertexAttribPointer(aColor, 3, gl.FLOAT, false, STRIDE, 2 * 4);

var limit =
  typeof globalThis.__triangleFrames === 'number' ? globalThis.__triangleFrames : Infinity;
var frames = 0;
var errors = 0;

function frame(t) {
  gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
  gl.clearColor(0.04, 0.03, 0.09, 1.0);
  gl.clear(gl.COLOR_BUFFER_BIT);

  gl.useProgram(prog);
  gl.uniform1f(uTime, t / 1000.0);
  gl.drawArrays(gl.TRIANGLES, 0, 3);

  if (gl.getError() !== gl.NO_ERROR) errors += 1;
  frames += 1;

  if (frames >= limit) {
    console.log('triangle: ' + frames + ' frames, ' + errors + ' gl errors');
    return;
  }
  requestAnimationFrame(frame);
}

requestAnimationFrame(frame);
console.log('triangle: first frame requested');
