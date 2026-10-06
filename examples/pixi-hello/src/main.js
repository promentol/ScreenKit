import { Application, Assets, BitmapText, Container, Sprite, Text } from 'pixi.js'

// A plain PixiJS app: a spinning logo and a line of text on a fullscreen canvas.
// The title is a pre-generated MSDF bitmap font (public/assets/lato-black-msdf.xml,
// Lato Black under the SIL Open Font License), which renders through WebGL alone;
// the caption is ordinary canvas text in an installed font.
;(async () => {
  const app = new Application()
  await app.init({ background: '#101826', resizeTo: window, antialias: true })
  document.body.appendChild(app.canvas)

  // Decode textures on the main thread. Pixi otherwise loads them in a Web
  // Worker, and ScreenKit has no Workers (runtime/js/README.md); browsers are
  // unaffected either way.
  Assets.setPreferences({ preferWorkers: false })
  await Assets.load(['/assets/logo.png', '/assets/lato-black-msdf.xml'])

  // Laid out on a 720-unit-tall design and scaled to the screen's height, so a
  // 640x480 screen shows the same picture as a 1080p one.
  const scene = new Container()
  app.stage.addChild(scene)

  const logo = Sprite.from('/assets/logo.png')
  logo.anchor.set(0.5)
  scene.addChild(logo)

  const title = new BitmapText({
    text: 'Hello, PixiJS!',
    style: { fontFamily: 'Lato-Black', fontSize: 96, fill: '#ffffff' },
  })
  title.anchor.set(0.5)
  scene.addChild(title)

  const caption = new Text({
    text: 'Canvas text: Text in Helvetica',
    style: { fontFamily: 'Helvetica, Arial, sans-serif', fontSize: 40, fill: '#ffd166', stroke: { color: '#000000', width: 4 } },
  })
  caption.anchor.set(0.5)
  scene.addChild(caption)

  const layout = () => {
    const scale = app.screen.height / 720
    const centre = app.screen.width / scale / 2
    scene.scale.set(scale)
    logo.position.set(centre, 270)
    title.position.set(centre, 510)
    caption.position.set(centre, 610)
  }
  layout()
  app.renderer.on('resize', layout)

  let elapsed = 0
  app.ticker.add((ticker) => {
    elapsed += ticker.deltaMS / 1000
    logo.rotation = elapsed
    logo.scale.set(1 + Math.sin(elapsed * 2) * 0.08)
  })
})()
