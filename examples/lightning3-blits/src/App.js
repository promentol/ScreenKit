import Blits from '@lightningjs/blits'

// Blits colours are hex or rgb(a); hsl() is parsed but rejected at runtime
// ("HSL(A) color format is not supported yet"), so the cycle converts here.
const hsl = (h, s, l) => {
  const a = s * Math.min(l, 1 - l)
  const channel = (n) => {
    const k = (n + h / 30) % 12
    const v = l - a * Math.max(-1, Math.min(k - 3, 9 - k, 1))
    return Math.round(v * 255).toString(16).padStart(2, '0')
  }
  return `#${channel(0)}${channel(8)}${channel(4)}`
}

// The whole app. Blits templates are strings, precompiled by the Vite plugin.
// Attribute values in {} are reactive expressions bound to state.
export default Blits.Component('App', {
  template: `
    <Element w="1920" h="1080" color="#0a0a14">
      <Text
        content="Hello World"
        x="960"
        y="470"
        mount="{ x: 0.5, y: 0.5 }"
        size="110"
        :color="$titleColor"
      />
      <Text
        content="Lightning 3 + Blits on ScreenKit"
        x="960"
        y="580"
        mount="{ x: 0.5, y: 0.5 }"
        size="36"
        color="#64748b"
      />
      <Text
        content="Canvas text: a web font through the 2D context"
        x="960"
        y="760"
        mount="{ x: 0.5, y: 0.5 }"
        size="40"
        font="lato-web"
        color="#fbbf24"
      />
      <Element
        w="220"
        h="6"
        x="960"
        y="650"
        mount="{ x: 0.5, y: 0.5 }"
        :color="$barColor"
        :effects="[{ type: 'radius', props: { radius: 3 } }]"
      />
    </Element>
  `,
  state() {
    return {
      titleColor: '#4ade80',
      barColor: '#4ade80',
      hue: 0,
    }
  },
  hooks: {
    ready() {
      // A frame-driven colour cycle: proof the render loop is live rather than a
      // single painted frame. Lightning drives this from its own raf loop.
      setInterval(() => {
        this.hue = (this.hue + 4) % 360
        this.titleColor = hsl(this.hue, 0.7, 0.62)
        this.barColor = hsl((this.hue + 40) % 360, 0.7, 0.62)
      }, 50)
    },
  },
})
