import Blits from '@lightningjs/blits'

// Every WebGL shader Blits registers, plus one custom shader, one tile each.
//
//   rounded / border / shadow  -> element attributes; Blits picks the shader
//                                 (rounded, border, roundedWithShadow,
//                                 roundedWithBorderAndShadow) from the combination
//   shader="{type: ...}"       -> linearGradient, radialGradient, holePunch, and
//                                 the custom `plasma` registered in index.js
//
// Two tiles move, on purpose, because they exercise different renderer paths:
// the linear gradient's angle is reactive state, so each change goes through
// shader-prop reactivity and `update()` -> uniforms; the plasma is a timed shader,
// so the renderer redraws it every frame with a new `u_time`.
export default Blits.Component('App', {
  template: `
    <Element w="1920" h="1080" color="#0b1020">
      <Text content="Lightning 3 shaders" x="140" y="56" size="64" color="#f8fafc" />
      <Text
        content="built-in WebGL shaders and a custom one, on ScreenKit"
        x="140"
        y="136"
        size="30"
        color="#94a3b8"
      />

      <Element x="140" y="230" w="380" h="300" color="#38bdf8" rounded="48" />
      <Text content="rounded" x="140" y="550" size="30" color="#cbd5e1" />

      <Element
        x="560"
        y="230"
        w="380"
        h="300"
        color="#1e293b"
        border="{w: 10, color: '#f472b6'}"
      />
      <Text content="border" x="560" y="550" size="30" color="#cbd5e1" />

      <Element
        x="980"
        y="230"
        w="380"
        h="300"
        color="#facc15"
        rounded="32"
        shadow="{color: '#000000', projection: [0, 24, 48, 4]}"
      />
      <Text content="rounded + shadow" x="980" y="550" size="30" color="#cbd5e1" />

      <Element
        x="1400"
        y="230"
        w="380"
        h="300"
        color="#10b981"
        rounded="60"
        border="{w: 8, color: '#ecfdf5'}"
        shadow="{color: '#000000', projection: [0, 16, 40, 0]}"
      />
      <Text content="rounded + border + shadow" x="1400" y="550" size="30" color="#cbd5e1" />

      <Element
        x="140"
        y="640"
        w="380"
        h="300"
        color="#ffffff"
        :shader="{type: 'linearGradient', angle: $angle, colors: ['#f97316', '#db2777', '#6d28d9']}"
      />
      <Text content="linearGradient (animated)" x="140" y="960" size="30" color="#cbd5e1" />

      <Element
        x="560"
        y="640"
        w="380"
        h="300"
        color="#ffffff"
        shader="{type: 'radialGradient', colors: ['#fde047', '#166534'], w: 260, h: 220}"
      />
      <Text content="radialGradient" x="560" y="960" size="30" color="#cbd5e1" />

      <Element
        x="980"
        y="640"
        w="380"
        h="300"
        color="#e2e8f0"
        shader="{type: 'holePunch', x: 110, y: 70, w: 160, h: 160, radius: 80}"
      />
      <Text content="holePunch" x="980" y="960" size="30" color="#cbd5e1" />

      <Element
        x="1400"
        y="640"
        w="380"
        h="300"
        color="#ffffff"
        shader="{type: 'plasma', speed: 1.2, colorA: '#22d3ee', colorB: '#7c3aed'}"
      />
      <Text content="custom: plasma (u_time)" x="1400" y="960" size="30" color="#cbd5e1" />
    </Element>
  `,
  state() {
    return {
      angle: 0,
    }
  },
  hooks: {
    ready() {
      // A full turn every 6 seconds, in radians -- LinearGradient's unit.
      setInterval(() => {
        this.angle = (this.angle + Math.PI / 90) % (Math.PI * 2)
      }, 50)
    },
  },
})
