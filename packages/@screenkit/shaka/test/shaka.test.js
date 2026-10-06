// @screenkit/shaka under node: configuration as Shaka merges it, its error
// codes, the networking engine's filters and retries, and a Player driving a
// scripted element controller -- the runtime's side, which the media-* rows
// cover against real players (runtime/tests/RuntimeTests.cpp).

import assert from 'node:assert/strict'
import { describe, test } from 'node:test'

import shaka from '../src/index.js'

const { Player } = shaka
const { Error: ShakaError } = shaka.util
const MEDIA = Symbol.for('screenkit.media')

// A <video> as the runtime's DOM shim presents one to the Shaka layer: the
// controller at Symbol.for('screenkit.media'), and the element events the
// Player listens to. `script` answers each load.
function fakeVideo(script) {
  const listeners = []
  const elementListeners = new Map()
  let tracks = { variants: [], audio: [], text: [] }
  let info = { live: false, seekStart: 0, seekEnd: 0, duration: NaN, variant: -1, buffering: false, manifest: '' }
  const calls = []
  const video = {
    paused: true, ended: false, currentTime: 0, duration: NaN, playbackRate: 1, defaultPlaybackRate: 1,
    buffered: { length: 0 },
    addEventListener(type, fn) { elementListeners.set(type, [...(elementListeners.get(type) || []), fn]) },
    removeEventListener(type, fn) { elementListeners.set(type, (elementListeners.get(type) || []).filter((f) => f !== fn)) },
  }
  const emit = (type, payload) => listeners.slice().forEach((l) => l(type, payload))
  video[MEDIA] = {
    capabilities: () => ({ available: true, platform: 'node', hls: true, dash: false, progressive: true,
                           keySystems: ['com.widevine.alpha'], containers: ['video/mp4'], codecs: ['avc1'],
                           videoOutput: true, videoOutputProblem: '' }),
    load(options) {
      calls.push(['load', options])
      return script(options, {
        setTracks: (t) => { tracks = t; emit('tracks', t) },
        setInfo: (i) => { info = { ...info, ...i } },
        emit,
      })
    },
    unload() { calls.push(['unload']) },
    info: () => info,
    tracks: () => tracks,
    stats: () => ({ decodedFrames: 42, droppedFrames: 1, corruptedFrames: 0, width: 640, height: 360,
                    streamBandwidth: 800000, estimatedBandwidth: 1e6 }),
    selectVariant: (id) => calls.push(['selectVariant', id]),
    setAbr: (abr) => calls.push(['setAbr', abr]),
    selectAudioLanguage: (l) => calls.push(['selectAudioLanguage', l]),
    selectText: (id, visible) => calls.push(['selectText', id, visible]),
    provideLicence: (id, bytes) => calls.push(['provideLicence', id, bytes]),
    listen(fn) {
      listeners.push(fn)
      return () => listeners.splice(listeners.indexOf(fn), 1)
    },
  }
  return { video, calls, emit }
}

const twoVariants = {
  variants: [
    { id: 0, bandwidth: 450000, width: 320, height: 180, videoCodec: 'avc1.4d4015', audioCodec: 'mp4a.40.2', language: '' },
    { id: 1, bandwidth: 1000000, width: 640, height: 360, videoCodec: 'avc1.4d401e', audioCodec: 'mp4a.40.2', language: '' },
  ],
  audio: [],
  text: [{ id: 5, language: 'en', label: 'English', kind: 'subtitles', mimeType: 'text/vtt' }],
}

function playsHls(options, { setTracks, setInfo }) {
  setInfo({ duration: 10, seekEnd: 10, variant: 1, manifest: 'hls' })
  setTracks(twoVariants)
  return Promise.resolve()
}

describe('configuration', () => {
  test('merges as Shaka does: unknown keys and wrong types are refused, the rest applied', () => {
    const player = new Player()
    assert.equal(player.configure({ abr: { enabled: false }, bogus: 1 }), false)
    assert.equal(player.getConfiguration().abr.enabled, false)
    assert.equal(player.configure('streaming.bufferingGoal', 'ten'), false)
    assert.equal(player.getConfiguration().streaming.bufferingGoal, 10)
    assert.equal(player.configure('abr.enabled', undefined), true)
    assert.equal(player.getConfiguration().abr.enabled, true)
  })

  test('drm maps take any key, merge, and a key set to undefined goes', () => {
    const player = new Player()
    player.configure({ drm: { servers: { 'com.widevine.alpha': 'https://a' } } })
    player.configure({ drm: { servers: { 'com.apple.fps': 'https://b' } } })
    assert.deepEqual(player.getConfiguration().drm.servers, { 'com.widevine.alpha': 'https://a', 'com.apple.fps': 'https://b' })
    player.configure({ drm: { servers: { 'com.apple.fps': undefined } } })
    assert.deepEqual(player.getConfiguration().drm.servers, { 'com.widevine.alpha': 'https://a' })
    player.configure({ drm: { advanced: { 'com.apple.fps': { serverCertificateUri: 'https://c', videoRobustness: 'HW' } } } })
    const advanced = player.getConfiguration().drm.advanced['com.apple.fps']
    assert.equal(advanced.serverCertificateUri, 'https://c')
    assert.deepEqual(advanced.videoRobustness, ['HW'])
  })

  test('takes Shaka 4 preference names into Shaka 5 preferences', () => {
    const player = new Player()
    player.configure({ preferredAudioLanguage: 'fr', preferredTextLanguage: 'de' })
    const config = player.getConfiguration()
    assert.equal(config.preferredAudio[0].language, 'fr')
    assert.equal(config.preferredText[0].language, 'de')
  })

  test('getConfiguration is a copy', () => {
    const player = new Player()
    player.getConfiguration().abr.enabled = false
    assert.equal(player.getConfiguration().abr.enabled, true)
  })
})

describe('shaka.util.Error', () => {
  test('carries Shaka\'s category, code, severity and data', () => {
    const error = new ShakaError(ShakaError.Severity.CRITICAL, ShakaError.Category.NETWORK,
                                 ShakaError.Code.BAD_HTTP_STATUS, 'https://x', 404)
    assert.deepEqual([error.severity, error.category, error.code, error.data], [2, 1, 1001, ['https://x', 404]])
    assert.match(error.message, /NETWORK\.BAD_HTTP_STATUS/)
    for (const [name, code] of [['UNABLE_TO_GUESS_MANIFEST_TYPE', 4000], ['VIDEO_ERROR', 3016],
                                ['REQUESTED_KEY_SYSTEM_CONFIG_UNAVAILABLE', 6001], ['LICENSE_REQUEST_FAILED', 6007],
                                ['LOAD_INTERRUPTED', 7000], ['NO_VIDEO_ELEMENT', 7002], ['HTTP_ERROR', 1002]]) {
      assert.equal(ShakaError.Code[name], code, name)
    }
  })
})

describe('the networking engine', () => {
  test('runs request and response filters around fetch, and retries a 5xx', async (t) => {
    const seen = []
    t.mock.method(globalThis, 'fetch', async (uri, init) => {
      seen.push([uri, init.method, init.headers['x-filter']])
      const status = seen.length === 1 ? 503 : 200
      return new Response(new Uint8Array([1, 2, 3]), { status, headers: { 'X-Answer': 'yes' } })
    })
    const engine = new shaka.net.NetworkingEngine()
    engine.registerRequestFilter((type, request) => { request.headers['x-filter'] = `type-${type}` })
    engine.registerResponseFilter((type, response) => { response.data = new Uint8Array([9]).buffer })
    const request = shaka.net.NetworkingEngine.makeRequest(['https://licence'],
      { maxAttempts: 2, baseDelay: 1, backoffFactor: 1, fuzzFactor: 0, timeout: 1000 })
    request.method = 'POST'
    const response = await engine.request(shaka.net.NetworkingEngine.RequestType.LICENSE, request).promise
    assert.deepEqual(seen, [['https://licence', 'POST', 'type-2'], ['https://licence', 'POST', 'type-2']])
    assert.equal(response.status, 200)
    assert.equal(response.headers['x-answer'], 'yes')
    assert.deepEqual([...new Uint8Array(response.data)], [9])
  })

  test('a failed request is Shaka\'s 1001 with the status, and a filter that throws is 1006', async (t) => {
    t.mock.method(globalThis, 'fetch', async () => new Response('nope', { status: 404 }))
    const engine = new shaka.net.NetworkingEngine()
    const request = shaka.net.NetworkingEngine.makeRequest(['https://x'], { maxAttempts: 1, baseDelay: 1, backoffFactor: 1, fuzzFactor: 0, timeout: 1000 })
    await assert.rejects(engine.request(0, request).promise, (e) => e.code === 1001 && e.data[1] === 404 && e.data[2] === 'nope')
    engine.registerRequestFilter(() => { throw new Error('refused') })
    await assert.rejects(engine.request(0, shaka.net.NetworkingEngine.makeRequest(['https://x'])).promise,
                         (e) => e.code === 1006)
  })
})

describe('Player', () => {
  test('attach, load, tracks, stats, destroy', async () => {
    const { video, calls } = fakeVideo(playsHls)
    const player = new Player()
    const events = []
    for (const type of ['loading', 'loaded', 'trackschanged', 'unloading']) {
      player.addEventListener(type, () => events.push(type))
    }
    await player.attach(video)
    await player.load('https://cdn/master.m3u8')
    assert.deepEqual(events, ['loading', 'trackschanged', 'loaded'])
    assert.equal(calls[0][1].mimeType, 'application/x-mpegurl')
    assert.equal(player.getLoadMode(), Player.LoadMode.SRC_EQUALS)
    assert.equal(player.getManifestType(), 'HLS')
    assert.deepEqual(player.getVariantTracks().map((t) => [t.id, t.active, t.height]), [[0, false, 180], [1, true, 360]])
    assert.deepEqual(player.getTextTracks().map((t) => [t.id, t.language, t.kind]), [[5, 'en', 'subtitle']])
    assert.deepEqual(player.seekRange(), { start: 0, end: 10 })
    assert.equal(player.getStats().decodedFrames, 42)
    player.selectTextTrack(player.getTextTracks()[0])
    assert.deepEqual(calls.at(-1), ['selectText', 5, true])
    await player.destroy()
    assert.ok(calls.some((c) => c[0] === 'unload'))
    await assert.rejects(player.load('https://cdn/master.m3u8'), (e) => e.code === 7003)
  })

  test('with no element, load is 7002; a load replaced by another is 7000', async () => {
    await assert.rejects(new Player().load('https://x.m3u8'), (e) => e.code === 7002)
    let release
    const { video } = fakeVideo((options, api) => {
      if (options.url.includes('slow')) return new Promise((resolve, reject) => { release = reject })
      return playsHls(options, api)
    })
    const player = new Player()
    await player.attach(video)
    const first = player.load('https://cdn/slow.m3u8')
    const second = player.load('https://cdn/master.m3u8')
    await assert.rejects(first, (e) => e.code === 7000 && e.severity === 2)
    await second
    // The first never reached the platform: it was superseded while choosing its key system.
    assert.equal(release, undefined)
  })

  test('a platform failure is Shaka\'s error, and the player fires `error` too', async () => {
    const { video } = fakeVideo(() => Promise.reject({ kind: 'network', httpStatus: 404, message: 'HTTP 404' }))
    const player = new Player()
    const errors = []
    player.addEventListener('error', (e) => errors.push(e.detail.code))
    await player.attach(video)
    await assert.rejects(player.load('https://cdn/missing.m3u8'), (e) => e.code === 1001 && e.data[1] === 404)
    assert.deepEqual(errors, [1001])
  })

  test('a key system the platform lacks is 6001; DASH it cannot play is 4000', async () => {
    const { video } = fakeVideo(playsHls)
    const player = new Player()
    await player.attach(video)
    player.configure({ drm: { clearKeys: { '00112233445566778899aabbccddeeff': 'ffeeddccbbaa99887766554433221100' } } })
    await assert.rejects(player.load('https://cdn/stream.mpd'), (e) => e.code === 6001)
    player.resetConfiguration()
    await assert.rejects(player.load('https://cdn/stream.mpd'), (e) => e.code === 4000 && e.category === 4)
  })

  test('a licence request goes through the filters and its answer back to the key system', async (t) => {
    const bodies = []
    t.mock.method(globalThis, 'fetch', async (uri, init) => {
      bodies.push([uri, init.headers['x-filter'], new TextDecoder().decode(init.body)])
      return new Response('licence-bytes', { status: 200 })
    })
    const { video, calls, emit } = fakeVideo((options, api) => {
      setTimeout(() => emit('licence', { requestId: 7, keySystem: 'com.widevine.alpha',
                                         challenge: new TextEncoder().encode('challenge').buffer, contentId: '' }), 0)
      return new Promise((resolve) => setTimeout(() => { playsHls(options, api); resolve() }, 20))
    })
    const player = new Player()
    await player.attach(video)
    player.configure({ drm: { servers: { 'com.widevine.alpha': 'https://licence/server' } } })
    player.getNetworkingEngine().registerRequestFilter((type, request) => {
      if (type === shaka.net.NetworkingEngine.RequestType.LICENSE) request.headers['x-filter'] = 'yes'
    })
    await player.load('https://cdn/master.m3u8')
    assert.deepEqual(bodies, [['https://licence/server', 'yes', 'challenge']])
    const provided = calls.find((c) => c[0] === 'provideLicence')
    assert.equal(provided[1], 7)
    assert.equal(new TextDecoder().decode(new Uint8Array(provided[2])), 'licence-bytes')
    assert.equal(calls[0][1].drm.keySystem, 'com.widevine.alpha')
  })

  test('isBrowserSupported asks the runtime', () => {
    assert.equal(Player.isBrowserSupported(), false)
    globalThis.__screenkit = { media: { capabilities: () => ({ available: true }) } }
    try {
      assert.equal(Player.isBrowserSupported(), true)
    } finally {
      delete globalThis.__screenkit
    }
    assert.equal(typeof shaka.polyfill.installAll, 'function')
  })
})
