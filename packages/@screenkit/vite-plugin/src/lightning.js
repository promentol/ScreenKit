// Build-time fixes a Lightning 3 / Blits app needs to run on ScreenKit.
//
// Part of every screenkit() build (index.js). They are ScreenKit's platform, not
// patches to the app: the app's source and its npm packages stay untouched, and
// the fix exists because of a runtime property: Hermes canonicalises NaN bit
// patterns. (Text needs none: the 2D context draws it, so Blits' canvas and web
// fonts work as they do in a browser.)
//
// Each rewrites one specific module, matched by id, and fails the build loudly in
// both ways a fix can silently stop applying:
//
//   - the module changed shape, so the code it rewrites is gone   -> transform
//   - the module moved or was renamed, so it is never matched     -> buildEnd
//
// The second is decided per build: a fix whose package (`@lightningjs/blits/`,
// `@lightningjs/renderer/`) was in the module graph but whose target module never
// went through the transform fails the build, naming the fix and the path it
// expected. An app that does not use Lightning never loads those packages, so for
// it the fixes are inert.
//
// The fixes are factories, called once per screenkit() -- the "seen" bookkeeping
// belongs to one build and must not leak into another's.

/** Forward slashes, no query: how a module id is compared. */
function normalise(id) {
  return id.replace(/\\/g, '/').split('?')[0]
}

/**
 * One fix. `rewrite(code)` is called with the plugin context as `this`, for the
 * target module only, and returns the new code (or calls this.error).
 */
function fix({ name, pkg, target, rewrite, consequence }) {
  let packageSeen = false
  let targetTransformed = false
  return {
    name,
    enforce: 'pre',
    transform(code, id) {
      const path = normalise(id)
      // Not anchored at node_modules: a linked or workspace copy resolves to its
      // real path, which still names the package.
      if (path.includes(`/${pkg}/`)) packageSeen = true
      if (!path.endsWith(`/${pkg}/${target}`)) return null
      targetTransformed = true
      return { code: rewrite.call(this, code), map: null }
    },
    buildEnd(error) {
      // A build that already failed has its own error; do not bury it.
      if (error || !packageSeen || targetTransformed) return
      this.error(
        `${name}: ${pkg} is in this build, but ${pkg}/${target} was never transformed -- it moved or was ` +
          `renamed, so this fix no longer applies and ${consequence}. Find where that code lives now and ` +
          'update @screenkit/vite-plugin.',
      )
    },
  }
}

// Lightning's partial quad upload copies each dirty quad into a scratch array
// element by element through Float32Array views:
//
//   for (let j = 0; j < 20; j++) scratch[j] = f[slot + j]
//
// Four of those twenty slots are not floats. They are packed ABGR colours
// written through a Uint32Array over the same buffer. Any colour whose blue byte
// is >= 0x80 has a NaN bit pattern when read as a float. V8 and JSC keep a
// NaN's bits through a read and a write; Hermes cannot, because its values are
// NaN-boxed and every NaN entering the VM is canonicalised to 0x7FC00000. The
// colour arrives as rgb(0,0,192) at half alpha -- measured on screen as
// (5,5,202) over the background, the moment a quad's colour first changes.
//
// TypedArray.prototype.set copies bytes without surfacing them as JS numbers, so
// it is bit-exact in every engine, and it is one call instead of twenty.
export function hermesBitExactQuadCopy() {
  return fix({
    name: 'screenkit-hermes-bit-exact-quad-copy',
    pkg: '@lightningjs/renderer',
    target: 'dist/src/core/renderers/webgl/WebGlRenderer.js',
    consequence: 'quads will draw wrong colours on Hermes once their colour changes',
    rewrite(code) {
      const loop = /for \(let j = 0; j < 20; j\+\+\) \{\s*scratch\[j\] = f\[slot \+ j\];\s*\}/
      if (!loop.test(code)) {
        this.error('screenkit-hermes-bit-exact-quad-copy: WebGlRenderer.js changed; the element-wise ' +
                   'quad copy is gone or reshaped -- check it still cannot mangle colours, then update this plugin')
      }
      return code.replace(loop, 'scratch.set(f.subarray(slot, slot + 20));')
    },
  })
}

// Blits scales a 1080p-authored app to the screen through a lookup table:
//
//   SCREEN_RESOLUTIONS = { 720: 0.66666667, 1080: 1, 2160: 2 }
//
// which is `screenHeight / 1080` written out for the three heights a TV has --
// but only those three. Every other height misses the table and falls through to
// `|| 1`, so the app draws its 1920x1080 stage at 1:1 into whatever canvas it was
// given. On a Raspberry Pi at 640x480 that is the top-left quarter of the UI,
// with no error anywhere (EMBEDDED_LINUX_EXPERIMENTS.md).
//
// A browser makes the table nearly sufficient, because a window can be any size
// but a TV panel is one of three. ScreenKit's canvas is always the full drawable
// and the drawable is the device's, not the app author's, so heights off the
// table are the normal case rather than the exception.
//
// So the last fallback computes what the table would have held. The author's own
// `pixelRatio` and `screenResolution` still win, and for 720/1080/2160 the table
// answers first with exactly the same number -- this only replaces the `|| 1`.
//
// It fits both axes, not just the height Blits looks at. The table can use height
// alone because a TV panel and a 1080p-authored app are both 16:9, and ScreenKit's
// `--size` drawables need not be: at 640x480 a height fit gives 480/1080 = 0.444
// and a 1920-wide stage becomes 853 -- still wider than the 640 it has. The
// smaller of the two ratios is the one that fits, which is `object-fit: contain`
// and matches what the host does with the drawable itself (pi.sh, Host.cpp).
export const BLITS_CONTAIN_FIT =
  '(function () {\n' +
  '  var view = (settings.platform || platform).viewport\n' +
  '  var width = view && view.innerWidth\n' +
  '  if (!screenHeight || !width) return 1\n' +
  '  return Math.min(width / (settings.w || 1920), screenHeight / (settings.h || 1080))\n' +
  '})(),'

export function blitsFitsTheDrawable() {
  return fix({
    name: 'screenkit-blits-fits-the-drawable',
    pkg: '@lightningjs/blits',
    target: 'src/engines/L3/launch.js',
    consequence: 'a 1080p Blits app will render cropped on any drawable Blits has no table entry for',
    rewrite(code) {
      const fallback =
        /(deviceLogicalPixelRatio:\s*settings\.pixelRatio \|\|\s*SCREEN_RESOLUTIONS\[settings\.screenResolution\] \|\|\s*SCREEN_RESOLUTIONS\[screenHeight\] \|\|\s*)1,/
      if (!fallback.test(code)) {
        this.error('screenkit-blits-fits-the-drawable: L3/launch.js changed; the deviceLogicalPixelRatio ' +
                   'fallback chain is gone or reshaped -- check how Blits picks a scale now, then update this plugin')
      }
      // `platform` and `settings` are both in scope at the call site; the fix reads
      // the viewport through the same object Blits took `screenHeight` from.
      return code.replace(fallback, '$1' + BLITS_CONTAIN_FIT)
    },
  })
}

/**
 * Every fix, fresh for one build, in the order they must run (all
 * `enforce: 'pre'`, ahead of Blits' own plugins).
 */
export function lightningOnScreenKit() {
  return [hermesBitExactQuadCopy(), blitsFitsTheDrawable()]
}
