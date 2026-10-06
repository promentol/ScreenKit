import Phaser from 'phaser'

// A plain Phaser scene: a logo spinning and pulsing above a line of bitmap text,
// on a fullscreen canvas. The font is a pre-generated bitmap font
// (public/assets/lato-black-plain.xml, Lato Black under the SIL Open Font
// License), drawn through WebGL; the caption is ordinary canvas text.
class Hello extends Phaser.Scene {
  preload() {
    this.load.image('logo', '/assets/logo.png')
    this.load.bitmapFont('lato', '/assets/lato-black-plain.png', '/assets/lato-black-plain.xml')
  }

  create() {
    this.logo = this.add.image(0, 0, 'logo')
    this.title = this.add.bitmapText(0, 0, 'lato', 'Hello, Phaser!', 64).setOrigin(0.5)
    this.caption = this.add.text(0, 0, 'Canvas text: Text in Courier', {
      fontFamily: 'Courier', fontSize: '40px', color: '#ffd166', stroke: '#000000', strokeThickness: 4,
    }).setOrigin(0.5)
    this.tweens.add({ targets: this.logo, angle: 360, duration: 4000, repeat: -1 })
    this.tweens.add({ targets: this.logo, scale: 1.15, duration: 900, yoyo: true, repeat: -1, ease: 'Sine.easeInOut' })

    this.layout(this.scale.gameSize)
    this.scale.on('resize', this.layout, this)
  }

  // Laid out on a 720-unit-tall design, which the camera zooms to the screen's
  // height, so a 640x480 screen shows the same picture as a 1080p one.
  layout(gameSize) {
    const zoom = gameSize.height / 720
    const centre = gameSize.width / zoom / 2
    this.cameras.main.setOrigin(0, 0).setZoom(zoom)
    this.logo.setPosition(centre, 270)
    this.title.setPosition(centre, 510)
    this.caption.setPosition(centre, 610)
  }
}

new Phaser.Game({
  type: Phaser.WEBGL,
  backgroundColor: '#101826',
  scale: { mode: Phaser.Scale.RESIZE, width: window.innerWidth, height: window.innerHeight },
  scene: Hello,
})
