// Copyright (c) ScreenKit contributors. MIT.
//
// @screenkit/shaka -- a player whose API mirrors Shaka Player's (v5), over the
// ScreenKit runtime's platform players.
//
// Real Shaka cannot run in ScreenKit: it parses manifests and feeds segments to
// MediaSource Extensions, and the runtime has no MSE. So this does what
// react-native-video does instead -- the platform's own player owns manifests,
// buffering, ABR, decoding, DRM and presentation (AVPlayer on Apple, Media3
// ExoPlayer on Android, the image's libvlc on Linux) -- and keeps
// Shaka's names, shapes, events and error codes on top, so Shaka code such as
// the Blits example's PlayerManager.js runs unmodified. @screenkit/vite-plugin
// resolves `shaka-player` (and its subpaths) to this package.
//
// It drives a <video> through the controller the runtime's DOM shim hangs off
// every media element, `video[Symbol.for('screenkit.media')]`
// (runtime/js/dom-shim.js), and it performs the one kind of request the
// platform player cannot: the licence exchange, which goes through this
// player's networking engine so request and response filters apply.
//
// What differs from Shaka, deliberately and recorded (runtime/js/README.md,
// "Video"): manifests and segments are the platform player's, so request
// filters see only licence and certificate requests; the load mode is
// SRC_EQUALS; external text, thumbnails, chapters, preload, offline storage,
// ads, cast and the UI are absent; a configured key system the platform lacks
// fails the load with 6001 whether or not the content is encrypted.
//
// Written as a plain script body up to the module-exports marker at the end:
// runtime/tests evaluates everything above that marker as a script.

const shaka = {};

// ---- util: bytes and strings ----------------------------------------------------------

function toUint8(data) {
  if (data instanceof Uint8Array) return data;
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (data && data.buffer instanceof ArrayBuffer) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  return new Uint8Array(0);
}
function toArrayBuffer(data) {
  const bytes = toUint8(data);
  return bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
    ? bytes.buffer : bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
}

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function base64Encode(bytes, padding = true) {
  let out = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const n = (bytes[i] << 16) | ((bytes[i + 1] || 0) << 8) | (bytes[i + 2] || 0);
    out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63];
    out += i + 1 < bytes.length ? B64[(n >> 6) & 63] : (padding ? '=' : '');
    out += i + 2 < bytes.length ? B64[n & 63] : (padding ? '=' : '');
  }
  return out;
}
function base64Decode(text) {
  const clean = String(text).replace(/-/g, '+').replace(/_/g, '/').replace(/[^A-Za-z0-9+/]/g, '');
  const out = new Uint8Array(Math.floor(clean.length * 3 / 4));
  let o = 0;
  for (let i = 0; i < clean.length; i += 4) {
    const n = (B64.indexOf(clean[i]) << 18) | (B64.indexOf(clean[i + 1]) << 12) |
              ((i + 2 < clean.length ? B64.indexOf(clean[i + 2]) : 0) << 6) |
              (i + 3 < clean.length ? B64.indexOf(clean[i + 3]) : 0);
    if (o < out.length) out[o++] = (n >> 16) & 255;
    if (i + 2 < clean.length && o < out.length) out[o++] = (n >> 8) & 255;
    if (i + 3 < clean.length && o < out.length) out[o++] = n & 255;
  }
  return out.subarray(0, o);
}

const Uint8ArrayUtils = {
  toBase64: (data, padding = true) => base64Encode(toUint8(data), padding).replace(/\+/g, '-').replace(/\//g, '_'),
  toStandardBase64: (data, padding = true) => base64Encode(toUint8(data), padding),
  fromBase64: (text) => base64Decode(text),
  toHex: (data) => Array.from(toUint8(data), (b) => (b < 16 ? '0' : '') + b.toString(16)).join(''),
  fromHex: (text) => {
    const out = new Uint8Array(Math.floor(String(text).length / 2));
    for (let i = 0; i < out.length; i++) out[i] = parseInt(String(text).substr(i * 2, 2), 16);
    return out;
  },
  equal: (a, b) => {
    const x = toUint8(a), y = toUint8(b);
    if (x.length !== y.length) return false;
    for (let i = 0; i < x.length; i++) if (x[i] !== y[i]) return false;
    return true;
  },
  concat: (...arrays) => {
    const parts = arrays.map(toUint8);
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let at = 0;
    for (const p of parts) { out.set(p, at); at += p.length; }
    return out;
  },
};

const StringUtils = {
  fromUTF8: (data) => new TextDecoder().decode(toUint8(data)),
  toUTF8: (text) => toArrayBuffer(new TextEncoder().encode(String(text))),
  fromUTF16: (data, littleEndian) => {
    const bytes = toUint8(data);
    let out = '';
    for (let i = 0; i + 1 < bytes.length; i += 2) {
      out += String.fromCharCode(littleEndian ? bytes[i] | (bytes[i + 1] << 8) : (bytes[i] << 8) | bytes[i + 1]);
    }
    return out;
  },
  toUTF16: (text, littleEndian) => {
    const value = String(text);
    const out = new Uint8Array(value.length * 2);
    for (let i = 0; i < value.length; i++) {
      const c = value.charCodeAt(i);
      out[i * 2] = littleEndian ? c & 255 : c >> 8;
      out[i * 2 + 1] = littleEndian ? c >> 8 : c & 255;
    }
    return toArrayBuffer(out);
  },
  fromBytesAutoDetect: (data) => {
    const bytes = toUint8(data);
    if (bytes.length >= 2 && bytes[0] === 0xfe && bytes[1] === 0xff) return StringUtils.fromUTF16(bytes.subarray(2), false);
    if (bytes.length >= 2 && bytes[0] === 0xff && bytes[1] === 0xfe) return StringUtils.fromUTF16(bytes.subarray(2), true);
    if (bytes.length >= 3 && bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf) {
      return StringUtils.fromUTF8(bytes.subarray(3));
    }
    return StringUtils.fromUTF8(bytes);
  },
};

const BufferUtils = {
  toUint8,
  toArrayBuffer,
  equal: (a, b) => (a == null && b == null) || (a != null && b != null && Uint8ArrayUtils.equal(a, b)),
  toDataView: (data) => { const b = toUint8(data); return new DataView(b.buffer, b.byteOffset, b.byteLength); },
};

// ---- shaka.log --------------------------------------------------------------------------

const Level = { NONE: 0, ERROR: 1, WARNING: 2, INFO: 3, DEBUG: 4, V1: 5, V2: 6 };
let logLevel = Level.WARNING;
const warnedOnce = new Set();
const log = {
  Level,
  setLevel: (level) => { logLevel = level; },
  alwaysError: (...args) => console.error(...args),
  alwaysWarn: (...args) => console.warn(...args),
  warnOnce: (id, ...args) => { if (!warnedOnce.has(id)) { warnedOnce.add(id); console.warn(...args); } },
  error: (...args) => { if (logLevel >= Level.ERROR) console.error(...args); },
  warning: (...args) => { if (logLevel >= Level.WARNING) console.warn(...args); },
  info: (...args) => { if (logLevel >= Level.INFO) console.info(...args); },
  debug: (...args) => { if (logLevel >= Level.DEBUG) console.log(...args); },
  v1: (...args) => { if (logLevel >= Level.V1) console.log(...args); },
  v2: (...args) => { if (logLevel >= Level.V2) console.log(...args); },
};

// ---- shaka.util.Error -------------------------------------------------------------------

const Severity = { RECOVERABLE: 1, CRITICAL: 2 };
const Category = { NETWORK: 1, TEXT: 2, MEDIA: 3, MANIFEST: 4, STREAMING: 5, DRM: 6, PLAYER: 7, CAST: 8, STORAGE: 9,
                   ADS: 10 };
// Shaka's own numbering, every code (lib/util/error.js).
const Code = {
  UNSUPPORTED_SCHEME: 1000, BAD_HTTP_STATUS: 1001, HTTP_ERROR: 1002, TIMEOUT: 1003, MALFORMED_DATA_URI: 1004,
  UNKNOWN_DATA_URI_ENCODING: 1005, REQUEST_FILTER_ERROR: 1006, RESPONSE_FILTER_ERROR: 1007,
  MALFORMED_TEST_URI: 1008, UNEXPECTED_TEST_REQUEST: 1009, ATTEMPTS_EXHAUSTED: 1010, SEGMENT_MISSING: 1011,
  INVALID_TEXT_HEADER: 2000, INVALID_TEXT_CUE: 2001, INVALID_TEXT_SETTINGS: 2002,
  UNABLE_TO_DETECT_ENCODING: 2003, BAD_ENCODING: 2004, INVALID_XML: 2005, INVALID_TTML: 2006,
  INVALID_MP4_TTML: 2007, INVALID_MP4_VTT: 2008, UNABLE_TO_EXTRACT_CUE_START_TIME: 2009, INVALID_MP4_CEA: 2010,
  TEXT_COULD_NOT_GUESS_MIME_TYPE: 2011, CANNOT_ADD_EXTERNAL_TEXT_TO_SRC_EQUALS: 2012,
  TEXT_ONLY_WEBVTT_SRC_EQUALS: 2013, MISSING_TEXT_PLUGIN: 2014, CHAPTERS_TRACK_FAILED: 2015,
  CANNOT_ADD_EXTERNAL_THUMBNAILS_TO_SRC_EQUALS: 2016, UNSUPPORTED_EXTERNAL_THUMBNAILS_URI: 2017,
  BUFFER_READ_OUT_OF_BOUNDS: 3000, JS_INTEGER_OVERFLOW: 3001, EBML_OVERFLOW: 3002,
  EBML_BAD_FLOATING_POINT_SIZE: 3003, MP4_SIDX_WRONG_BOX_TYPE: 3004, MP4_SIDX_INVALID_TIMESCALE: 3005,
  MP4_SIDX_TYPE_NOT_SUPPORTED: 3006, WEBM_CUES_ELEMENT_MISSING: 3007, WEBM_EBML_HEADER_ELEMENT_MISSING: 3008,
  WEBM_SEGMENT_ELEMENT_MISSING: 3009, WEBM_INFO_ELEMENT_MISSING: 3010, WEBM_DURATION_ELEMENT_MISSING: 3011,
  WEBM_CUE_TRACK_POSITIONS_ELEMENT_MISSING: 3012, WEBM_CUE_TIME_ELEMENT_MISSING: 3013,
  MEDIA_SOURCE_OPERATION_FAILED: 3014, MEDIA_SOURCE_OPERATION_THREW: 3015, VIDEO_ERROR: 3016,
  QUOTA_EXCEEDED_ERROR: 3017, TRANSMUXING_FAILED: 3018, CONTENT_TRANSFORMATION_FAILED: 3019,
  MSS_MISSING_DATA_FOR_TRANSMUXING: 3020, MSS_TRANSMUXING_CODEC_UNKNOWN: 3021, MSS_TRANSMUXING_FAILED: 3022,
  TRANSMUXING_NO_VIDEO_DATA: 3023, STREAMING_NOT_ALLOWED: 3024, BUFFER_WRITE_OUT_OF_BOUNDS: 3025,
  UNABLE_TO_GUESS_MANIFEST_TYPE: 4000, DASH_INVALID_XML: 4001, DASH_NO_SEGMENT_INFO: 4002,
  DASH_EMPTY_ADAPTATION_SET: 4003, DASH_EMPTY_PERIOD: 4004, DASH_WEBM_MISSING_INIT: 4005,
  DASH_UNSUPPORTED_CONTAINER: 4006, DASH_PSSH_BAD_ENCODING: 4007, DASH_NO_COMMON_KEY_SYSTEM: 4008,
  DASH_MULTIPLE_KEY_IDS_NOT_SUPPORTED: 4009, DASH_CONFLICTING_KEY_IDS: 4010, UNPLAYABLE_PERIOD: 4011,
  RESTRICTIONS_CANNOT_BE_MET: 4012, INTERNAL_ERROR_KEY_STATUS: 4013, NO_PERIODS: 4014,
  HLS_PLAYLIST_HEADER_MISSING: 4015, INVALID_HLS_TAG: 4016, HLS_INVALID_PLAYLIST_HIERARCHY: 4017,
  DASH_DUPLICATE_REPRESENTATION_ID: 4018, HLS_MEDIA_INIT_SECTION_INFO_MISSING: 4019,
  HLS_MULTIPLE_MEDIA_INIT_SECTIONS_FOUND: 4020, HLS_COULD_NOT_GUESS_MIME_TYPE: 4021,
  HLS_MASTER_PLAYLIST_NOT_PROVIDED: 4022, HLS_REQUIRED_ATTRIBUTE_MISSING: 4023, HLS_REQUIRED_TAG_MISSING: 4024,
  HLS_COULD_NOT_GUESS_CODECS: 4025, HLS_KEYFORMATS_NOT_SUPPORTED: 4026, DASH_UNSUPPORTED_XLINK_ACTUATE: 4027,
  DASH_XLINK_DEPTH_LIMIT: 4028, HLS_LIVE_CONTENT_NOT_SUPPORTED: 4029,
  HLS_COULD_NOT_PARSE_SEGMENT_START_TIME: 4030, HLS_MEDIA_SEQUENCE_REQUIRED_IN_LIVE_STREAMS: 4031,
  CONTENT_UNSUPPORTED_BY_BROWSER: 4032, CANNOT_ADD_EXTERNAL_TEXT_TO_LIVE_STREAM: 4033,
  HLS_AES_128_ENCRYPTION_NOT_SUPPORTED: 4034, HLS_INTERNAL_SKIP_STREAM: 4035, NO_VARIANTS: 4036,
  PERIOD_FLATTENING_FAILED: 4037, INCONSISTENT_DRM_ACROSS_PERIODS: 4038, HLS_VARIABLE_NOT_FOUND: 4039,
  HLS_MSE_ENCRYPTED_MP2T_NOT_SUPPORTED: 4040, HLS_MSE_ENCRYPTED_LEGACY_APPLE_MEDIA_KEYS_NOT_SUPPORTED: 4041,
  NO_WEB_CRYPTO_API: 4042, HLS_AES_128_INVALID_IV_LENGTH: 4043, HLS_AES_128_INVALID_KEY_LENGTH: 4044,
  CANNOT_ADD_EXTERNAL_THUMBNAILS_TO_LIVE_STREAM: 4045, MSS_INVALID_XML: 4046,
  MSS_LIVE_CONTENT_NOT_SUPPORTED: 4047, AES_128_INVALID_IV_LENGTH: 4048, AES_128_INVALID_KEY_LENGTH: 4049,
  DASH_CONFLICTING_AES_128: 4050, DASH_UNSUPPORTED_AES_128: 4051, DASH_INVALID_PATCH: 4052,
  HLS_EMPTY_MEDIA_PLAYLIST: 4053, DASH_MSE_ENCRYPTED_LEGACY_APPLE_MEDIA_KEYS_NOT_SUPPORTED: 4054,
  CANNOT_ADD_EXTERNAL_CHAPTERS_TO_LIVE_STREAM: 4055, WEBTRANSPORT_NOT_AVAILABLE: 4056,
  WEBTRANSPORT_INITIALIZATION_FAILED: 4057, MSF_VOD_CONTENT_NOT_SUPPORTED: 4058,
  HLS_INVALID_KEY_IV_FOR_GCM: 4059, HLS_INVALID_GCM_SEGMENT: 4060, DASH_INVALID_JSON: 4061,
  MSF_NO_CATALOG: 4062, DASH_UNSUPPORTED_ESSENTIAL_PROPERTY: 4063, MSF_CATALOG_TIMEOUT: 4064,
  INCONSISTENT_BUFFER_STATE: 5000, INVALID_SEGMENT_INDEX: 5001, SEGMENT_DOES_NOT_EXIST: 5002,
  CANNOT_SATISFY_BYTE_LIMIT: 5003, BAD_SEGMENT: 5004, INVALID_STREAMS_CHOSEN: 5005,
  STREAMING_ENGINE_STARTUP_INVALID_STATE: 5006, NO_RECOGNIZED_KEY_SYSTEMS: 6000,
  REQUESTED_KEY_SYSTEM_CONFIG_UNAVAILABLE: 6001, FAILED_TO_CREATE_CDM: 6002, FAILED_TO_ATTACH_TO_VIDEO: 6003,
  INVALID_SERVER_CERTIFICATE: 6004, FAILED_TO_CREATE_SESSION: 6005, FAILED_TO_GENERATE_LICENSE_REQUEST: 6006,
  LICENSE_REQUEST_FAILED: 6007, LICENSE_RESPONSE_REJECTED: 6008, NO_LICENSE_SERVER_SPECIFIED: 6009,
  ENCRYPTED_CONTENT_WITHOUT_DRM_INFO: 6010, WRONG_KEYS: 6011, NO_LICENSE_SERVER_GIVEN: 6012,
  OFFLINE_SESSION_REMOVED: 6013, EXPIRED: 6014, SERVER_CERTIFICATE_REQUIRED: 6015,
  INIT_DATA_TRANSFORM_ERROR: 6016, SERVER_CERTIFICATE_REQUEST_FAILED: 6017, MIN_HDCP_VERSION_NOT_MATCH: 6018,
  ERROR_CHECKING_HDCP_VERSION: 6019, MISSING_EME_SUPPORT: 6020, LOAD_INTERRUPTED: 7000,
  OPERATION_ABORTED: 7001, NO_VIDEO_ELEMENT: 7002, OBJECT_DESTROYED: 7003, CONTENT_NOT_LOADED: 7004,
  SRC_EQUALS_PRELOAD_NOT_SUPPORTED: 7005, PRELOAD_DESTROYED: 7006, QUEUE_INDEX_OUT_OF_BOUNDS: 7007,
  CAST_API_UNAVAILABLE: 8000, NO_CAST_RECEIVERS: 8001, ALREADY_CASTING: 8002, UNEXPECTED_CAST_ERROR: 8003,
  CAST_CANCELED_BY_USER: 8004, CAST_CONNECTION_TIMED_OUT: 8005, CAST_RECEIVER_APP_UNAVAILABLE: 8006,
  CAST_RECEIVER_APP_ID_MISSING: 8007, STORAGE_NOT_SUPPORTED: 9000, INDEXED_DB_ERROR: 9001,
  DEPRECATED_OPERATION_ABORTED: 9002, REQUESTED_ITEM_NOT_FOUND: 9003, MALFORMED_OFFLINE_URI: 9004,
  CANNOT_STORE_LIVE_OFFLINE: 9005, STORE_ALREADY_IN_PROGRESS: 9006, NO_INIT_DATA_FOR_OFFLINE: 9007,
  LOCAL_PLAYER_INSTANCE_REQUIRED: 9008, UNSUPPORTED_UPGRADE_REQUEST: 9010,
  NEW_KEY_OPERATION_NOT_SUPPORTED: 9011, KEY_NOT_FOUND: 9012, MISSING_STORAGE_CELL: 9013,
  STORAGE_LIMIT_REACHED: 9014, DOWNLOAD_SIZE_CALLBACK_ERROR: 9015, MODIFY_OPERATION_NOT_SUPPORTED: 9016,
  INDEXED_DB_INIT_TIMED_OUT: 9017, CS_IMA_SDK_MISSING: 10000, CS_AD_MANAGER_NOT_INITIALIZED: 10001,
  SS_IMA_SDK_MISSING: 10002, SS_AD_MANAGER_NOT_INITIALIZED: 10003, CURRENT_DAI_REQUEST_NOT_FINISHED: 10004,
  MT_AD_MANAGER_NOT_INITIALIZED: 10005, INTERSTITIAL_AD_MANAGER_NOT_INITIALIZED: 10006,
  VAST_INVALID_XML: 10007, CS_AD_CONTAINER_MISSING: 10008, SS_AD_CONTAINER_MISSING: 10009,
  MEDIATAILOR_REQUEST_FAILED: 10010
};

function nameOf(table, value) {
  for (const key of Object.keys(table)) if (table[key] === value) return key;
  return String(value);
}

class ShakaError extends Error {
  constructor(severity, category, code, ...data) {
    super(`Shaka Error ${nameOf(Category, category)}.${nameOf(Code, code)} (${data.map(describe).join(', ')})`);
    this.severity = severity;
    this.category = category;
    this.code = code;
    this.data = data;
    this.handled = false;
  }

  toString() {
    return `shaka.util.Error ${JSON.stringify({ severity: this.severity, category: this.category, code: this.code,
                                                data: this.data.map(describe), handled: this.handled })}`;
  }
}
ShakaError.Severity = Severity;
ShakaError.Category = Category;
ShakaError.Code = Code;

function describe(value) {
  if (value instanceof Error) return value.message;
  if (value instanceof ArrayBuffer || ArrayBuffer.isView(value)) return `<${value.byteLength} bytes>`;
  if (typeof value === 'object' && value !== null) {
    try { return JSON.stringify(value); } catch { return String(value); }
  }
  return String(value);
}

function critical(category, code, ...data) { return new ShakaError(Severity.CRITICAL, category, code, ...data); }
function loadInterrupted() { return critical(Category.PLAYER, Code.LOAD_INTERRUPTED); }
function objectDestroyed() { return critical(Category.PLAYER, Code.OBJECT_DESTROYED); }

// ---- shaka.util.FakeEvent, FakeEventTarget, EventManager ---------------------------------

class FakeEvent {
  constructor(type, dict) {
    if (dict) {
      for (const key of (dict instanceof Map ? dict.keys() : Object.keys(dict))) {
        if (key === 'type') continue;
        this[key] = dict instanceof Map ? dict.get(key) : dict[key];
      }
    }
    this.type = type;
    this.bubbles = false;
    this.cancelable = false;
    this.defaultPrevented = false;
    this.timeStamp = Date.now();
    this.target = null;
    this.currentTarget = null;
    this.stopped = false;
  }
  preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
  stopImmediatePropagation() { this.stopped = true; }
  stopPropagation() {}
}
FakeEvent.EventName = {
  AbrStatusChanged: 'abrstatuschanged', Adaptation: 'adaptation', Buffering: 'buffering',
  Complete: 'complete', DrmSessionUpdate: 'drmsessionupdate', Error: 'error', Loaded: 'loaded', Loading: 'loading',
  ManifestParsed: 'manifestparsed', OnStateChange: 'onstatechange', RateChange: 'ratechange',
  Streaming: 'streaming', TextChanged: 'textchanged', TextTrackVisibility: 'texttrackvisibility',
  TracksChanged: 'trackschanged', Unloading: 'unloading', VariantChanged: 'variantchanged',
};

class FakeEventTarget {
  constructor() { this.listeners_ = new Map(); this.dispatchTarget = this; }

  addEventListener(type, listener, options) {
    if (!listener) return;
    if (!this.listeners_.has(type)) this.listeners_.set(type, []);
    const list = this.listeners_.get(type);
    if (list.some((entry) => entry.listener === listener)) return;
    list.push({ listener, once: !!(options && typeof options === 'object' && options.once) });
  }

  removeEventListener(type, listener) {
    const list = this.listeners_.get(type);
    if (!list) return;
    const at = list.findIndex((entry) => entry.listener === listener);
    if (at >= 0) list.splice(at, 1);
  }

  dispatchEvent(event) {
    const list = this.listeners_.get(event.type);
    event.target = this.dispatchTarget;
    event.currentTarget = this.dispatchTarget;
    for (const entry of list ? list.slice() : []) {
      if (entry.once) this.removeEventListener(event.type, entry.listener);
      try {
        if (typeof entry.listener === 'function') entry.listener.call(this, event);
        else if (entry.listener && typeof entry.listener.handleEvent === 'function') entry.listener.handleEvent(event);
      } catch (err) {
        log.alwaysError(`Uncaught in a ${event.type} listener:`, err);
      }
      if (event.stopped) break;
    }
    return !event.defaultPrevented;
  }

  release() { this.listeners_.clear(); }
}

class EventManager {
  constructor() { this.bindings_ = []; }
  listen(target, type, listener, options) {
    if (!target) return;
    target.addEventListener(type, listener, options);
    this.bindings_.push({ target, type, listener, options });
  }
  listenOnce(target, type, listener) {
    const once = (event) => {
      this.unlisten(target, type, once);
      listener(event);
    };
    this.listen(target, type, once);
  }
  unlisten(target, type, listener) {
    this.bindings_ = this.bindings_.filter((b) => {
      const match = b.target === target && b.type === type && (!listener || b.listener === listener);
      if (match) b.target.removeEventListener(b.type, b.listener, b.options);
      return !match;
    });
  }
  removeAll() {
    for (const b of this.bindings_) b.target.removeEventListener(b.type, b.listener, b.options);
    this.bindings_ = [];
  }
  release() { this.removeAll(); }
}

// ---- shaka.util.AbortableOperation ------------------------------------------------------------

class AbortableOperation {
  constructor(promise, onAbort) {
    this.promise = promise;
    this.onAbort_ = onAbort;
    this.aborted_ = false;
  }
  static completed(value) { return new AbortableOperation(Promise.resolve(value), () => Promise.resolve()); }
  static failed(error) {
    const promise = Promise.reject(error);
    promise.catch(() => {});
    return new AbortableOperation(promise, () => Promise.resolve());
  }
  static aborted() { return AbortableOperation.failed(critical(Category.PLAYER, Code.OPERATION_ABORTED)); }
  abort() {
    this.aborted_ = true;
    return Promise.resolve(this.onAbort_());
  }
  finally(onFinal) {
    this.promise.then(() => onFinal(true), () => onFinal(false));
    return this;
  }
  chain(onSuccess, onError) {
    const next = this.promise.then(onSuccess, onError);
    return new AbortableOperation(next, () => this.abort());
  }
}

// ---- shaka.net.NetworkingEngine ---------------------------------------------------------------
//
// Filters and retries as Shaka runs them, over the runtime's fetch (the
// platform's HTTP client, runtime/js/README.md "Networking"). The platform player
// fetches manifests and segments itself; what comes through here is the
// licence exchange, a FairPlay certificate, and whatever the app requests.

const RequestType = { MANIFEST: 0, SEGMENT: 1, LICENSE: 2, APP: 3, TIMING: 4, SERVER_CERTIFICATE: 5, KEY: 6,
                      ADS: 7, CONTENT_STEERING: 8, CMCD: 9 };
const AdvancedRequestType = { INIT_SEGMENT: 0, MEDIA_SEGMENT: 1, MEDIA_PLAYLIST: 2, MASTER_PLAYLIST: 3, MPD: 4,
                              MSS: 5, MPD_PATCH: 6, MEDIATAILOR_SESSION_INFO: 7, MEDIATAILOR_TRACKING_INFO: 8,
                              MEDIATAILOR_STATIC_RESOURCE: 9, MEDIATAILOR_TRACKING_EVENT: 10,
                              INTERSTITIAL_ASSET_LIST: 11, INTERSTITIAL_AD_URL: 12 };
const PluginPriority = { FALLBACK: 1, PREFERRED: 2, APPLICATION: 3 };
const schemePlugins = new Map();

function defaultRetryParameters() {
  return { maxAttempts: 2, baseDelay: 1000, backoffFactor: 2, fuzzFactor: 0.5, timeout: 30000, stallTimeout: 5000,
           connectionTimeout: 10000 };
}

function sleep(ms) { return new Promise((resolve) => setTimeout(resolve, ms)); }

class NetworkingEngine extends FakeEventTarget {
  constructor() {
    super();
    this.requestFilters_ = new Set();
    this.responseFilters_ = new Set();
    this.destroyed_ = false;
    this.inFlight_ = new Set();
  }

  static registerScheme(scheme, plugin, priority = PluginPriority.APPLICATION) {
    const existing = schemePlugins.get(scheme);
    if (!existing || existing.priority <= priority) schemePlugins.set(scheme, { plugin, priority });
  }
  static unregisterScheme(scheme) { schemePlugins.delete(scheme); }

  static makeRequest(uris, retryParams, streamDataCallback = null) {
    return {
      uris, method: 'GET', body: null, headers: {}, allowCrossSiteCredentials: false,
      retryParameters: retryParams || defaultRetryParameters(), licenseRequestType: null, sessionId: null,
      drmInfo: null, initData: null, initDataType: null, streamDataCallback, cmcdData: null, contentType: '',
    };
  }
  static defaultRetryParameters() { return defaultRetryParameters(); }

  registerRequestFilter(filter) { this.requestFilters_.add(filter); }
  unregisterRequestFilter(filter) { this.requestFilters_.delete(filter); }
  clearAllRequestFilters() { this.requestFilters_.clear(); }
  registerResponseFilter(filter) { this.responseFilters_.add(filter); }
  unregisterResponseFilter(filter) { this.responseFilters_.delete(filter); }
  clearAllResponseFilters() { this.responseFilters_.clear(); }

  destroy() {
    this.destroyed_ = true;
    for (const op of this.inFlight_) op.abort();
    this.inFlight_.clear();
    this.clearAllRequestFilters();
    this.clearAllResponseFilters();
    return Promise.resolve();
  }

  request(type, request, context = {}) {
    if (this.destroyed_) return AbortableOperation.aborted();
    const defaults = NetworkingEngine.makeRequest([], defaultRetryParameters());
    for (const key of Object.keys(defaults)) if (request[key] === undefined) request[key] = defaults[key];
    let aborted = false;
    let controller = null;
    const run = async () => {
      for (const filter of this.requestFilters_) {
        try {
          await filter(type, request, context);
        } catch (err) {
          throw err instanceof ShakaError ? err : critical(Category.NETWORK, Code.REQUEST_FILTER_ERROR, err);
        }
      }
      const retry = request.retryParameters || defaultRetryParameters();
      const attempts = Math.max(1, retry.maxAttempts | 0);
      let delay = retry.baseDelay;
      let lastError = null;
      for (let attempt = 0; attempt < attempts; attempt++) {
        if (aborted || this.destroyed_) throw critical(Category.PLAYER, Code.OPERATION_ABORTED);
        if (attempt > 0) {
          const fuzz = 1 + (Math.random() * 2 - 1) * retry.fuzzFactor;
          await sleep(delay * fuzz);
          delay *= retry.backoffFactor;
        }
        request.attempt = attempt;
        const uri = request.uris[attempt % request.uris.length];
        try {
          controller = typeof AbortController === 'function' ? new AbortController() : null;
          const response = await this.send_(uri, request, type, retry, controller, () => aborted);
          for (const filter of this.responseFilters_) {
            try {
              await filter(type, response, context);
            } catch (err) {
              throw err instanceof ShakaError ? err : critical(Category.NETWORK, Code.RESPONSE_FILTER_ERROR, err);
            }
          }
          return response;
        } catch (err) {
          lastError = err;
          const status = err instanceof ShakaError && err.code === Code.BAD_HTTP_STATUS ? err.data[1] : 0;
          const fatal = !(err instanceof ShakaError) || err.code === Code.OPERATION_ABORTED ||
                        err.code === Code.RESPONSE_FILTER_ERROR || status === 401 || status === 403;
          if (fatal) throw err;
        }
      }
      throw lastError;
    };
    const promise = run();
    const op = new AbortableOperation(promise, () => {
      aborted = true;
      if (controller) controller.abort();
      return Promise.resolve();
    });
    this.inFlight_.add(op);
    promise.then(() => this.inFlight_.delete(op), () => this.inFlight_.delete(op));
    return op;
  }

  async send_(uri, request, type, retry, controller, isAborted) {
    const scheme = String(uri).split(':')[0].toLowerCase();
    const plugin = schemePlugins.get(scheme);
    if (plugin) {
      const op = plugin.plugin(uri, request, type, () => {}, () => {});
      return op && op.promise ? op.promise : op;
    }
    if (!/^https?$/.test(scheme)) throw critical(Category.NETWORK, Code.UNSUPPORTED_SCHEME, uri);
    const started = Date.now();
    let timedOut = false;
    const timer = retry.timeout > 0 && controller ? setTimeout(() => { timedOut = true; controller.abort(); }, retry.timeout) : null;
    let res;
    let data;
    try {
      res = await fetch(uri, {
        method: request.method || 'GET',
        headers: request.headers || {},
        body: request.body == null ? undefined : request.body,
        credentials: request.allowCrossSiteCredentials ? 'include' : 'same-origin',
        signal: controller ? controller.signal : undefined,
      });
      data = await res.arrayBuffer();
    } catch (err) {
      if (timedOut) throw new ShakaError(Severity.RECOVERABLE, Category.NETWORK, Code.TIMEOUT, uri, type);
      if (isAborted()) throw critical(Category.PLAYER, Code.OPERATION_ABORTED);
      throw new ShakaError(Severity.RECOVERABLE, Category.NETWORK, Code.HTTP_ERROR, uri, err, type);
    } finally {
      if (timer !== null) clearTimeout(timer);
    }
    const headers = {};
    res.headers.forEach((value, name) => { headers[name.toLowerCase()] = value; });
    if (res.status < 200 || res.status > 299) {
      let text = '';
      try { text = StringUtils.fromUTF8(data); } catch { text = ''; }
      const severity = res.status === 401 || res.status === 403 ? Severity.CRITICAL : Severity.RECOVERABLE;
      throw new ShakaError(severity, Category.NETWORK, Code.BAD_HTTP_STATUS, uri, res.status, text, headers, type,
                           res.url || uri);
    }
    return { uri: res.url || uri, originalUri: uri, data, status: res.status, headers,
             timeMs: Date.now() - started, fromCache: false, originalRequest: request };
  }
}
NetworkingEngine.RequestType = RequestType;
NetworkingEngine.AdvancedRequestType = AdvancedRequestType;
NetworkingEngine.PluginPriority = PluginPriority;

// ---- shaka.drm.FairPlay ---------------------------------------------------------------------
//
// The request and response shapes FairPlay key servers want, as filters an app
// registers -- Shaka's own helpers, since licence requests are this player's.

function isFairPlay(keySystem) { return /^com\.apple\.fps/.test(String(keySystem || '')); }

const FairPlay = {
  defaultGetContentId: (initData) => {
    const uri = StringUtils.fromUTF16(initData, true);
    return uri.replace(/^skd:\/\//, '');
  },
  verimatrixFairPlayRequest: (type, request) => {
    if (type !== RequestType.LICENSE || !request.drmInfo || !isFairPlay(request.drmInfo.keySystem) || request.attempt > 0) return;
    request.headers['Content-Type'] = 'application/x-www-form-urlencoded';
    request.body = StringUtils.toUTF8('spc=' + Uint8ArrayUtils.toStandardBase64(request.body));
  },
  ezdrmFairPlayRequest: (type, request) => FairPlay.octetStream_(type, request),
  conaxFairPlayRequest: (type, request) => FairPlay.octetStream_(type, request),
  expressplayFairPlayRequest: (type, request) => FairPlay.octetStream_(type, request),
  muxFairPlayRequest: (type, request) => FairPlay.octetStream_(type, request),
  octetStream_: (type, request) => {
    if (type !== RequestType.LICENSE || !request.drmInfo || !isFairPlay(request.drmInfo.keySystem) || request.attempt > 0) return;
    request.headers['Content-Type'] = 'application/octet-stream';
  },
  commonFairPlayResponse: (type, response) => {
    if (type !== RequestType.LICENSE) return;
    const drmInfo = response.originalRequest && response.originalRequest.drmInfo;
    if (!drmInfo || !isFairPlay(drmInfo.keySystem)) return;
    let text;
    try { text = StringUtils.fromUTF8(response.data).trim(); } catch { return; }
    let found = false;
    if (text.startsWith('<ckc>') && text.endsWith('</ckc>')) {
      text = text.slice(5, -6);
      found = true;
    }
    if (!found) {
      try {
        const object = JSON.parse(text);
        for (const key of ['ckc', 'CkcMessage', 'License']) {
          if (object[key]) { text = object[key]; found = true; break; }
        }
      } catch { /* not JSON */ }
    }
    if (!found && !/^[A-Za-z0-9+/=\s]+$/.test(text)) return;
    response.data = toArrayBuffer(Uint8ArrayUtils.fromBase64(text));
  },
};
FairPlay.ezdrmFairPlayResponse = FairPlay.commonFairPlayResponse;
FairPlay.conaxFairPlayResponse = FairPlay.commonFairPlayResponse;
FairPlay.muxFairPlayResponse = FairPlay.commonFairPlayResponse;
FairPlay.expressplayFairPlayResponse = FairPlay.commonFairPlayResponse;
FairPlay.verimatrixFairPlayResponse = FairPlay.commonFairPlayResponse;

// ---- configuration ----------------------------------------------------------------------------

// Shaka 5's own defaults (PlayerConfiguration.createDefault), with its
// functions reduced to what they do on a platform player.
function defaultConfiguration() {
  return {
    drm: {
      retryParameters: {
        maxAttempts: 2,
        baseDelay: 1000,
        backoffFactor: 2,
        fuzzFactor: 0.5,
        timeout: 30000,
        stallTimeout: 5000,
        connectionTimeout: 10000,
      },
      servers: {},
      clearKeys: {},
      advanced: {},
      delayLicenseRequestUntilPlayed: false,
      persistentSessionOnlinePlayback: false,
      persistentSessionsMetadata: [],
      initDataTransform: (initData) => initData,
      logLicenseExchange: false,
      updateExpirationTime: 1,
      preferredKeySystems: [],
      keySystemsMapping: {},
      parseInbandPsshEnabled: false,
      minHdcpVersion: '',
      ignoreDuplicateInitData: true,
      defaultAudioRobustnessForWidevine: 'SW_SECURE_CRYPTO',
      defaultVideoRobustnessForWidevine: 'SW_SECURE_DECODE',
      renewalIntervalSec: 0,
      failureCallback: () => {},
    },
    manifest: {
      retryParameters: {
        maxAttempts: 2,
        baseDelay: 1000,
        backoffFactor: 2,
        fuzzFactor: 0.5,
        timeout: 30000,
        stallTimeout: 5000,
        connectionTimeout: 10000,
      },
      availabilityWindowOverride: NaN,
      disableAudio: false,
      disableVideo: false,
      disableText: false,
      disableThumbnails: false,
      disableIFrames: false,
      disableChapters: false,
      defaultPresentationDelay: 0,
      segmentRelativeVttTiming: false,
      raiseFatalErrorOnManifestUpdateRequestFailure: false,
      continueLoadingWhenPaused: true,
      ignoreSupplementalCodecs: false,
      updatePeriod: -1,
      ignoreDrmInfo: false,
      enableAudioGroups: true,
      dash: {
        clockSyncUri: '',
        xlinkFailGracefully: false,
        ignoreMinBufferTime: false,
        autoCorrectDrift: true,
        initialSegmentLimit: 1000,
        ignoreSuggestedPresentationDelay: false,
        ignoreEmptyAdaptationSet: false,
        ignoreMaxSegmentDuration: false,
        keySystemsByURI: {
          'urn:uuid:1077efec-c0b2-4d02-ace3-3c1e52e2fb4b': 'org.w3.clearkey',
          'urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e': 'org.w3.clearkey',
          'urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed': 'com.widevine.alpha',
          'urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95': 'com.microsoft.playready',
          'urn:uuid:79f0049a-4098-8642-ab92-e65be0885f95': 'com.microsoft.playready',
          'urn:uuid:94ce86fb-07ff-4f43-adb8-93d2fa968ca2': 'com.apple.fps',
          'urn:uuid:3d5e6d35-9b9a-41e8-b843-dd3c6e72c42c': 'com.huawei.wiseplay',
        },
        manifestPreprocessorTXml: (element) => element,
        sequenceMode: false,
        useStreamOnceInPeriodFlattening: false,
        enableFastSwitching: true,
      },
      hls: {
        ignoreTextStreamFailures: false,
        ignoreImageStreamFailures: false,
        defaultAudioCodec: 'mp4a.40.2',
        defaultVideoCodec: 'avc1.42E01E',
        ignoreManifestProgramDateTime: false,
        ignoreManifestProgramDateTimeForTypes: [],
        mediaPlaylistFullMimeType: 'video/mp2t; codecs="avc1.42E01E, mp4a.40.2"',
        liveSegmentsDelay: 3,
        sequenceMode: false,
        ignoreManifestTimestampsInSegmentsMode: false,
        disableCodecGuessing: false,
        disableClosedCaptionsDetection: false,
        allowLowLatencyByteRangeOptimization: true,
        allowRangeRequestsToGuessMimeType: false,
        chaptersUri: '',
      },
      msf: {
        fingerprintUri: '',
        namespaces: [],
        authorizationToken: '',
        subscribeFilterType: 2,
        useFetchCatalog: false,
        version: 'auto',
        catalogPreprocessor: (catalog) => catalog,
      },
    },
    streaming: {
      retryParameters: {
        maxAttempts: 2,
        baseDelay: 1000,
        backoffFactor: 2,
        fuzzFactor: 0.5,
        timeout: 30000,
        stallTimeout: 5000,
        connectionTimeout: 10000,
      },
      failureCallback: () => {},
      rebufferingGoal: 0,
      bufferingGoal: 10,
      bufferBehind: 30,
      evictionGoal: 1,
      ignoreTextStreamFailures: false,
      startAtSegmentBoundary: false,
      gapDetectionThreshold: 0.5,
      gapPadding: 0,
      gapJumpTimerTime: 0.25,
      durationBackoff: 1,
      safeSeekOffset: 5,
      safeSeekEndOffset: 0,
      stallEnabled: true,
      stallThreshold: 1,
      stallSkip: 0.1,
      useNativeHlsForFairPlay: true,
      inaccurateManifestTolerance: 2,
      lowLatencyMode: true,
      preferNativeDash: false,
      preferNativeHls: false,
      updateIntervalSeconds: 1,
      observeQualityChanges: false,
      maxDisabledTime: 30,
      segmentPrefetchLimit: 1,
      prefetchAudioLanguages: [],
      disableAudioPrefetch: false,
      disableTextPrefetch: false,
      disableVideoPrefetch: false,
      liveSync: {
        enabled: false,
        targetLatency: 0.5,
        targetLatencyTolerance: 0.5,
        maxPlaybackRate: 1.1,
        minPlaybackRate: 0.95,
        panicMode: false,
        panicThreshold: 60,
        dynamicTargetLatency: {
          enabled: false,
          stabilityThreshold: 60,
          rebufferIncrement: 0.5,
          maxAttempts: 10,
          maxLatency: 4,
          minLatency: 1,
        },
      },
      allowMediaSourceRecoveries: true,
      minTimeBetweenRecoveries: 5,
      vodDynamicPlaybackRate: false,
      vodDynamicPlaybackRateLowBufferRate: 0.95,
      vodDynamicPlaybackRateBufferRatio: 0.5,
      preloadNextUrlWindow: 30,
      loadTimeout: 30,
      clearDecodingCache: false,
      dontChooseCodecs: false,
      shouldFixTimestampOffset: false,
      avoidEvictionOnQuotaExceededError: false,
      crossBoundaryStrategy: 'keep',
      returnToEndOfLiveWindowWhenOutside: false,
      stopFetchingOnPause: false,
      clampAppendWindowToDuration: false,
      processSrcEqualMetadata: true,
    },
    networking: {
      forceHTTP: false,
      forceHTTPS: false,
      minBytesForProgressEvents: 16000,
      commonAccessTokenHeaderName: 'cta-common-access-token',
    },
    mediaSource: {
      codecSwitchingStrategy: 'reload',
      addExtraFeaturesToSourceBuffer: () => '',
      forceTransmux: false,
      insertFakeEncryptionInInit: true,
      correctEc3Enca: false,
      modifyCueCallback: () => {},
      dispatchAllEmsgBoxes: false,
      useSourceElements: true,
      durationReductionEmitsUpdateEnd: true,
      transmuxWorkerUrl: '',
      repairIFrames: true,
    },
    offline: {
      trackSelectionCallback: (tracks) => tracks,
      downloadSizeCallback: () => true,
      progressCallback: () => {},
      usePersistentLicense: true,
      numberOfParallelDownloads: 5,
    },
    abrFactory: () => null,
    adaptationSetCriteriaFactory: () => null,
    abr: {
      enabled: true,
      useNetworkInformation: true,
      defaultBandwidthEstimate: 1000000,
      switchInterval: 8,
      bandwidthUpgradeTarget: 0.85,
      bandwidthDowngradeTarget: 0.95,
      restrictions: {
        minWidth: 0,
        maxWidth: Infinity,
        minHeight: 0,
        maxHeight: Infinity,
        minPixels: 0,
        maxPixels: Infinity,
        minFrameRate: 0,
        maxFrameRate: Infinity,
        minBandwidth: 0,
        maxBandwidth: Infinity,
        minChannelsCount: 0,
        maxChannelsCount: Infinity,
      },
      advanced: {
        minTotalBytes: 128000,
        minBytes: 16000,
        fastHalfLife: 2,
        slowHalfLife: 5,
        droppedFramesThreshold: 0.15,
        droppedFramesInterval: 2,
        droppedFramesBanDuration: 30,
      },
      restrictToElementSize: false,
      restrictToScreenSize: false,
      ignoreDevicePixelRatio: false,
      clearBufferSwitch: false,
      safeMarginSwitch: 0,
      cacheLoadThreshold: 5,
      minTimeToSwitch: 0,
      preferNetworkInformationBandwidth: false,
      droppedFrames: true,
    },
    preferredAudio: [
      {
        language: '',
        role: '',
        label: '',
        channelCount: 2,
        codec: '',
      },
    ],
    preferredText: [],
    preferredVideo: [
      {
        label: '',
        role: '',
        language: '',
        codec: '',
        hdrLevel: 'AUTO',
        layout: '',
      },
    ],
    preferredDecodingAttributes: [],
    restrictions: {
      minWidth: 0,
      maxWidth: Infinity,
      minHeight: 0,
      maxHeight: Infinity,
      minPixels: 0,
      maxPixels: Infinity,
      minFrameRate: 0,
      maxFrameRate: Infinity,
      minBandwidth: 0,
      maxBandwidth: Infinity,
      minChannelsCount: 0,
      maxChannelsCount: Infinity,
    },
    playRangeStart: 0,
    playRangeEnd: Infinity,
    textDisplayer: {
      fontScaleFactor: 1,
      positionArea: 0,
      subtitleDelay: 0,
      suspendRenderingWhenHidden: true,
    },
    textDisplayFactory: () => null,
    cmcd: {
      enabled: false,
      sessionId: '',
      contentId: '',
      rtpSafetyFactor: 5,
      useHeaders: false,
      includeKeys: [],
      version: 1,
      eventTargets: [],
    },
    cmsd: {
      enabled: true,
      applyMaximumSuggestedBitrate: true,
      estimatedThroughputWeightRatio: 0.5,
    },
    lcevc: {
      enabled: false,
      dynamicPerformanceScaling: true,
      logLevel: 0,
      drawLogo: false,
      poster: true,
    },
    ads: {
      customPlayheadTracker: false,
      skipPlayDetection: false,
      supportsMultipleMediaElements: true,
      disableHLSInterstitial: false,
      disableDASHInterstitial: false,
      allowPreloadOnDomElements: true,
      allowStartInMiddleOfInterstitial: true,
      disableTrackingEvents: false,
      disableSnapback: false,
      interstitialPreloadAheadTime: 10,
      disablePlayedLinearAdSkip: false,
      disableTrackingForPlayedLinearAds: false,
    },
    ignoreHardwareResolution: false,
    queue: {
      preloadNextUrlWindow: Infinity,
      preloadPrevItem: true,
      repeatMode: 0,
    },
    accessibility: {
      handleForcedSubtitlesAutomatically: true,
      speechToText: {
        enabled: false,
        maxTextLength: 140,
        processLocally: false,
        languagesToTranslate: [],
      },
    },
  };
}

// Maps whose keys are the app's own, and what their values are.
const MAP_PATHS = {
  '.drm.servers': 'string', '.drm.clearKeys': 'string', '.drm.advanced': 'advanced',
  '.drm.keySystemsMapping': 'string', '.manifest.dash.keySystemsByURI': 'string',
};
function advancedDrmTemplate() {
  return { distinctiveIdentifierRequired: false, persistentStateRequired: false, videoRobustness: [],
           audioRobustness: [], sessionType: '', serverCertificate: new Uint8Array(0), serverCertificateUri: '',
           individualizationServer: '', headers: {} };
}

function isPlainObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value) && !ArrayBuffer.isView(value) &&
         !(value instanceof ArrayBuffer);
}

function cloneConfig(value) {
  if (Array.isArray(value)) return value.map(cloneConfig);
  if (ArrayBuffer.isView(value)) return new Uint8Array(toUint8(value));
  if (isPlainObject(value)) {
    const out = {};
    for (const key of Object.keys(value)) out[key] = cloneConfig(value[key]);
    return out;
  }
  return value;
}

// Shaka's ConfigUtils.mergeConfigObjects: a key the template does not have, or
// a value of the wrong type, is refused with an error and leaves the rest
// applied; `undefined` restores the default.
function mergeConfig(destination, source, template, path) {
  let valid = true;
  for (const key of Object.keys(source)) {
    const subPath = `${path}.${key}`;
    const value = source[key];
    const mapKind = MAP_PATHS[path];
    if (mapKind) {
      if (value === undefined || value === null) {
        delete destination[key];
      } else if (mapKind === 'advanced') {
        if (!isPlainObject(value)) {
          log.alwaysError(`Invalid config, wrong type for ${subPath}`);
          valid = false;
          continue;
        }
        if (!isPlainObject(destination[key])) destination[key] = advancedDrmTemplate();
        valid = mergeConfig(destination[key], value, advancedDrmTemplate(), subPath) && valid;
      } else {
        destination[key] = String(value);
      }
      continue;
    }
    if (!(key in template)) {
      log.alwaysError(`Invalid config, unrecognized key ${subPath}`);
      valid = false;
      continue;
    }
    const model = template[key];
    if (value === undefined) {
      destination[key] = cloneConfig(model);
      continue;
    }
    if (MAP_PATHS[subPath]) {
      if (!isPlainObject(value)) {
        log.alwaysError(`Invalid config, wrong type for ${subPath}`);
        valid = false;
        continue;
      }
      if (!isPlainObject(destination[key])) destination[key] = {};
      valid = mergeConfig(destination[key], value, {}, subPath) && valid;
    } else if (key === 'headers' && isPlainObject(value)) {
      destination[key] = Object.assign({}, value);
    } else if (isPlainObject(model)) {
      if (!isPlainObject(value)) {
        log.alwaysError(`Invalid config, wrong type for ${subPath}`);
        valid = false;
        continue;
      }
      if (!isPlainObject(destination[key])) destination[key] = cloneConfig(model);
      valid = mergeConfig(destination[key], value, model, subPath) && valid;
    } else if (Array.isArray(model)) {
      // Robustness is a string in Shaka 4 and an array in Shaka 5: take either.
      if (Array.isArray(value)) destination[key] = cloneConfig(value);
      else if (typeof value === 'string' && /Robustness$/.test(key)) destination[key] = value ? [value] : [];
      else {
        log.alwaysError(`Invalid config, wrong type for ${subPath}`);
        valid = false;
      }
    } else if (ArrayBuffer.isView(model)) {
      if (value instanceof ArrayBuffer || ArrayBuffer.isView(value)) destination[key] = new Uint8Array(toUint8(value));
      else {
        log.alwaysError(`Invalid config, wrong type for ${subPath}`);
        valid = false;
      }
    } else if (typeof model === 'function') {
      if (typeof value === 'function') destination[key] = value;
      else {
        log.alwaysError(`Invalid config, wrong type for ${subPath}`);
        valid = false;
      }
    } else if (model !== null && typeof model !== typeof value) {
      log.alwaysError(`Invalid config, wrong type for ${subPath}`);
      valid = false;
    } else {
      destination[key] = value;
    }
  }
  return valid;
}

// Shaka 4's flat preference keys, which Shaka 5 still takes and maps.
const LEGACY_PREFERENCES = {
  preferredAudioLanguage: ['preferredAudio', 'language'], preferredAudioLabel: ['preferredAudio', 'label'],
  preferredVariantRole: ['preferredAudio', 'role'], preferredAudioChannelCount: ['preferredAudio', 'channelCount'],
  preferredTextLanguage: ['preferredText', 'language'], preferredTextRole: ['preferredText', 'role'],
  preferForcedSubs: ['preferredText', 'forced'], preferredVideoLabel: ['preferredVideo', 'label'],
  preferredVideoHdrLevel: ['preferredVideo', 'hdrLevel'], preferredVideoLayout: ['preferredVideo', 'layout'],
};
function applyLegacyPreferences(config, source) {
  const rest = {};
  for (const key of Object.keys(source)) {
    const target = LEGACY_PREFERENCES[key];
    if (!target) {
      rest[key] = source[key];
      continue;
    }
    log.warnOnce(key, `${key} is deprecated: use ${target[0]} (Shaka 5)`);
    const list = config[target[0]];
    if (list.length === 0) {
      list.push(target[0] === 'preferredText' ? { language: '', role: '', format: '', forced: false }
                                              : { language: '', role: '', label: '', channelCount: 2, codec: '' });
    }
    list[0][target[1]] = source[key];
  }
  return rest;
}

function objectFromPath(path, value) {
  const out = {};
  let at = out;
  const parts = String(path).split('.');
  parts.forEach((part, i) => {
    if (i === parts.length - 1) at[part] = value;
    else at = at[part] = {};
  });
  return out;
}

// ---- shaka.Player ---------------------------------------------------------------------------

const MEDIA_CONTROLLER = Symbol.for('screenkit.media');
const LoadMode = { DESTROYED: 0, NOT_LOADED: 1, MEDIA_SOURCE: 2, SRC_EQUALS: 3 };
const VERSION = 'v5.2.0-screenkit';
const supportPlugins = new Map();

function controllerOf(element) {
  const controller = element && element[MEDIA_CONTROLLER];
  return controller && typeof controller.load === 'function' ? controller : null;
}

function runtimeCapabilities() {
  const media = globalThis.__screenkit && globalThis.__screenkit.media;
  return media && typeof media.capabilities === 'function' ? media.capabilities() : null;
}

function guessMimeType(uri) {
  const path = String(uri).split(/[?#]/)[0].toLowerCase();
  if (/\.m3u8?$/.test(path)) return 'application/x-mpegurl';
  if (/\.mpd$/.test(path)) return 'application/dash+xml';
  if (/\.(mp4|m4v|m4a|mov)$/.test(path)) return 'video/mp4';
  if (/\.webm$/.test(path)) return 'video/webm';
  return '';
}
function manifestTypeOf(mimeType) {
  if (/mpegurl/i.test(mimeType)) return 'HLS';
  if (/dash\+xml/i.test(mimeType)) return 'DASH';
  return null;
}

// Shaka's key system names, reduced to the ones the platform players know.
function normaliseKeySystem(keySystem, mapping) {
  const mapped = (mapping && mapping[keySystem]) || keySystem;
  if (isFairPlay(mapped)) return 'com.apple.fps';
  if (/^com\.widevine\.alpha/.test(mapped)) return 'com.widevine.alpha';
  return mapped;
}

function firstRobustness(value) {
  if (Array.isArray(value)) return value.find((v) => v) || '';
  return value ? String(value) : '';
}

function freshStats() {
  return { loadStarted: 0, loadLatency: NaN, firstFrame: NaN, drmTime: NaN, licenseTime: 0,
           playTime: 0, pauseTime: 0, bufferingTime: 0, state: null, stateSince: 0,
           stateHistory: [], switchHistory: [] };
}

class Player extends FakeEventTarget {
  constructor(mediaElement = null, videoContainer = null, dependencyInjector = null) {
    super();
    this.video_ = null;
    this.controller_ = null;
    this.unlisten_ = null;
    this.eventManager_ = new EventManager();
    this.config_ = defaultConfiguration();
    this.networkingEngine_ = new NetworkingEngine();
    this.loadMode_ = LoadMode.NOT_LOADED;
    this.assetUri_ = null;
    this.mimeType_ = null;
    this.keySystem_ = '';
    this.drmInfo_ = null;
    this.loadToken_ = 0;
    this.pendingLoad_ = null;
    this.textVisible_ = false;
    this.selectedText_ = null;
    this.pendingVariant_ = null;
    this.activeVariant_ = -1;
    this.buffering_ = false;
    this.hasVideo_ = true;
    this.hasAudio_ = true;
    this.licenceFailed_ = -1;
    this.stats_ = freshStats();
    this.videoContainer_ = videoContainer;
    this.startTime_ = null;
    this.destroyed_ = false;
    if (dependencyInjector) dependencyInjector(this);
    if (mediaElement) {
      log.alwaysWarn('new shaka.Player(mediaElement) is deprecated: call attach(mediaElement)');
      this.attach(mediaElement).catch((err) => log.alwaysError(err));
    }
  }

  static isBrowserSupported() {
    const caps = runtimeCapabilities();
    return !!(caps && caps.available);
  }

  static async probeSupport(promptsOkay = true) {
    void promptsOkay;
    const caps = runtimeCapabilities() || { hls: false, dash: false, containers: [], codecs: [], keySystems: [] };
    const manifest = {
      mpd: caps.dash, 'application/dash+xml': caps.dash, 'video/vnd.mpeg.dash.mpd': caps.dash,
      m3u8: caps.hls, 'application/x-mpegurl': caps.hls, 'application/vnd.apple.mpegurl': caps.hls,
      'audio/mpegurl': caps.hls, mss: false, ism: false, 'application/vnd.ms-sstr+xml': false,
    };
    const media = {};
    for (const container of caps.containers) media[container] = true;
    for (const codec of caps.codecs) {
      media[`video/mp4; codecs="${codec}"`] = true;
      media[`audio/mp4; codecs="${codec}"`] = true;
    }
    const drm = {};
    for (const keySystem of ['org.w3.clearkey', 'com.widevine.alpha', 'com.microsoft.playready', 'com.apple.fps']) {
      drm[keySystem] = caps.keySystems.indexOf(keySystem) >= 0
        ? { persistentState: false, encryptionSchemes: keySystem === 'com.apple.fps' ? ['cbcs'] : ['cenc'],
            videoRobustnessLevels: [], audioRobustnessLevels: [], minHdcpVersions: [] }
        : null;
    }
    const support = { manifest, media, drm, hardwareResolution: { width: Infinity, height: Infinity } };
    for (const [name, callback] of supportPlugins) support[name] = callback();
    return support;
  }

  static registerSupportPlugin(name, callback) { supportPlugins.set(name, callback); }
  static setAdManagerFactory() {}
  static setQueueManagerFactory() {}

  // --- attaching ---------------------------------------------------------------------------

  async attach(mediaElement, initializeMediaSource = true) {
    void initializeMediaSource;
    if (this.destroyed_) throw objectDestroyed();
    if (this.video_ === mediaElement) return;
    if (this.video_) await this.detach();
    const controller = controllerOf(mediaElement);
    if (!controller) {
      throw critical(Category.PLAYER, Code.NO_VIDEO_ELEMENT,
                     'not a ScreenKit media element: create it with document.createElement("video")');
    }
    this.video_ = mediaElement;
    this.controller_ = controller;
    this.unlisten_ = controller.listen((type, payload) => this.onPlayerEvent_(type, payload));
    const em = this.eventManager_;
    em.listen(mediaElement, 'playing', () => this.setState_('playing'));
    em.listen(mediaElement, 'pause', () => { if (!mediaElement.ended) this.setState_('paused'); });
    em.listen(mediaElement, 'waiting', () => this.setState_('buffering'));
    em.listen(mediaElement, 'ended', () => {
      this.setState_('ended');
      this.dispatchEvent(new FakeEvent('complete'));
    });
    em.listen(mediaElement, 'ratechange', () => this.dispatchEvent(new FakeEvent('ratechange')));
    em.listen(mediaElement, 'loadeddata', () => {
      if (isNaN(this.stats_.firstFrame) && this.stats_.loadStarted) {
        this.stats_.firstFrame = (Date.now() - this.stats_.loadStarted) / 1000;
      }
    });
    this.dispatchEvent(new FakeEvent('onstatechange', { state: 'attach' }));
  }

  async detach() {
    if (this.destroyed_ || !this.video_) return;
    await this.unload(false);
    if (this.unlisten_) this.unlisten_();
    this.unlisten_ = null;
    this.eventManager_.removeAll();
    this.video_ = null;
    this.controller_ = null;
    this.dispatchEvent(new FakeEvent('onstatechange', { state: 'detach' }));
  }

  setVideoContainer(container) { this.videoContainer_ = container; }
  getMediaElement() { return this.video_; }
  getNetworkingEngine() { return this.networkingEngine_; }

  // --- loading -------------------------------------------------------------------------------

  // A load in progress, superseded by another load, an unload or destroy: its
  // promise rejects with LOAD_INTERRUPTED at once, whatever it was waiting on.
  beginLoad_() {
    this.interruptPending_(loadInterrupted());
    const token = ++this.loadToken_;
    let reject = null;
    const interrupt = new Promise((resolveUnused, rejectLoad) => { reject = rejectLoad; });
    interrupt.catch(() => {});
    this.pendingLoad_ = { token, reject };
    return { token, interrupt };
  }

  interruptPending_(error) {
    const pending = this.pendingLoad_;
    this.pendingLoad_ = null;
    if (pending) pending.reject(error);
  }

  async load(assetUri, startTime = null, mimeType = null) {
    if (this.destroyed_) throw objectDestroyed();
    if (!this.video_) throw critical(Category.PLAYER, Code.NO_VIDEO_ELEMENT);
    if (assetUri !== null && typeof assetUri === 'object') {
      throw critical(Category.PLAYER, Code.SRC_EQUALS_PRELOAD_NOT_SUPPORTED);
    }
    const { token, interrupt } = this.beginLoad_();
    const inner = this.loadInner_(token, String(assetUri), startTime, mimeType, interrupt);
    inner.catch(() => {});
    try {
      await Promise.race([inner, interrupt]);
    } catch (err) {
      const error = err instanceof ShakaError ? err : critical(Category.PLAYER, Code.LOAD_INTERRUPTED, err);
      if (error.code !== Code.LOAD_INTERRUPTED) {
        if (token === this.loadToken_) this.loadMode_ = LoadMode.NOT_LOADED;
        this.dispatchEvent(new FakeEvent('error', { detail: error }));
      }
      throw error;
    } finally {
      if (this.pendingLoad_ && this.pendingLoad_.token === token) this.pendingLoad_ = null;
    }
  }

  async loadInner_(token, uri, startTime, mimeType, interrupt) {
    const check = () => {
      if (token !== this.loadToken_ || this.destroyed_) throw loadInterrupted();
    };
    if (this.loadMode_ === LoadMode.SRC_EQUALS || this.assetUri_ !== null) {
      this.dispatchEvent(new FakeEvent('unloading'));
      this.resetContent_();
    }
    this.dispatchEvent(new FakeEvent('loading'));
    this.stats_ = freshStats();
    this.stats_.loadStarted = Date.now();
    this.assetUri_ = uri;
    const caps = this.controller_.capabilities();
    const type = String(mimeType || guessMimeType(uri));
    this.mimeType_ = type || null;
    // The key system first: a configured one the platform lacks is 6001
    // whatever the manifest (ClearKey on Apple is 6001, not DASH's 4000).
    const keySystem = this.chooseKeySystem_(caps);
    if (manifestTypeOf(type) === 'DASH' && !caps.dash) {
      throw critical(Category.MANIFEST, Code.UNABLE_TO_GUESS_MANIFEST_TYPE, uri,
                     `DASH is not played on ${caps.platform}: its player has none`);
    }
    const drmStarted = Date.now();
    const drm = await Promise.race([this.resolveDrm_(keySystem), interrupt]);
    check();
    if (drm) this.stats_.drmTime = (Date.now() - drmStarted) / 1000;
    if (startTime instanceof Date) {
      log.alwaysWarn('load(uri, Date) is not supported on a platform player; starting at the default position');
      startTime = null;
    }
    this.startTime_ = typeof startTime === 'number' ? startTime : null;
    const preferredText = this.config_.preferredText[0];
    const preferredAudio = this.config_.preferredAudio[0];
    try {
      await Promise.race([this.controller_.load({
        url: uri, mimeType: type, startTime: this.startTime_, drm, abr: this.abrSettings_(),
        audioLanguage: preferredAudio ? preferredAudio.language : '',
        textLanguage: preferredText ? preferredText.language : '',
      }), interrupt]);
    } catch (failure) {
      throw failure instanceof ShakaError ? failure : this.errorFromFailure_(failure, uri);
    }
    check();
    // Shaka resolves a load once the manifest is parsed: its tracks, the
    // variant chosen and, for live, the window are known. A platform player can
    // report metadata first and those a moment later: wait for them, briefly.
    await Promise.race([this.settled_(), interrupt]);
    check();
    this.loadMode_ = LoadMode.SRC_EQUALS;
    this.stats_.loadLatency = (Date.now() - this.stats_.loadStarted) / 1000;
    this.setState_(this.video_.paused ? 'paused' : 'playing');
    this.dispatchEvent(new FakeEvent('manifestparsed'));
    this.dispatchEvent(new FakeEvent('streaming'));
    if (this.controller_.tracks().variants.length > 0) this.dispatchEvent(new FakeEvent('trackschanged'));
    this.dispatchEvent(new FakeEvent('loaded'));
    this.applyTextPreference_();
  }

  settled_() {
    const controller = this.controller_;
    const ready = () => {
      const variants = controller.tracks().variants;
      const info = controller.info();
      return variants.length > 0 && (info.variant >= 0 || variants.some((v) => v.active)) &&
             (!info.live || info.seekEnd > info.seekStart);
    };
    if (ready()) return Promise.resolve();
    return new Promise((resolve) => {
      const timer = setTimeout(done, 3000);
      const unlisten = controller.listen(() => { if (ready()) done(); });
      function done() {
        clearTimeout(timer);
        unlisten();
        resolve();
      }
    });
  }

  // The configured key system this platform has, the licence server and, for
  // FairPlay, the application certificate -- fetched here, through the
  // networking engine, as Shaka fetches serverCertificateUri.
  // The configured key system to use: the first (by preferredKeySystems) the
  // platform has; null when none is configured; 6001 when none it has is.
  chooseKeySystem_(caps) {
    const config = this.config_.drm;
    const configured = [];
    if (Object.keys(config.clearKeys).length > 0) configured.push('org.w3.clearkey');
    for (const keySystem of Object.keys(config.servers)) if (config.servers[keySystem]) configured.push(keySystem);
    this.keySystem_ = '';
    this.drmInfo_ = null;
    if (configured.length === 0) return null;
    const preferred = config.preferredKeySystems || [];
    configured.sort((a, b) => {
      const x = preferred.indexOf(a), y = preferred.indexOf(b);
      return (x < 0 ? Infinity : x) - (y < 0 ? Infinity : y);
    });
    const chosen = configured.find((ks) => caps.keySystems.indexOf(normaliseKeySystem(ks, config.keySystemsMapping)) >= 0);
    if (!chosen) {
      const error = critical(Category.DRM, Code.REQUESTED_KEY_SYSTEM_CONFIG_UNAVAILABLE);
      error.message += ` -- configured ${configured.join(', ')}; ${caps.platform} has ${caps.keySystems.join(', ') || 'none'}`;
      throw error;
    }
    return chosen;
  }

  async resolveDrm_(chosen) {
    if (chosen === null) return null;
    const config = this.config_.drm;
    const keySystem = normaliseKeySystem(chosen, config.keySystemsMapping);
    const advanced = config.advanced[chosen] || advancedDrmTemplate();
    let certificate = advanced.serverCertificate && advanced.serverCertificate.byteLength
      ? toUint8(advanced.serverCertificate) : null;
    if (!certificate && advanced.serverCertificateUri) {
      const request = NetworkingEngine.makeRequest([advanced.serverCertificateUri], config.retryParameters);
      try {
        const response = await this.networkingEngine_.request(RequestType.SERVER_CERTIFICATE, request).promise;
        certificate = toUint8(response.data);
      } catch (err) {
        throw critical(Category.DRM, Code.LICENSE_REQUEST_FAILED, err);
      }
    }
    if (keySystem === 'com.apple.fps' && !certificate) {
      throw critical(Category.DRM, Code.SERVER_CERTIFICATE_REQUIRED);
    }
    this.keySystem_ = chosen;
    this.drmInfo_ = {
      keySystem: chosen, encryptionScheme: keySystem === 'com.apple.fps' ? 'cbcs' : 'cenc',
      licenseServerUri: config.servers[chosen] || '',
      distinctiveIdentifierRequired: !!advanced.distinctiveIdentifierRequired,
      persistentStateRequired: !!advanced.persistentStateRequired,
      audioRobustness: firstRobustness(advanced.audioRobustness), videoRobustness: firstRobustness(advanced.videoRobustness),
      serverCertificate: certificate, serverCertificateUri: advanced.serverCertificateUri || '',
      sessionType: advanced.sessionType || 'temporary', initData: [], keyIds: new Set(),
    };
    return {
      keySystem, licenceServer: config.servers[chosen] || '',
      clearKeys: keySystem === 'org.w3.clearkey'
        ? Object.keys(config.clearKeys).map((kid) => [kid.toLowerCase(), String(config.clearKeys[kid]).toLowerCase()])
        : [],
      serverCertificate: certificate, videoRobustness: this.drmInfo_.videoRobustness,
      audioRobustness: this.drmInfo_.audioRobustness,
    };
  }

  // What the platform player said went wrong, as Shaka's error for it.
  errorFromFailure_(failure, uri) {
    const message = failure && failure.message ? String(failure.message) : '';
    switch (failure && failure.kind) {
      case 'interrupted':
        return loadInterrupted();
      case 'network':
        return failure.httpStatus > 0
          ? critical(Category.NETWORK, Code.BAD_HTTP_STATUS, uri, failure.httpStatus, message, {}, RequestType.MANIFEST, uri)
          : critical(Category.NETWORK, Code.HTTP_ERROR, uri, new Error(message), RequestType.MANIFEST);
      case 'manifest':
        return critical(Category.MANIFEST, Code.UNABLE_TO_GUESS_MANIFEST_TYPE, uri, message);
      case 'key-system': {
        const error = critical(Category.DRM, Code.REQUESTED_KEY_SYSTEM_CONFIG_UNAVAILABLE);
        error.message += ` -- ${message}`;
        return error;
      }
      case 'licence':
        return critical(Category.DRM, Code.LICENSE_REQUEST_FAILED, new Error(message));
      default: {
        const code = failure && failure.mediaError ? failure.mediaError.code : 4;
        return critical(Category.MEDIA, Code.VIDEO_ERROR, code, 0, message);
      }
    }
  }

  async unload(initializeMediaSource = true) {
    void initializeMediaSource;
    if (this.destroyed_) return;
    this.loadToken_++;
    this.interruptPending_(loadInterrupted());
    const loaded = this.loadMode_ === LoadMode.SRC_EQUALS || this.assetUri_ !== null;
    if (loaded) this.dispatchEvent(new FakeEvent('unloading'));
    this.resetContent_();
    if (loaded) this.dispatchEvent(new FakeEvent('onstatechange', { state: 'unload' }));
  }

  resetContent_() {
    if (this.controller_) this.controller_.unload();
    this.loadMode_ = LoadMode.NOT_LOADED;
    this.assetUri_ = null;
    this.mimeType_ = null;
    this.keySystem_ = '';
    this.drmInfo_ = null;
    this.selectedText_ = null;
    this.pendingVariant_ = null;
    this.activeVariant_ = -1;
    this.hasVideo_ = true;
    this.hasAudio_ = true;
    if (this.buffering_) {
      this.buffering_ = false;
      this.dispatchEvent(new FakeEvent('buffering', { buffering: false }));
    }
  }

  async destroy() {
    if (this.destroyed_) return;
    if (this.video_) await this.detach();
    else await this.unload();
    this.destroyed_ = true;
    this.loadMode_ = LoadMode.DESTROYED;
    await this.networkingEngine_.destroy();
    this.release();
  }

  // --- what the platform player reports --------------------------------------------------------

  onPlayerEvent_(type, payload) {
    if (this.loadMode_ === LoadMode.DESTROYED) return;
    switch (type) {
      case 'metadata':
        this.hasVideo_ = payload.hasVideo !== false;
        this.hasAudio_ = payload.hasAudio !== false;
        break;
      case 'state': {
        const buffering = payload.state === 'loading' || payload.state === 'buffering';
        if (buffering !== this.buffering_) {
          this.buffering_ = buffering;
          this.dispatchEvent(new FakeEvent('buffering', { buffering }));
        }
        break;
      }
      case 'tracks':
        if (this.loadMode_ === LoadMode.SRC_EQUALS) this.dispatchEvent(new FakeEvent('trackschanged'));
        break;
      case 'variant':
        this.onVariant_(payload.id);
        break;
      case 'licence':
        this.onLicence_(payload);
        break;
      case 'error':
        // A licence exchange this player already failed has said so.
        if (payload.kind === 'licence' && this.licenceFailed_ === this.loadToken_) break;
        // During a load the load's promise reports it; afterwards, an event.
        if (this.loadMode_ === LoadMode.SRC_EQUALS) {
          this.dispatchEvent(new FakeEvent('error', { detail: this.errorFromFailure_(payload, this.assetUri_) }));
        }
        break;
    }
  }

  onVariant_(id) {
    const previous = this.activeVariant_;
    this.activeVariant_ = id;
    if (previous === id) return;
    const tracks = this.getVariantTracks();
    const oldTrack = tracks.find((t) => t.id === previous) || null;
    const newTrack = tracks.find((t) => t.id === id) || null;
    const manual = this.pendingVariant_ === id;
    if (manual) this.pendingVariant_ = null;
    this.stats_.switchHistory.push({ timestamp: Date.now() / 1000, id, type: 'variant', fromAdaptation: !manual,
                                     bandwidth: newTrack ? newTrack.bandwidth : null });
    if (previous < 0 && !manual) return;
    this.dispatchEvent(new FakeEvent(manual ? 'variantchanged' : 'adaptation', { oldTrack, newTrack }));
  }

  // The key system wants a licence: Shaka's request, through the filters, to the
  // configured server, and its answer back to the key system.
  async onLicence_(payload) {
    const token = this.loadToken_;
    const controller = this.controller_;
    const drmInfo = this.drmInfo_;
    const server = drmInfo && drmInfo.licenseServerUri;
    if (!server) {
      controller.provideLicence(payload.requestId, null);
      this.licenceFailed_ = token;
      this.failLoad_(critical(Category.DRM, Code.NO_LICENSE_SERVER_GIVEN));
      return;
    }
    const request = NetworkingEngine.makeRequest([server], this.config_.drm.retryParameters);
    request.method = 'POST';
    request.body = payload.challenge;
    request.licenseRequestType = 'license-request';
    request.sessionId = '';
    request.drmInfo = drmInfo;
    const fairPlay = isFairPlay(drmInfo.keySystem);
    request.initData = fairPlay && payload.contentId ? new Uint8Array(StringUtils.toUTF16(payload.contentId, true)) : null;
    request.initDataType = fairPlay ? 'skd' : 'cenc';
    const advanced = this.config_.drm.advanced[drmInfo.keySystem];
    if (advanced && advanced.headers) Object.assign(request.headers, advanced.headers);
    const started = Date.now();
    try {
      const response = await this.networkingEngine_.request(RequestType.LICENSE, request,
                                                             { type: RequestType.LICENSE }).promise;
      if (token !== this.loadToken_) return;
      this.stats_.licenseTime += (Date.now() - started) / 1000;
      if (this.config_.drm.logLicenseExchange) {
        log.info('licence request', Uint8ArrayUtils.toStandardBase64(request.body),
                 'response', Uint8ArrayUtils.toStandardBase64(response.data));
      }
      controller.provideLicence(payload.requestId, toArrayBuffer(response.data));
      this.dispatchEvent(new FakeEvent('drmsessionupdate'));
    } catch (err) {
      if (token !== this.loadToken_) return;
      controller.provideLicence(payload.requestId, null);
      this.licenceFailed_ = token;
      const error = critical(Category.DRM, Code.LICENSE_REQUEST_FAILED, err);
      try {
        this.config_.drm.failureCallback(error);
      } catch (callbackError) {
        log.alwaysError('Error in failureCallback:', callbackError);
      }
      if (!error.handled) this.failLoad_(error);
    }
  }

  // Fail the load in progress with `error`, or -- once loaded -- report it.
  failLoad_(error) {
    if (this.pendingLoad_) this.interruptPending_(error);
    else this.dispatchEvent(new FakeEvent('error', { detail: error }));
  }

  setState_(state) {
    const s = this.stats_;
    const now = Date.now() / 1000;
    if (s.state === state) return;
    this.accountState_(now);
    s.state = state;
    s.stateSince = now;
    s.stateHistory.push({ timestamp: now, state, duration: 0 });
  }

  accountState_(now) {
    const s = this.stats_;
    if (s.state === null) return;
    const spent = now - s.stateSince;
    if (s.state === 'playing') s.playTime += spent;
    else if (s.state === 'paused') s.pauseTime += spent;
    else if (s.state === 'buffering') s.bufferingTime += spent;
    s.stateSince = now;
    const last = s.stateHistory[s.stateHistory.length - 1];
    if (last) last.duration += spent;
  }

  // --- configuration -----------------------------------------------------------------------------

  configure(config, value) {
    if (arguments.length === 2 && typeof config === 'string') config = objectFromPath(config, value);
    if (!isPlainObject(config)) {
      log.alwaysError('configure() takes an object, or a path and a value');
      return false;
    }
    const rest = applyLegacyPreferences(this.config_, config);
    const valid = mergeConfig(this.config_, rest, defaultConfiguration(), '');
    if (this.controller_ && this.loadMode_ === LoadMode.SRC_EQUALS &&
        (config.abr !== undefined || config.restrictions !== undefined)) {
      this.controller_.setAbr(this.abrSettings_());
    }
    return valid;
  }

  getConfiguration() { return cloneConfig(this.config_); }
  getNonDefaultConfiguration() {
    const diff = (current, base) => {
      const out = {};
      for (const key of Object.keys(current)) {
        const a = current[key], b = base ? base[key] : undefined;
        if (isPlainObject(a) && isPlainObject(b)) {
          const inner = diff(a, b);
          if (Object.keys(inner).length) out[key] = inner;
        } else if (JSON.stringify(a) !== JSON.stringify(b) && typeof a !== 'function') {
          out[key] = cloneConfig(a);
        }
      }
      return out;
    };
    return diff(this.config_, defaultConfiguration());
  }
  resetConfiguration() {
    this.config_ = defaultConfiguration();
    if (this.controller_ && this.loadMode_ === LoadMode.SRC_EQUALS) this.controller_.setAbr(this.abrSettings_());
  }
  getConfigurationForLowLatency() { return {}; }
  configurationForLowLatency(config) { this.configure(config); }

  // ABR for the platform player: `abr.enabled`, and the tighter of the ABR and
  // player restrictions.
  abrSettings_() {
    const abr = this.config_.abr.restrictions, all = this.config_.restrictions;
    return {
      enabled: this.config_.abr.enabled,
      minBandwidth: Math.max(abr.minBandwidth, all.minBandwidth),
      maxBandwidth: Math.min(abr.maxBandwidth, all.maxBandwidth),
      minWidth: Math.max(abr.minWidth, all.minWidth),
      maxWidth: Math.min(abr.maxWidth, all.maxWidth),
      minHeight: Math.max(abr.minHeight, all.minHeight),
      maxHeight: Math.min(abr.maxHeight, all.maxHeight),
    };
  }

  // --- tracks --------------------------------------------------------------------------------------

  loaded_() { return !!this.controller_ && this.loadMode_ === LoadMode.SRC_EQUALS; }

  variantTrack_(v, activeId) {
    const codecs = [v.videoCodec, v.audioCodec].filter(Boolean).join(', ');
    return {
      id: v.id, active: activeId >= 0 ? v.id === activeId : !!v.active, type: 'variant',
      bandwidth: v.bandwidth || 0, language: v.language || 'und', label: v.label || null, videoLabel: null,
      kind: null, width: v.width || null, height: v.height || null, frameRate: v.frameRate || null,
      pixelAspectRatio: null, hdr: null, colorGamut: null, videoLayout: null, mimeType: this.mimeType_,
      audioMimeType: null, videoMimeType: null, codecs: codecs || null, audioCodec: v.audioCodec || null,
      videoCodec: v.videoCodec || null, primary: false, roles: [], audioRoles: [], videoRoles: [],
      audioLanguage: v.language || null, videoLanguage: null, accessibilityPurpose: null, forced: false,
      videoId: v.id, audioId: v.audioId >= 0 ? v.audioId : null, audioGroupId: null,
      channelsCount: v.channels || null, audioSamplingRate: null, tilesLayout: null, audioBandwidth: null,
      videoBandwidth: null, spatialAudio: false, originalVideoId: null, originalAudioId: null,
      originalTextId: null, originalImageId: null, originalLanguage: v.language || null,
    };
  }

  getVariantTracks() {
    if (!this.loaded_()) return [];
    const info = this.controller_.info();
    const active = info.variant >= 0 ? info.variant : this.activeVariant_;
    return this.controller_.tracks().variants.map((v) => this.variantTrack_(v, active));
  }

  getTextTracks() {
    if (!this.loaded_()) return [];
    return this.controller_.tracks().text.map((t) => ({
      id: t.id, active: this.selectedText_ === t.id, type: 'text', bandwidth: 0, language: t.language || 'und',
      label: t.label || null, kind: t.kind === 'captions' ? 'caption' : 'subtitle', mimeType: t.mimeType || null,
      codecs: null, primary: false, roles: [], accessibilityPurpose: null, forced: !!t.forced,
      originalTextId: String(t.id), originalLanguage: t.language || null,
    }));
  }

  getAudioTracks() {
    if (!this.loaded_()) return [];
    return this.controller_.tracks().audio.map((a) => ({
      active: !!a.active, language: a.language || 'und', label: a.label || null, mimeType: null,
      codecs: a.codec || null, primary: false, roles: a.role ? [a.role] : [], accessibilityPurpose: null,
      channelsCount: a.channels || null, audioSamplingRate: null, spatialAudio: false,
      originalLanguage: a.language || null,
    }));
  }

  getVideoTracks() {
    const seen = new Map();
    for (const t of this.getVariantTracks()) {
      if (!t.width && !t.height) continue;
      const key = [t.width, t.height, t.frameRate, t.videoCodec].join('/');
      const existing = seen.get(key);
      if (!existing || t.active) {
        seen.set(key, { active: t.active || !!(existing && existing.active), bandwidth: t.bandwidth,
                        width: t.width, height: t.height, frameRate: t.frameRate, pixelAspectRatio: null,
                        hdr: null, colorGamut: null, videoLayout: null, mimeType: t.mimeType,
                        codecs: t.videoCodec, roles: [], label: null });
      }
    }
    return Array.from(seen.values());
  }

  getImageTracks() { return []; }
  getChaptersTracks() { return []; }
  getChaptersAsync() { return Promise.resolve([]); }
  getThumbnails() { return Promise.resolve(null); }
  getAllThumbnails() { return Promise.resolve(null); }

  getAudioLanguages() { return Array.from(new Set(this.audioChoices_().map((c) => c.language))); }
  getAudioLanguagesAndRoles() { return this.audioChoices_(); }
  getTextLanguages() { return Array.from(new Set(this.getTextTracks().map((t) => t.language))); }
  getTextLanguagesAndRoles() {
    return this.getTextTracks().map((t) => ({ language: t.language, role: '', label: t.label }));
  }
  audioChoices_() {
    const audio = this.getAudioTracks();
    const from = audio.length > 0 ? audio.map((a) => ({ language: a.language, role: a.roles[0] || '', label: a.label }))
                                  : this.getVariantTracks().map((v) => ({ language: v.language, role: '', label: null }));
    const seen = new Set();
    return from.filter((c) => {
      const key = c.language + '/' + c.role;
      if (seen.has(key)) return false;
      seen.add(key);
      return true;
    });
  }

  selectVariantTrack(track, clearBuffer = false, safeMargin = 0) {
    void clearBuffer; void safeMargin;
    if (!this.loaded_() || !track) return;
    if (this.config_.abr.enabled) {
      log.alwaysWarn('Changing tracks while abr manager is enabled will likely result in the selected track ' +
                     'being overriden. Consider disabling abr before calling selectVariantTrack().');
    }
    this.pendingVariant_ = track.id;
    this.controller_.selectVariant(track.id);
    if (this.activeVariant_ === track.id) {
      this.pendingVariant_ = null;
      const same = this.getVariantTracks().find((t) => t.id === track.id) || track;
      setTimeout(() => this.dispatchEvent(new FakeEvent('variantchanged', { oldTrack: same, newTrack: same })), 0);
    }
  }

  selectVideoTrack(videoTrack, clearBuffer = false, safeMargin = 0) {
    const match = this.getVariantTracks().find((t) => t.width === videoTrack.width && t.height === videoTrack.height &&
                                                      (!videoTrack.frameRate || t.frameRate === videoTrack.frameRate));
    if (match) this.selectVariantTrack(match, clearBuffer, safeMargin);
  }

  selectAudioTrack(audioTrack) {
    if (!audioTrack) return;
    this.selectAudioLanguage(audioTrack.language, audioTrack.roles && audioTrack.roles[0]);
  }

  selectAudioLanguage(language, role) {
    if (!this.loaded_()) return;
    this.controller_.selectAudioLanguage(language || '', role || '');
    setTimeout(() => this.dispatchEvent(new FakeEvent('variantchanged', {})), 0);
  }

  // Shaka 5: a text track selected is shown; null hides text.
  selectTextTrack(track = null) {
    if (!this.loaded_()) return;
    if (!track) {
      const was = this.textVisible_;
      this.selectedText_ = null;
      this.textVisible_ = false;
      this.controller_.selectText(-1, false);
      setTimeout(() => {
        this.dispatchEvent(new FakeEvent('textchanged'));
        if (was) this.dispatchEvent(new FakeEvent('texttrackvisibility'));
      }, 0);
      return;
    }
    const wasVisible = this.textVisible_;
    this.selectedText_ = track.id;
    this.textVisible_ = true;
    this.controller_.selectText(track.id, true);
    setTimeout(() => {
      this.dispatchEvent(new FakeEvent('textchanged'));
      if (!wasVisible) this.dispatchEvent(new FakeEvent('texttrackvisibility'));
    }, 0);
  }

  selectTextLanguage(language, role = '', forced = false) {
    void role;
    const track = this.getTextTracks().find((t) => t.language === language && (!forced || t.forced));
    if (track) this.selectTextTrack(track);
  }

  // Shaka 4's visibility switch, kept: the selected track shows (mode
  // 'showing' on its TextTrack) or stays selected but hidden ('hidden').
  setTextTrackVisibility(isVisible) {
    const visible = !!isVisible;
    if (visible === this.textVisible_) return;
    this.textVisible_ = visible;
    if (this.loaded_()) {
      if (visible && this.selectedText_ === null) {
        const preferred = this.config_.preferredText[0];
        const tracks = this.getTextTracks();
        const pick = (preferred && tracks.find((t) => t.language === preferred.language)) || tracks[0];
        if (pick) this.selectedText_ = pick.id;
      }
      if (this.selectedText_ !== null) this.controller_.selectText(this.selectedText_, visible);
    }
    setTimeout(() => this.dispatchEvent(new FakeEvent('texttrackvisibility')), 0);
  }

  isTextTrackVisible() { return this.textVisible_; }

  applyTextPreference_() {
    const preferred = this.config_.preferredText[0];
    if (!preferred || !preferred.language) return;
    const track = this.getTextTracks().find((t) => t.language === preferred.language);
    if (track) this.selectTextTrack(track);
  }

  // --- where playback is -----------------------------------------------------------------------------

  isLive() { return this.loaded_() && this.controller_.info().live; }
  isDynamic() { return this.isLive(); }
  isInProgress() { return false; }
  isAudioOnly() { return this.loaded_() && !this.hasVideo_; }
  isVideoOnly() { return this.loaded_() && !this.hasAudio_; }
  isBuffering() { return this.loaded_() && this.controller_.info().buffering; }
  isEnded() { return !!(this.video_ && this.video_.ended); }
  isRemotePlayback() { return false; }

  seekRange() {
    if (!this.loaded_()) return { start: 0, end: 0 };
    const info = this.controller_.info();
    if (info.live) return { start: info.seekStart, end: info.seekEnd };
    return { start: 0, end: isFinite(info.duration) ? info.duration : 0 };
  }

  goToLive() {
    if (this.isLive()) this.video_.currentTime = this.seekRange().end;
  }

  getBufferedInfo() {
    const total = [];
    const buffered = this.video_ ? this.video_.buffered : null;
    for (let i = 0; buffered && i < buffered.length; i++) total.push({ start: buffered.start(i), end: buffered.end(i) });
    return { total, audio: [], video: [], text: [] };
  }

  getBufferFullness() {
    if (!this.video_) return 0;
    const now = this.video_.currentTime;
    const ahead = this.getBufferedInfo().total.reduce((n, r) => (r.start <= now && r.end > now ? r.end - now : n), 0);
    return Math.min(1, ahead / Math.max(1, this.config_.streaming.bufferingGoal));
  }

  isFullyLoaded() {
    if (!this.loaded_() || this.isLive()) return false;
    const end = this.seekRange().end;
    return this.getBufferedInfo().total.some((r) => r.end >= end - 0.5);
  }

  getStats() {
    const s = this.stats_;
    this.accountState_(Date.now() / 1000);
    const native = this.loaded_() ? this.controller_.stats() || {} : {};
    const number = (value) => (typeof value === 'number' ? value : NaN);
    const duration = this.video_ ? this.video_.duration : NaN;
    const live = this.isLive();
    return {
      width: native.width || NaN, height: native.height || NaN,
      streamBandwidth: number(native.streamBandwidth), currentCodecs: '',
      decodedFrames: number(native.decodedFrames), droppedFrames: number(native.droppedFrames),
      corruptedFrames: number(native.corruptedFrames), estimatedBandwidth: number(native.estimatedBandwidth),
      completionPercent: !live && this.video_ && isFinite(duration) && duration > 0
        ? Math.round(this.video_.currentTime / duration * 100) : NaN,
      loadLatency: s.loadLatency, timeToFirstFrame: s.firstFrame, manifestTimeSeconds: NaN,
      drmTimeSeconds: s.drmTime, playTime: s.playTime, pauseTime: s.pauseTime, bufferingTime: s.bufferingTime,
      licenseTime: s.licenseTime, liveLatency: live && this.video_ ? this.seekRange().end - this.video_.currentTime : NaN,
      maxSegmentDuration: NaN, gapsJumped: 0, stallsDetected: 0, manifestSizeBytes: NaN, bytesDownloaded: NaN,
      nonFatalErrorCount: 0, manifestPeriodCount: NaN, manifestGapCount: NaN,
      switchHistory: s.switchHistory.map((e) => Object.assign({}, e)),
      stateHistory: s.stateHistory.map((e) => Object.assign({}, e)),
    };
  }

  // --- about what is loaded ---------------------------------------------------------------------------

  getLoadMode() { return this.loadMode_; }
  getAssetUri() { return this.assetUri_; }
  getMimeType() { return this.mimeType_; }
  getManifestType() {
    if (!this.loaded_()) return null;
    const manifest = this.controller_.info().manifest;
    return manifest === 'hls' ? 'HLS' : manifest === 'dash' ? 'DASH' : manifestTypeOf(this.mimeType_ || '');
  }
  getManifest() { return null; }
  getManifestParserFactory() { return null; }
  keySystem() { return this.keySystem_; }
  drmInfo() { return this.drmInfo_ ? Object.assign({}, this.drmInfo_) : null; }
  getExpiration() { return Infinity; }
  getKeyStatuses() { return {}; }
  getActiveSessionsMetadata() { return []; }
  getPlaybackRate() { return this.video_ ? this.video_.playbackRate : 0; }
  getPresentationStartTimeAsDate() { return null; }
  getPlayheadTimeAsDate() { return null; }
  getSegmentAvailabilityDuration() {
    if (!this.isLive()) return null;
    const range = this.seekRange();
    return range.end - range.start;
  }
  getFetchedPlaybackInfo() {
    const variant = this.getVariantTracks().find((t) => t.active) || null;
    return { video: variant, audio: variant, text: null };
  }
  getAllTimelineRegions() { return []; }
  getAllEmsgRegions() { return []; }
  getAllMetadataRegions() { return []; }
  getAdManager() { return null; }
  getQueueManager() { return null; }

  trickPlay(rate) {
    if (!this.video_) return;
    if (rate > 0 && rate <= 16) this.video_.playbackRate = rate;
    else log.alwaysWarn(`trickPlay(${rate}): a platform player plays forwards only, at up to 16x`);
  }
  cancelTrickPlay() { if (this.video_) this.video_.playbackRate = this.video_.defaultPlaybackRate; }
  useTrickPlayTrackIfAvailable() {}
  updateStartTime(startTime) { this.startTime_ = startTime; }
  setMaxHardwareResolution() {}
  attachCanvas() {}
  releaseAllMutexes() {}
  retryStreaming() { return false; }
  renewLicense() {}
  retryLicensing() { return Promise.resolve(false); }
  destroyAllPreloads() {}

  // Out-of-band text, thumbnails and chapters need Shaka's own text engine and
  // segment index, which a platform player does not expose: they fail as Shaka
  // fails them in src= mode.
  addTextTrackAsync() {
    return Promise.reject(critical(Category.TEXT, Code.CANNOT_ADD_EXTERNAL_TEXT_TO_SRC_EQUALS));
  }
  addThumbnailsTrack() {
    return Promise.reject(critical(Category.TEXT, Code.CANNOT_ADD_EXTERNAL_THUMBNAILS_TO_SRC_EQUALS));
  }
  addChaptersTrack() { return Promise.reject(critical(Category.TEXT, Code.CHAPTERS_TRACK_FAILED)); }
  addFont() { return Promise.resolve(); }
  preload() { return Promise.reject(critical(Category.PLAYER, Code.SRC_EQUALS_PRELOAD_NOT_SUPPORTED)); }
  detachAndSavePreload() { return this.detach().then(() => null); }
  unloadAndSavePreload() { return this.unload().then(() => null); }
}
Player.LoadMode = LoadMode;
Player.version = VERSION;

// ---- the namespace ---------------------------------------------------------------------------------

shaka.Player = Player;
shaka.log = log;
// Nothing to polyfill: the runtime is the platform, and there is no MSE to patch.
shaka.polyfill = { installAll() {}, register() {} };
shaka.util = {
  Error: ShakaError, FakeEvent, FakeEventTarget, EventManager, AbortableOperation, StringUtils, Uint8ArrayUtils,
  BufferUtils,
};
shaka.net = { NetworkingEngine };
shaka.drm = { FairPlay };
shaka.dependencies = { add() {}, has() { return false; } };
shaka.config = {
  AutoShowText: { NEVER: 0, ALWAYS: 1, IF_PREFERRED_TEXT_LANGUAGE: 2, IF_SUBTITLES_MAY_BE_NEEDED: 3 },
  CodecSwitchingStrategy: { RELOAD: 'reload', SMOOTH: 'smooth' },
  CrossBoundaryStrategy: { KEEP: 'keep', RESET: 'reset', RESET_TO_ENCRYPTED: 'reset_to_encrypted' },
  RepeatMode: { OFF: 0, ALL: 1, SINGLE: 2 },
};

// ---- module exports: runtime/tests evaluates everything above this line as a script -------------
export default shaka;
export { Player, log, ShakaError as Error };
export const { util, net, polyfill, drm } = shaka;
