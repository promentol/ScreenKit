// A custom WebGL shader: an animated plasma field.
//
// The renderer's contract, from @lightningjs/renderer's WebGlShaderType:
//
//  - `props` are the values a template can set, with their defaults. Setting one
//    re-runs `update`, which is where props become uniforms.
//  - `time` makes the shader timed. The renderer tracks the node, keeps the stage
//    rendering every frame and feeds the returned value to `u_time`. That turns a
//    shader from "draw once when dirty" into continuous per-frame GPU work, which
//    is the load this example exists to put on ScreenKit.
//  - No `vertex`: the renderer's default vertex shader supplies the varyings
//    below. `u_dimensions`, `u_alpha` and `u_time` are filled in by the renderer.
//
// One renderer detail matters here: in 3.3.1 it only feeds `u_time` when the
// program also *uses* `u_dimensions` (WebGlShaderProgram checks that location
// before enabling time). This shader uses it for aspect correction, so it holds.
export const Plasma = {
  props: {
    speed: 1,
    colorA: 0x22d3eeff,
    colorB: 0x7c3aedff,
  },
  // Seconds since the stage started.
  time(stage) {
    return stage.elapsedTime / 1000
  },
  update() {
    const props = this.props
    this.uniform1f('u_speed', props.speed)
    this.uniformRGBA('u_colorA', props.colorA)
    this.uniformRGBA('u_colorB', props.colorB)
  },
  fragment: `
    # ifdef GL_FRAGMENT_PRECISION_HIGH
    precision highp float;
    # else
    precision mediump float;
    # endif

    uniform vec2 u_dimensions;
    uniform float u_alpha;
    uniform float u_time;

    uniform float u_speed;
    uniform vec4 u_colorA;
    uniform vec4 u_colorB;

    varying vec4 v_color;
    varying vec2 v_textureCoords;
    varying vec2 v_nodeCoords;

    void main() {
      // Square cells whatever the node's aspect ratio.
      vec2 p = v_nodeCoords * u_dimensions / min(u_dimensions.x, u_dimensions.y) * 6.0;
      float t = u_time * u_speed;

      float v = sin(p.x + t);
      v += sin((p.y + t) * 0.5);
      v += sin((p.x + p.y + t) * 0.5);
      vec2 c = p + vec2(sin(t / 3.0), cos(t / 2.0)) * 3.0;
      v += sin(sqrt(c.x * c.x + c.y * c.y + 1.0) + t);

      float k = 0.5 + 0.5 * sin(v * 3.14159);
      gl_FragColor = mix(u_colorA, u_colorB, k) * u_alpha;
    }
  `,
}
