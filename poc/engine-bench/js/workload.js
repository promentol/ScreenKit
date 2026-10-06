// Phaser-shaped work for comparing JavaScript engines -- no Phaser, no graphics.
//
// What a Phaser game's JavaScript does every frame, written out plainly: game objects behind
// accessor properties, a scene with an event emitter and an update list, tweens, a particle
// emitter, arcade physics against a tilemap and between bodies, animations looked up by frame
// name, bitmap text layout, Graphics paths ear-clipped into triangles, a depth-sorted display
// list, and the render walk that turns all of it into vertices in a typed array. Nothing is
// drawn: the "renderer" fills a vertex buffer and counts a draw call where WebGL would take one.
//
// The shapes follow Phaser 3/4 closely enough to load an engine the same way (the same kinds of
// objects, the same property traffic, the same garbage), not to reproduce Phaser's numbers.
// The game logic is examples/phaser-stress's (src/stress.js): the same level, the same bunnymark.
//
// Plain ES5, no modules: Hermes (as bytecode) and SpiderMonkey (as source) run this same file.
// The host drives it through one global:
//
//   bench.info()                 JSON: the scenes, their units and default loads
//   bench.plan(optionsJson)      "scene load" lines: what to run, in order
//   bench.setView(w, h)          the screen the camera fits the 720-unit world into
//   bench.enter(scene, load)     build a scene at a load (objects are created here)
//   bench.frame()                one game step: update, tweens, physics, render walk
//   bench.exit()                 JSON: a checksum of the scene's state and render counts
//
// Time is fixed at 60 steps per second of game time whatever the engine's speed, so every
// engine does exactly the same work; the host measures how long that work takes.
(function (global) {
  'use strict';

  var VERSION = 1;
  var WORLD_HEIGHT = 720;
  var TILE = 32;
  var FRAME_MS = 1000 / 60;
  var PI2 = Math.PI * 2;
  var view = { width: 640, height: 480 };
  var nextId = 1;

  // ---- math ------------------------------------------------------------------

  // mulberry32, as examples/phaser-stress: every engine builds the same scenes.
  function Random(seed) {
    this.s = seed | 0;
  }
  Random.prototype.next = function () {
    var s = (this.s = (this.s + 0x6d2b79f5) | 0);
    var t = Math.imul(s ^ (s >>> 15), 1 | s);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
  Random.prototype.between = function (min, max) {
    return min + (max - min) * this.next();
  };
  Random.prototype.integer = function (min, max) {
    return min + Math.floor((max - min + 1) * this.next());
  };

  function wrap(value, min, max) {
    var range = max - min;
    return min + ((((value - min) % range) + range) % range);
  }
  function wrapAngle(angle) {
    return wrap(angle, -Math.PI, Math.PI);
  }
  function clamp(value, min, max) {
    return value < min ? min : value > max ? max : value;
  }
  function lerpColor(from, to, t) {
    var r = (from >> 16) & 0xff, g = (from >> 8) & 0xff, b = from & 0xff;
    r += (((to >> 16) & 0xff) - r) * t;
    g += (((to >> 8) & 0xff) - g) * t;
    b += ((to & 0xff) - b) * t;
    return ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
  }
  // Phaser's getTintAppendFloatAlpha: ABGR in one 32-bit word.
  function packColor(tint, alpha) {
    var a = ((alpha * 255) | 0) & 0xff;
    return ((a << 24) | ((tint & 0xff) << 16) | (tint & 0xff00) | ((tint >> 16) & 0xff)) >>> 0;
  }

  // Looked up by name when a tween is made, as Phaser's GetEaseFunction does.
  var EASE = {
    Linear: function (v) { return v; },
    'Quad.easeOut': function (v) { return v * (2 - v); },
    'Cubic.easeIn': function (v) { return v * v * v; },
    'Sine.easeInOut': function (v) { return -0.5 * (Math.cos(Math.PI * v) - 1); },
    'Back.easeOut': function (v) {
      var s = 1.70158;
      v -= 1;
      return v * v * ((s + 1) * v + s) + 1;
    },
    'Bounce.easeOut': function (v) {
      if (v < 1 / 2.75) return 7.5625 * v * v;
      if (v < 2 / 2.75) { v -= 1.5 / 2.75; return 7.5625 * v * v + 0.75; }
      if (v < 2.5 / 2.75) { v -= 2.25 / 2.75; return 7.5625 * v * v + 0.9375; }
      v -= 2.625 / 2.75;
      return 7.5625 * v * v + 0.984375;
    },
  };
  function getEase(name) {
    return EASE[name] || EASE.Linear;
  }

  function Vector2(x, y) {
    this.x = x || 0;
    this.y = y || 0;
  }
  Vector2.prototype.set = function (x, y) {
    this.x = x;
    this.y = y === undefined ? x : y;
    return this;
  };

  // Phaser's TransformMatrix: a 2x3 affine matrix in a Float32Array.
  function TransformMatrix() {
    this.matrix = new Float32Array([1, 0, 0, 1, 0, 0]);
    this.quad = new Float32Array(8);
  }
  var TM = TransformMatrix.prototype;
  TM.copyFrom = function (src) {
    var m = this.matrix, s = src.matrix;
    m[0] = s[0]; m[1] = s[1]; m[2] = s[2]; m[3] = s[3]; m[4] = s[4]; m[5] = s[5];
    return this;
  };
  TM.applyITRS = function (x, y, rotation, scaleX, scaleY) {
    var m = this.matrix, sr = Math.sin(rotation), cr = Math.cos(rotation);
    m[4] = x;
    m[5] = y;
    m[0] = cr * scaleX;
    m[1] = sr * scaleX;
    m[2] = -sr * scaleY;
    m[3] = cr * scaleY;
    return this;
  };
  TM.multiply = function (rhs, out) {
    var m = this.matrix, s = rhs.matrix;
    var a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5];
    var sa = s[0], sb = s[1], sc = s[2], sd = s[3], se = s[4], sf = s[5];
    var target = out || this, dest = target.matrix;
    dest[0] = sa * a + sb * c;
    dest[1] = sa * b + sb * d;
    dest[2] = sc * a + sd * c;
    dest[3] = sc * b + sd * d;
    dest[4] = se * a + sf * c + e;
    dest[5] = se * b + sf * d + f;
    return target;
  };
  TM.multiplyWithOffset = function (rhs, offsetX, offsetY) {
    var m = this.matrix, o = rhs.matrix;
    var a0 = m[0], b0 = m[1], c0 = m[2], d0 = m[3], tx0 = m[4], ty0 = m[5];
    var pse = offsetX * a0 + offsetY * c0 + tx0;
    var psf = offsetX * b0 + offsetY * d0 + ty0;
    var a1 = o[0], b1 = o[1], c1 = o[2], d1 = o[3], tx1 = o[4], ty1 = o[5];
    m[0] = a1 * a0 + b1 * c0;
    m[1] = a1 * b0 + b1 * d0;
    m[2] = c1 * a0 + d1 * c0;
    m[3] = c1 * b0 + d1 * d0;
    m[4] = tx1 * a0 + ty1 * c0 + pse;
    m[5] = tx1 * b0 + ty1 * d0 + psf;
    return this;
  };
  TM.setQuad = function (x, y, xw, yh) {
    var m = this.matrix, q = this.quad;
    var a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5];
    q[0] = x * a + y * c + e;
    q[1] = x * b + y * d + f;
    q[2] = x * a + yh * c + e;
    q[3] = x * b + yh * d + f;
    q[4] = xw * a + yh * c + e;
    q[5] = xw * b + yh * d + f;
    q[6] = xw * a + y * c + e;
    q[7] = xw * b + y * d + f;
    return q;
  };

  // Phaser's StableSort (the `stable` package): a merge sort into a fresh buffer.
  function stableSort(arr, comp) {
    var len = arr.length;
    if (len <= 1) return arr;
    var input = arr, buffer = new Array(len);
    for (var chk = 1; chk < len; chk *= 2) {
      mergePass(input, comp, chk, buffer);
      var tmp = input;
      input = buffer;
      buffer = tmp;
    }
    if (input !== arr) mergePass(input, null, len, arr);
    return arr;
  }
  function mergePass(arr, comp, chk, result) {
    var len = arr.length, i = 0, dbl = chk * 2;
    for (var l = 0; l < len; l += dbl) {
      var r = Math.min(l + chk, len), e = Math.min(r + chk, len), li = l, ri = r;
      for (;;) {
        if (li < r && ri < e) {
          if (comp(arr[li], arr[ri]) <= 0) result[i++] = arr[li++];
          else result[i++] = arr[ri++];
        } else if (li < r) result[i++] = arr[li++];
        else if (ri < e) result[i++] = arr[ri++];
        else break;
      }
    }
  }

  // Earcut's ear clipping (without its z-order hash), which is how Phaser fills a Graphics
  // path: a linked node per vertex, allocated every time the path is filled.
  function EarNode(i, x, y) {
    this.i = i;
    this.x = x;
    this.y = y;
    this.prev = null;
    this.next = null;
  }
  function earInsert(i, x, y, last) {
    var p = new EarNode(i, x, y);
    if (!last) {
      p.prev = p;
      p.next = p;
    } else {
      p.next = last.next;
      p.prev = last;
      last.next.prev = p;
      last.next = p;
    }
    return p;
  }
  function earArea(p, q, r) {
    return (q.y - p.y) * (r.x - q.x) - (q.x - p.x) * (r.y - q.y);
  }
  function pointInTriangle(ax, ay, bx, by, cx, cy, px, py) {
    return (cx - px) * (ay - py) >= (ax - px) * (cy - py) &&
      (ax - px) * (by - py) >= (bx - px) * (ay - py) &&
      (bx - px) * (cy - py) >= (cx - px) * (by - py);
  }
  function isEar(ear) {
    var a = ear.prev, b = ear, c = ear.next;
    if (earArea(a, b, c) >= 0) return false;
    var p = c.next;
    while (p !== a) {
      if (pointInTriangle(a.x, a.y, b.x, b.y, c.x, c.y, p.x, p.y) && earArea(p.prev, p, p.next) >= 0) return false;
      p = p.next;
    }
    return true;
  }
  function earcut(data) {
    var triangles = [], n = data.length, sum = 0, i, j, last = null;
    for (i = 0, j = n - 2; i < n; j = i, i += 2) sum += (data[j] - data[i]) * (data[i + 1] + data[j + 1]);
    if (sum > 0) for (i = 0; i < n; i += 2) last = earInsert(i, data[i], data[i + 1], last);
    else for (i = n - 2; i >= 0; i -= 2) last = earInsert(i, data[i], data[i + 1], last);
    if (!last || last.next === last.prev) return triangles;
    var ear = last, stop = last;
    while (ear.prev !== ear.next) {
      var prev = ear.prev, next = ear.next;
      if (isEar(ear)) {
        triangles.push(prev.i / 2, ear.i / 2, next.i / 2);
        next.prev = prev;
        prev.next = next;
        ear = next.next;
        stop = next.next;
        continue;
      }
      ear = next;
      if (ear === stop) break;
    }
    return triangles;
  }

  // ---- events ----------------------------------------------------------------

  // eventemitter3, which Phaser uses for everything: every game object is one of these.
  function EventEmitter() {
    this._events = Object.create(null);
  }
  var EE = EventEmitter.prototype;
  function addListener(emitter, event, fn, context, once) {
    var listener = { fn: fn, context: context || emitter, once: once };
    var list = emitter._events[event];
    if (!list) emitter._events[event] = [listener];
    else list.push(listener);
    return emitter;
  }
  EE.on = function (event, fn, context) {
    return addListener(this, event, fn, context, false);
  };
  EE.once = function (event, fn, context) {
    return addListener(this, event, fn, context, true);
  };
  EE.off = function (event, fn, context) {
    var list = this._events[event];
    if (!list) return this;
    var kept = [];
    for (var i = 0; i < list.length; i++) {
      var l = list[i];
      if (l.fn !== fn || (context && l.context !== context)) kept.push(l);
    }
    if (kept.length) this._events[event] = kept;
    else delete this._events[event];
    return this;
  };
  EE.emit = function (event, a1, a2, a3) {
    var list = this._events[event];
    if (!list) return false;
    var len = arguments.length;
    for (var i = 0; i < list.length; i++) {
      var l = list[i];
      if (l.once) this.off(event, l.fn, l.context);
      switch (len) {
        case 1: l.fn.call(l.context); break;
        case 2: l.fn.call(l.context, a1); break;
        case 3: l.fn.call(l.context, a1, a2); break;
        default: l.fn.call(l.context, a1, a2, a3); break;
      }
    }
    return true;
  };
  EE.removeAllListeners = function () {
    this._events = Object.create(null);
    return this;
  };

  // ---- textures --------------------------------------------------------------

  function Frame(texture, name, x, y, width, height) {
    this.texture = texture;
    this.name = name;
    this.cutX = x;
    this.cutY = y;
    this.width = width;
    this.height = height;
    this.halfWidth = width / 2;
    this.halfHeight = height / 2;
    this.u0 = x / texture.width;
    this.v0 = y / texture.height;
    this.u1 = (x + width) / texture.width;
    this.v1 = (y + height) / texture.height;
  }
  function Texture(key, index, width, height) {
    this.key = key;
    this.index = index;
    this.width = width;
    this.height = height;
    this.frames = new Map();
    this.add('__BASE', 0, 0, width, height);
  }
  Texture.prototype.add = function (name, x, y, width, height) {
    var frame = new Frame(this, name, x, y, width, height);
    this.frames.set(name, frame);
    return frame;
  };
  Texture.prototype.get = function (name) {
    return (name !== undefined && this.frames.get(name)) || this.frames.get('__BASE');
  };
  function TextureManager() {
    this.list = new Map();
  }
  TextureManager.prototype.create = function (key, width, height) {
    var texture = new Texture(key, this.list.size, width, height);
    this.list.set(key, texture);
    return texture;
  };
  TextureManager.prototype.get = function (key) {
    return this.list.get(key) || this.list.get('__MISSING');
  };

  var TEXTURES = new TextureManager();
  TEXTURES.create('__MISSING', 32, 32);
  (function () {
    // examples/phaser-stress's atlas, plus gems and icons.
    var atlas = TEXTURES.create('atlas', 256, 256);
    atlas.add('star', 0, 0, 32, 32);
    atlas.add('player-0', 32, 0, 24, 32);
    atlas.add('player-1', 56, 0, 24, 32);
    atlas.add('enemy-0', 80, 0, 28, 24);
    atlas.add('enemy-1', 108, 0, 28, 24);
    atlas.add('bullet', 136, 0, 12, 12);
    atlas.add('spark', 148, 0, 16, 16);
    for (var i = 0; i < 6; i++) atlas.add('gem-' + i, i * 44, 32, 44, 44);
    for (i = 0; i < 8; i++) atlas.add('icon-' + i, i * 24, 80, 24, 24);
    var tiles = TEXTURES.create('tiles', 128, 32);
    for (i = 0; i < 4; i++) tiles.add(String(i), i * 32, 0, 32, 32);
    TEXTURES.create('hills-far', 256, 256);
    TEXTURES.create('hills-near', 256, 256);
  })();

  // A BMFont-style bitmap font: glyph metrics and kerning pairs keyed by character code.
  var FONT = (function () {
    var texture = TEXTURES.create('font', 256, 128);
    var rand = new Random(21), chars = {}, x = 0, y = 0;
    for (var code = 32; code < 127; code++) {
      var width = 6 + Math.floor(rand.next() * 8);
      if (x + width > 256) { x = 0; y += 16; }
      chars[code] = {
        x: x, y: y, width: width, height: 16, xOffset: 0, yOffset: rand.integer(0, 3),
        xAdvance: width + 1, kerning: {}, frame: texture.add('c' + code, x, y, width, 16),
      };
      x += width;
    }
    for (var k = 0; k < 200; k++) {
      var a = rand.integer(65, 122), b = rand.integer(65, 122);
      if (chars[b]) chars[b].kerning[a] = -rand.integer(1, 2);
    }
    return { chars: chars, lineHeight: 18, size: 16, texture: texture };
  })();

  // ---- game objects ----------------------------------------------------------

  var FLAG_VISIBLE = 1, FLAG_ALPHA = 2, FLAG_SCALE = 4, FLAGS_ALL = 7;
  var BLEND_NORMAL = 0, BLEND_ADD = 1;

  // Phaser's GameObject with its Transform, Alpha, Visible and Depth components: the fields
  // behind accessors whose setters keep renderFlags and the display list's sort up to date.
  function GameObject(scene, type) {
    EventEmitter.call(this);
    this.id = nextId++;
    this.scene = scene;
    this.type = type;
    this.active = true;
    this.renderFlags = FLAGS_ALL;
    this.x = 0;
    this.y = 0;
    this._rotation = 0;
    this._scaleX = 1;
    this._scaleY = 1;
    this._alpha = 1;
    this._visible = true;
    this._depth = 0;
    this.scrollFactorX = 1;
    this.scrollFactorY = 1;
    this.parentContainer = null;
    this.body = null;
  }
  GameObject.prototype = Object.create(EventEmitter.prototype);
  GameObject.prototype.constructor = GameObject;
  var GO = GameObject.prototype;
  Object.defineProperties(GO, {
    rotation: {
      get: function () { return this._rotation; },
      set: function (value) { this._rotation = wrapAngle(value); },
    },
    angle: {
      get: function () { return (this._rotation * 180) / Math.PI; },
      set: function (value) { this.rotation = (value * Math.PI) / 180; },
    },
    scaleX: {
      get: function () { return this._scaleX; },
      set: function (value) {
        this._scaleX = value;
        if (value === 0) this.renderFlags &= ~FLAG_SCALE;
        else if (this._scaleY !== 0) this.renderFlags |= FLAG_SCALE;
      },
    },
    scaleY: {
      get: function () { return this._scaleY; },
      set: function (value) {
        this._scaleY = value;
        if (value === 0) this.renderFlags &= ~FLAG_SCALE;
        else if (this._scaleX !== 0) this.renderFlags |= FLAG_SCALE;
      },
    },
    scale: {
      get: function () { return (this._scaleX + this._scaleY) / 2; },
      set: function (value) {
        this._scaleX = value;
        this._scaleY = value;
        if (value === 0) this.renderFlags &= ~FLAG_SCALE;
        else this.renderFlags |= FLAG_SCALE;
      },
    },
    alpha: {
      get: function () { return this._alpha; },
      set: function (value) {
        var v = clamp(value, 0, 1);
        this._alpha = v;
        if (v === 0) this.renderFlags &= ~FLAG_ALPHA;
        else this.renderFlags |= FLAG_ALPHA;
      },
    },
    visible: {
      get: function () { return this._visible; },
      set: function (value) {
        this._visible = !!value;
        if (value) this.renderFlags |= FLAG_VISIBLE;
        else this.renderFlags &= ~FLAG_VISIBLE;
      },
    },
    depth: {
      get: function () { return this._depth; },
      set: function (value) {
        if (this.scene) this.scene.displayList.queueDepthSort();
        this._depth = value;
      },
    },
  });
  GO.setPosition = function (x, y) {
    this.x = x;
    this.y = y === undefined ? x : y;
    return this;
  };
  GO.setScale = function (x, y) {
    this.scaleX = x;
    this.scaleY = y === undefined ? x : y;
    return this;
  };
  GO.setRotation = function (radians) {
    this.rotation = radians;
    return this;
  };
  GO.setAlpha = function (value) {
    this.alpha = value;
    return this;
  };
  GO.setVisible = function (value) {
    this.visible = value;
    return this;
  };
  GO.setDepth = function (value) {
    this.depth = value;
    return this;
  };
  GO.setScrollFactor = function (x, y) {
    this.scrollFactorX = x;
    this.scrollFactorY = y === undefined ? x : y;
    return this;
  };
  GO.willRender = function () {
    return this.renderFlags === FLAGS_ALL;
  };
  GO.destroy = function () {
    var scene = this.scene;
    if (!scene) return;
    this.emit('destroy', this);
    scene.displayList.remove(this);
    if (this.preUpdate) {
      var i = scene.updateList.indexOf(this);
      if (i !== -1) scene.updateList.splice(i, 1);
    }
    if (this.body) scene.physics.remove(this.body);
    this.removeAllListeners();
    this.scene = null;
    this.active = false;
  };

  function Sprite(scene, x, y, key, frame) {
    GameObject.call(this, scene, 'Sprite');
    this.x = x;
    this.y = y;
    this.texture = scene.textures.get(key);
    this.frame = this.texture.get(frame);
    this.originX = 0.5;
    this.originY = 0.5;
    this.displayOriginX = this.frame.halfWidth;
    this.displayOriginY = this.frame.halfHeight;
    this.tint = 0xffffff;
    this.flipX = false;
    this.blendMode = BLEND_NORMAL;
    this.anims = null;
  }
  Sprite.prototype = Object.create(GameObject.prototype);
  Sprite.prototype.constructor = Sprite;
  var SP = Sprite.prototype;
  SP.setFrame = function (name) {
    this.frame = this.texture.get(name);
    this.displayOriginX = this.originX * this.frame.width;
    this.displayOriginY = this.originY * this.frame.height;
    return this;
  };
  SP.setOrigin = function (x, y) {
    this.originX = x;
    this.originY = y === undefined ? x : y;
    this.displayOriginX = this.originX * this.frame.width;
    this.displayOriginY = this.originY * this.frame.height;
    return this;
  };
  SP.setTint = function (tint) {
    this.tint = tint;
    return this;
  };
  SP.setBlendMode = function (mode) {
    this.blendMode = mode;
    return this;
  };
  SP.play = function (animation) {
    if (!this.anims) {
      this.anims = new AnimationState(this);
      this.scene.updateList.push(this);
    }
    this.anims.play(animation);
    return this;
  };
  SP.preUpdate = function (time, delta) {
    if (this.anims) this.anims.update(time, delta);
  };
  SP.renderWebGL = function (renderer, camera, parentMatrix) {
    renderer.batchSprite(this, this.frame, camera, parentMatrix);
  };

  // A TileSprite: a quad whose texture scrolls inside it (parallax backgrounds).
  function TileSprite(scene, x, y, width, height, key) {
    Sprite.call(this, scene, x, y, key);
    this.width = width;
    this.height = height;
    this.tilePositionX = 0;
    this.tilePositionY = 0;
    this.setOrigin(0);
  }
  TileSprite.prototype = Object.create(Sprite.prototype);
  TileSprite.prototype.constructor = TileSprite;
  TileSprite.prototype.renderWebGL = function (renderer, camera) {
    var frame = this.frame, texture = this.texture;
    var u0 = (this.tilePositionX % texture.width) / texture.width, v0 = (this.tilePositionY % texture.height) / texture.height;
    var calc = renderer.spriteCalc(this, camera, null);
    var q = calc.setQuad(0, 0, this.width, this.height);
    renderer.batchQuad(q, u0, v0, u0 + this.width / frame.width, v0 + this.height / frame.height, texture, this.tint,
      this._alpha * camera.alpha, this.blendMode);
  };

  function Animation(key, frames, frameRate) {
    this.key = key;
    this.frames = frames;
    this.msPerFrame = 1000 / frameRate;
  }
  var ANIMATIONS = {
    'player-run': new Animation('player-run', ['player-0', 'player-1'], 10),
    'enemy-walk': new Animation('enemy-walk', ['enemy-0', 'enemy-1'], 6),
  };
  function AnimationState(parent) {
    this.parent = parent;
    this.current = null;
    this.index = 0;
    this.accumulator = 0;
    this.timeScale = 1;
    this.isPlaying = false;
  }
  AnimationState.prototype.play = function (key) {
    this.current = ANIMATIONS[key];
    this.index = 0;
    this.accumulator = 0;
    this.isPlaying = true;
    this.parent.setFrame(this.current.frames[0]);
  };
  AnimationState.prototype.update = function (time, delta) {
    if (!this.isPlaying) return;
    var anim = this.current;
    this.accumulator += delta * this.timeScale;
    while (this.accumulator >= anim.msPerFrame) {
      this.accumulator -= anim.msPerFrame;
      this.index = (this.index + 1) % anim.frames.length;
      this.parent.setFrame(anim.frames[this.index]);
      this.parent.emit('animationupdate', anim, this.index);
    }
  };

  function Container(scene, x, y) {
    GameObject.call(this, scene, 'Container');
    this.x = x;
    this.y = y;
    this.list = [];
    this.localTransform = new TransformMatrix();
    this.worldTransform = new TransformMatrix();
  }
  Container.prototype = Object.create(GameObject.prototype);
  Container.prototype.constructor = Container;
  Container.prototype.add = function (child) {
    child.parentContainer = this;
    this.list.push(child);
    return this;
  };
  Container.prototype.destroy = function () {
    for (var i = this.list.length - 1; i >= 0; i--) this.list[i].destroy();
    this.list.length = 0;
    GO.destroy.call(this);
  };
  // Phaser's ContainerWebGLRenderer: children render through the container's matrix, and each
  // child's alpha is multiplied in through its own setter and put back after.
  Container.prototype.renderWebGL = function (renderer, camera, parentMatrix) {
    var local = this.localTransform.applyITRS(this.x, this.y, this._rotation, this._scaleX, this._scaleY);
    var transform = local;
    if (parentMatrix) transform = this.worldTransform.copyFrom(parentMatrix).multiply(local);
    var list = this.list, alpha = this._alpha;
    for (var i = 0; i < list.length; i++) {
      var child = list[i];
      if (!child.willRender(camera)) continue;
      var childAlpha = child._alpha;
      child.alpha = childAlpha * alpha;
      child.renderWebGL(renderer, camera, transform);
      child.alpha = childAlpha;
    }
  };

  function BitmapText(scene, x, y, text, size) {
    GameObject.call(this, scene, 'BitmapText');
    this.x = x;
    this.y = y;
    this.font = FONT;
    this.texture = FONT.texture;
    this.fontSize = size;
    this.letterSpacing = 0;
    this.tint = 0xffffff;
    this._text = '';
    this.glyphs = [];
    this.width = 0;
    this.height = 0;
    this.setText(text);
  }
  BitmapText.prototype = Object.create(GameObject.prototype);
  BitmapText.prototype.constructor = BitmapText;
  // Phaser lays the text out (GetBitmapTextSize) whenever it changes: an object per character.
  BitmapText.prototype.setText = function (value) {
    var text = String(value);
    if (text === this._text) return this;
    this._text = text;
    var chars = this.font.chars, glyphs = [], x = 0, y = 0, prev = -1, width = 0, lines = 1;
    for (var i = 0; i < text.length; i++) {
      var code = text.charCodeAt(i);
      if (code === 10) {
        x = 0;
        y += this.font.lineHeight;
        prev = -1;
        lines++;
        continue;
      }
      var glyph = chars[code];
      if (!glyph) continue;
      if (prev !== -1) {
        var kerning = glyph.kerning[prev];
        if (kerning) x += kerning;
      }
      glyphs.push({ glyph: glyph, x: x + glyph.xOffset, y: y + glyph.yOffset, code: code });
      x += glyph.xAdvance + this.letterSpacing;
      if (x > width) width = x;
      prev = code;
    }
    var scale = this.fontSize / this.font.size;
    this.glyphs = glyphs;
    this.width = width * scale;
    this.height = lines * this.font.lineHeight * scale;
    return this;
  };
  BitmapText.prototype.renderWebGL = function (renderer, camera, parentMatrix) {
    var scale = this.fontSize / this.font.size;
    var calc = renderer.calc(this.x, this.y, this._rotation, this._scaleX * scale, this._scaleY * scale, this, camera, parentMatrix);
    var glyphs = this.glyphs, alpha = this._alpha * camera.alpha;
    for (var i = 0; i < glyphs.length; i++) {
      var g = glyphs[i], frame = g.glyph.frame;
      var q = calc.setQuad(g.x, g.y, g.x + frame.width, g.y + frame.height);
      renderer.batchQuad(q, frame.u0, frame.v0, frame.u1, frame.v1, this.texture, this.tint, alpha, BLEND_NORMAL);
    }
  };

  // Phaser's Graphics: commands recorded into an array, turned into triangles when rendered.
  var CMD_FILL_STYLE = 0, CMD_FILL_RECT = 1, CMD_BEGIN_PATH = 2, CMD_ARC = 3, CMD_FILL_PATH = 4;
  function Graphics(scene) {
    GameObject.call(this, scene, 'Graphics');
    this.commandBuffer = [];
  }
  Graphics.prototype = Object.create(GameObject.prototype);
  Graphics.prototype.constructor = Graphics;
  var GR = Graphics.prototype;
  GR.clear = function () {
    this.commandBuffer.length = 0;
    return this;
  };
  GR.fillStyle = function (color, alpha) {
    this.commandBuffer.push(CMD_FILL_STYLE, color, alpha === undefined ? 1 : alpha);
    return this;
  };
  GR.fillRect = function (x, y, width, height) {
    this.commandBuffer.push(CMD_FILL_RECT, x, y, width, height);
    return this;
  };
  GR.fillCircle = function (x, y, radius) {
    this.commandBuffer.push(CMD_BEGIN_PATH, CMD_ARC, x, y, radius, 0, PI2, CMD_FILL_PATH);
    return this;
  };
  GR.renderWebGL = function (renderer, camera, parentMatrix) {
    var calc = renderer.calc(this.x, this.y, this._rotation, this._scaleX, this._scaleY, this, camera, parentMatrix);
    var cmds = this.commandBuffer, color = 0, alpha = 1, path = null;
    for (var i = 0; i < cmds.length;) {
      switch (cmds[i]) {
        case CMD_FILL_STYLE:
          color = cmds[i + 1];
          alpha = cmds[i + 2] * this._alpha;
          i += 3;
          break;
        case CMD_FILL_RECT:
          var x = cmds[i + 1], y = cmds[i + 2], w = cmds[i + 3], h = cmds[i + 4];
          renderer.batchTriangle(calc, x, y, x, y + h, x + w, y + h, color, alpha);
          renderer.batchTriangle(calc, x, y, x + w, y + h, x + w, y, color, alpha);
          i += 5;
          break;
        case CMD_BEGIN_PATH:
          path = [];
          i += 1;
          break;
        case CMD_ARC:
          // Phaser 3's arc: a point every 1% of the sweep.
          var cx = cmds[i + 1], cy = cmds[i + 2], radius = cmds[i + 3], start = cmds[i + 4], end = cmds[i + 5];
          for (var t = 0; t < 1; t += 0.01) {
            var a = start + (end - start) * t;
            path.push(cx + Math.cos(a) * radius, cy + Math.sin(a) * radius);
          }
          i += 6;
          break;
        case CMD_FILL_PATH:
          var tris = earcut(path);
          for (var k = 0; k < tris.length; k += 3) {
            var p0 = tris[k] * 2, p1 = tris[k + 1] * 2, p2 = tris[k + 2] * 2;
            renderer.batchTriangle(calc, path[p0], path[p0 + 1], path[p1], path[p1 + 1], path[p2], path[p2 + 1], color, alpha);
          }
          i += 1;
          break;
        default:
          throw new Error('unknown graphics command ' + cmds[i]);
      }
    }
  };

  // ---- the renderer: a vertex batch, no GPU ------------------------------------

  var MAX_TEXTURES = 16;
  var QUAD_CAPACITY = 4096;
  var QUAD_FLOATS = 4 * 6; // four vertices of x, y, u, v, texture unit, packed tint
  var TRI_CAPACITY = 8192;
  var TRI_FLOATS = 3 * 3; // three vertices of x, y, packed colour

  function Renderer() {
    var quads = new ArrayBuffer(QUAD_CAPACITY * QUAD_FLOATS * 4);
    this.f32 = new Float32Array(quads);
    this.u32 = new Uint32Array(quads);
    var tris = new ArrayBuffer(TRI_CAPACITY * TRI_FLOATS * 4);
    this.triF32 = new Float32Array(tris);
    this.triU32 = new Uint32Array(tris);
    this.quadCount = 0;
    this.triCount = 0;
    this.units = [];
    this.blendMode = BLEND_NORMAL;
    this.spriteMatrix = new TransformMatrix();
    this.camMatrix = new TransformMatrix();
    this.calcMatrix = new TransformMatrix();
    this.resetCounts();
    this.hash = 0;
  }
  var RP = Renderer.prototype;
  RP.resetCounts = function () {
    this.drawCalls = 0;
    this.quads = 0;
    this.triangles = 0;
    this.culled = 0;
  };
  // Where WebGL would upload the buffer and draw. The hash reads back what was written, so no
  // engine can treat the vertex writes as dead.
  RP.flush = function () {
    if (this.quadCount > 0) {
      var last = (this.quadCount - 1) * QUAD_FLOATS;
      this.hash = (this.hash * 31 + ((this.f32[0] + this.f32[last + 19]) | 0)) % 1000000007;
      this.drawCalls++;
      this.quadCount = 0;
    }
    if (this.triCount > 0) {
      var lastTri = (this.triCount - 1) * TRI_FLOATS;
      this.hash = (this.hash * 31 + ((this.triF32[0] + this.triF32[lastTri + 7]) | 0)) % 1000000007;
      this.drawCalls++;
      this.triCount = 0;
    }
    this.units.length = 0;
  };
  RP.setBlendMode = function (mode) {
    if (mode !== this.blendMode) {
      this.flush();
      this.blendMode = mode;
    }
  };
  RP.textureUnit = function (texture) {
    var units = this.units;
    for (var i = 0; i < units.length; i++) if (units[i] === texture) return i;
    if (units.length === MAX_TEXTURES) this.flush();
    units.push(texture);
    return units.length - 1;
  };
  RP.batchQuad = function (q, u0, v0, u1, v1, texture, tint, alpha, blendMode) {
    this.setBlendMode(blendMode);
    if (this.quadCount === QUAD_CAPACITY) this.flush();
    var unit = this.textureUnit(texture);
    var color = packColor(tint, alpha);
    var f = this.f32, u = this.u32, o = this.quadCount * QUAD_FLOATS;
    f[o] = q[0]; f[o + 1] = q[1]; f[o + 2] = u0; f[o + 3] = v0; f[o + 4] = unit; u[o + 5] = color;
    f[o + 6] = q[2]; f[o + 7] = q[3]; f[o + 8] = u0; f[o + 9] = v1; f[o + 10] = unit; u[o + 11] = color;
    f[o + 12] = q[4]; f[o + 13] = q[5]; f[o + 14] = u1; f[o + 15] = v1; f[o + 16] = unit; u[o + 17] = color;
    f[o + 18] = q[6]; f[o + 19] = q[7]; f[o + 20] = u1; f[o + 21] = v0; f[o + 22] = unit; u[o + 23] = color;
    this.quadCount++;
    this.quads++;
  };
  RP.batchTriangle = function (calc, x0, y0, x1, y1, x2, y2, tint, alpha) {
    this.setBlendMode(BLEND_NORMAL);
    if (this.triCount === TRI_CAPACITY) this.flush();
    var m = calc.matrix, a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], ff = m[5];
    var color = packColor(tint, alpha);
    var f = this.triF32, u = this.triU32, o = this.triCount * TRI_FLOATS;
    f[o] = x0 * a + y0 * c + e; f[o + 1] = x0 * b + y0 * d + ff; u[o + 2] = color;
    f[o + 3] = x1 * a + y1 * c + e; f[o + 4] = x1 * b + y1 * d + ff; u[o + 5] = color;
    f[o + 6] = x2 * a + y2 * c + e; f[o + 7] = x2 * b + y2 * d + ff; u[o + 8] = color;
    this.triCount++;
    this.triangles++;
  };
  // Phaser's GetCalcMatrix: the object's matrix, the camera's, and a parent's if it has one.
  RP.calc = function (x, y, rotation, scaleX, scaleY, object, camera, parentMatrix) {
    var sprite = this.spriteMatrix.applyITRS(x, y, rotation, scaleX, scaleY);
    var cam = this.camMatrix.copyFrom(camera.matrix);
    var scrollX = camera.scrollX * object.scrollFactorX, scrollY = camera.scrollY * object.scrollFactorY;
    if (parentMatrix) {
      cam.multiplyWithOffset(parentMatrix, -scrollX, -scrollY);
    } else {
      sprite.matrix[4] -= scrollX;
      sprite.matrix[5] -= scrollY;
    }
    return cam.multiply(sprite, this.calcMatrix);
  };
  RP.spriteCalc = function (sprite, camera, parentMatrix) {
    var sx = sprite.flipX ? -sprite._scaleX : sprite._scaleX;
    return this.calc(sprite.x, sprite.y, sprite._rotation, sx, sprite._scaleY, sprite, camera, parentMatrix);
  };
  RP.batchSprite = function (sprite, frame, camera, parentMatrix) {
    var calc = this.spriteCalc(sprite, camera, parentMatrix);
    var x = -sprite.displayOriginX, y = -sprite.displayOriginY;
    var q = calc.setQuad(x, y, x + frame.width, y + frame.height);
    if (offscreen(q, camera)) {
      this.culled++;
      return;
    }
    this.batchQuad(q, frame.u0, frame.v0, frame.u1, frame.v1, frame.texture, sprite.tint, sprite._alpha * camera.alpha,
      sprite.blendMode);
  };
  function offscreen(q, camera) {
    var w = camera.width, h = camera.height;
    return (q[0] < 0 && q[2] < 0 && q[4] < 0 && q[6] < 0) || (q[0] > w && q[2] > w && q[4] > w && q[6] > w) ||
      (q[1] < 0 && q[3] < 0 && q[5] < 0 && q[7] < 0) || (q[1] > h && q[3] > h && q[5] > h && q[7] > h);
  }

  // ---- the scene ---------------------------------------------------------------

  function Camera(width, height) {
    this.width = width;
    this.height = height;
    this.scrollX = 0;
    this.scrollY = 0;
    this.zoom = height / WORLD_HEIGHT;
    this.alpha = 1;
    this.matrix = new TransformMatrix();
    this.worldView = { x: 0, y: 0, width: 0, height: 0 };
  }
  // Origin (0, 0), as examples/phaser-stress sets it: the 720-unit world zoomed to the screen height.
  Camera.prototype.preRender = function () {
    this.matrix.applyITRS(0, 0, 0, this.zoom, this.zoom);
    var v = this.worldView;
    v.x = this.scrollX;
    v.y = this.scrollY;
    v.width = this.width / this.zoom;
    v.height = this.height / this.zoom;
  };
  Camera.prototype.worldWidth = function () {
    return this.width / this.zoom;
  };

  function DisplayList() {
    this.list = [];
    this.sortFlag = false;
  }
  DisplayList.prototype.add = function (object) {
    this.list.push(object);
    this.sortFlag = true;
    return object;
  };
  // ArrayUtils.Remove: indexOf and splice, which is what destroying an object costs Phaser.
  DisplayList.prototype.remove = function (object) {
    var i = this.list.indexOf(object);
    if (i !== -1) this.list.splice(i, 1);
  };
  DisplayList.prototype.queueDepthSort = function () {
    this.sortFlag = true;
  };
  DisplayList.prototype.depthSort = function () {
    if (!this.sortFlag) return;
    stableSort(this.list, sortByDepth);
    this.sortFlag = false;
  };
  function sortByDepth(a, b) {
    return a._depth - b._depth;
  }

  var RESERVED = { targets: 1, duration: 1, delay: 1, ease: 1, yoyo: 1, repeat: 1, onComplete: 1, callbackScope: 1 };
  var TWEEN_ACTIVE = 0, TWEEN_DONE = 1;
  function TweenData(target, key, end) {
    this.target = target;
    this.key = key;
    this.start = target[key];
    this.end = end;
  }
  function Tween(targets, props, config) {
    this.data = [];
    for (var t = 0; t < targets.length; t++) {
      for (var p = 0; p < props.length; p++) this.data.push(new TweenData(targets[t], props[p].key, props[p].value));
    }
    this.targets = targets;
    this.duration = config.duration || 1000;
    this.delay = config.delay || 0;
    this.ease = getEase(config.ease);
    this.yoyo = !!config.yoyo;
    this.repeat = config.repeat || 0;
    this.onComplete = config.onComplete || null;
    this.callbackScope = config.callbackScope || this;
    this.elapsed = 0;
    this.forward = true;
    this.state = TWEEN_ACTIVE;
  }
  // True when the tween has finished and can be dropped.
  Tween.prototype.update = function (delta) {
    if (this.state !== TWEEN_ACTIVE) return true;
    this.elapsed += delta;
    if (this.elapsed < this.delay) return false;
    var progress = (this.elapsed - this.delay) / this.duration;
    if (progress > 1) progress = 1;
    var v = this.ease(this.forward ? progress : 1 - progress);
    var data = this.data;
    // Properties by name, through whatever setter the target has: Phaser tweens `scale`, `x`...
    for (var i = 0; i < data.length; i++) {
      var d = data[i];
      d.target[d.key] = d.start + (d.end - d.start) * v;
    }
    if (progress < 1) return false;
    if (this.yoyo && this.forward) {
      this.forward = false;
      this.elapsed = this.delay;
    } else if (this.repeat !== 0) {
      if (this.repeat > 0) this.repeat--;
      this.forward = true;
      this.elapsed = this.delay;
    } else {
      this.state = TWEEN_DONE;
      if (this.onComplete) this.onComplete.call(this.callbackScope, this);
      return true;
    }
    return false;
  };
  function TweenManager() {
    this.tweens = [];
  }
  // Phaser's GetProps: every key of the config that is not a tween setting is a property to tween.
  TweenManager.prototype.add = function (config) {
    var targets = Array.isArray(config.targets) ? config.targets : [config.targets];
    var props = [];
    for (var key in config) {
      if (!RESERVED[key]) props.push({ key: key, value: config[key] });
    }
    var tween = new Tween(targets, props, config);
    this.tweens.push(tween);
    return tween;
  };
  TweenManager.prototype.update = function (delta) {
    var list = this.tweens, finished = 0;
    for (var i = 0; i < list.length; i++) if (list[i].update(delta)) finished++;
    if (finished) {
      var kept = [];
      for (i = 0; i < list.length; i++) if (list[i].state === TWEEN_ACTIVE) kept.push(list[i]);
      this.tweens = kept;
    }
  };
  TweenManager.prototype.killTweensOf = function (target) {
    var list = this.tweens;
    for (var i = 0; i < list.length; i++) {
      if (list[i].targets.indexOf(target) !== -1) list[i].state = TWEEN_DONE;
    }
  };

  // ---- arcade physics ----------------------------------------------------------

  function Body(world, gameObject, width, height) {
    this.id = nextId++;
    this.world = world;
    this.gameObject = gameObject;
    this.enable = true;
    this.width = width;
    this.height = height;
    this.halfWidth = width / 2;
    this.halfHeight = height / 2;
    this.position = new Vector2(gameObject.x - this.halfWidth, gameObject.y - this.halfHeight);
    this.prev = new Vector2(this.position.x, this.position.y);
    this.velocity = new Vector2();
    this.acceleration = new Vector2();
    this.gravity = new Vector2();
    this.bounce = new Vector2();
    this.drag = new Vector2();
    this.maxVelocity = new Vector2(10000, 10000);
    this.allowGravity = true;
    this.collideWorldBounds = false;
    this.blocked = { none: true, up: false, down: false, left: false, right: false };
    this.touching = { none: true, up: false, down: false, left: false, right: false };
  }
  Object.defineProperties(Body.prototype, {
    right: { get: function () { return this.position.x + this.width; } },
    bottom: { get: function () { return this.position.y + this.height; } },
  });
  function resetFlags(flags) {
    flags.none = true;
    flags.up = false;
    flags.down = false;
    flags.left = false;
    flags.right = false;
  }
  Body.prototype.update = function (delta) {
    var dt = delta / 1000, world = this.world;
    resetFlags(this.blocked);
    resetFlags(this.touching);
    var vx = this.velocity.x, vy = this.velocity.y;
    if (this.allowGravity) {
      vx += (world.gravity.x + this.gravity.x) * dt;
      vy += (world.gravity.y + this.gravity.y) * dt;
    }
    vx += this.acceleration.x * dt;
    vy += this.acceleration.y * dt;
    if (this.drag.x && !this.acceleration.x) {
      var dragX = this.drag.x * dt;
      if (vx - dragX > 0) vx -= dragX;
      else if (vx + dragX < 0) vx += dragX;
      else vx = 0;
    }
    vx = clamp(vx, -this.maxVelocity.x, this.maxVelocity.x);
    vy = clamp(vy, -this.maxVelocity.y, this.maxVelocity.y);
    this.velocity.set(vx, vy);
    this.prev.set(this.position.x, this.position.y);
    this.position.x += vx * dt;
    this.position.y += vy * dt;
    if (this.collideWorldBounds) {
      var b = world.bounds;
      if (this.position.x < b.x) { this.position.x = b.x; this.velocity.x *= -this.bounce.x; this.blocked.left = true; }
      else if (this.right > b.x + b.width) { this.position.x = b.x + b.width - this.width; this.velocity.x *= -this.bounce.x; this.blocked.right = true; }
      if (this.position.y < b.y) { this.position.y = b.y; this.velocity.y *= -this.bounce.y; this.blocked.up = true; }
      else if (this.bottom > b.y + b.height) { this.position.y = b.y + b.height - this.height; this.velocity.y *= -this.bounce.y; this.blocked.down = true; }
    }
  };
  Body.prototype.postUpdate = function () {
    this.gameObject.x = this.position.x + this.halfWidth;
    this.gameObject.y = this.position.y + this.halfHeight;
  };

  function World(width, height) {
    this.bodies = [];
    this.colliders = [];
    this.gravity = new Vector2(0, 0);
    this.bounds = { x: 0, y: 0, width: width, height: height };
    this.events = new EventEmitter();
    this.cellSize = 64;
    this.pairsTested = 0;
  }
  World.prototype.add = function (gameObject, width, height) {
    var body = new Body(this, gameObject, width, height);
    gameObject.body = body;
    this.bodies.push(body);
    return body;
  };
  World.prototype.remove = function (body) {
    var i = this.bodies.indexOf(body);
    if (i !== -1) this.bodies.splice(i, 1);
  };
  World.prototype.addTileCollider = function (layer) {
    this.colliders.push({ kind: 'tiles', layer: layer });
  };
  World.prototype.addGroupCollider = function () {
    this.colliders.push({ kind: 'group' });
  };
  World.prototype.step = function (delta) {
    var bodies = this.bodies, i;
    for (i = 0; i < bodies.length; i++) if (bodies[i].enable) bodies[i].update(delta);
    for (var c = 0; c < this.colliders.length; c++) {
      var collider = this.colliders[c];
      if (collider.kind === 'tiles') {
        for (i = 0; i < bodies.length; i++) this.collideTiles(bodies[i], collider.layer);
      } else {
        this.collideGroup(bodies);
      }
    }
    for (i = 0; i < bodies.length; i++) bodies[i].postUpdate();
  };
  // Phaser's collideSpriteVsTilemapLayer: the tiles under the body (a fresh array), then a
  // separation per tile along the side the body came in from.
  World.prototype.collideTiles = function (body, layer) {
    var tiles = layer.getTilesWithinWorldXY(body.position.x, body.position.y, body.width, body.height);
    for (var i = 0; i < tiles.length; i++) {
      var tile = tiles[i];
      var left = tile.pixelX, top = tile.pixelY, right = left + tile.width, bottom = top + tile.height;
      if (body.right <= left || body.position.x >= right || body.bottom <= top || body.position.y >= bottom) continue;
      var blocked = body.blocked;
      if (body.prev.y + body.height <= top + 0.01 && body.velocity.y >= 0 && tile.collideUp) {
        body.position.y = top - body.height;
        body.velocity.y = -body.velocity.y * body.bounce.y;
        blocked.down = true;
      } else if (body.prev.y >= bottom - 0.01 && body.velocity.y < 0 && tile.collideDown) {
        body.position.y = bottom;
        body.velocity.y = -body.velocity.y * body.bounce.y;
        blocked.up = true;
      } else if (body.prev.x + body.width <= left + 0.01 && tile.collideLeft) {
        body.position.x = left - body.width;
        body.velocity.x = -body.velocity.x * body.bounce.x;
        blocked.right = true;
      } else if (body.prev.x >= right - 0.01 && tile.collideRight) {
        body.position.x = right;
        body.velocity.x = -body.velocity.x * body.bounce.x;
        blocked.left = true;
      } else {
        continue;
      }
      blocked.none = false;
    }
  };
  // Body against body: a spatial hash of fresh cell arrays, each pair tested once.
  World.prototype.collideGroup = function (bodies) {
    var cells = new Map(), size = this.cellSize, world = this;
    for (var i = 0; i < bodies.length; i++) {
      var body = bodies[i];
      var x0 = Math.floor(body.position.x / size), x1 = Math.floor(body.right / size);
      var y0 = Math.floor(body.position.y / size), y1 = Math.floor(body.bottom / size);
      for (var cx = x0; cx <= x1; cx++) {
        for (var cy = y0; cy <= y1; cy++) {
          var key = (cx + 1024) * 4096 + (cy + 1024);
          var cell = cells.get(key);
          if (!cell) {
            cell = [];
            cells.set(key, cell);
          }
          cell.push(body);
        }
      }
    }
    var seen = new Set();
    cells.forEach(function (cell) {
      for (var a = 0; a < cell.length; a++) {
        for (var b = a + 1; b < cell.length; b++) {
          var p = cell[a], q = cell[b];
          var pair = p.id < q.id ? p.id * 1048576 + q.id : q.id * 1048576 + p.id;
          if (seen.has(pair)) continue;
          seen.add(pair);
          world.pairsTested++;
          if (separate(p, q)) world.events.emit('collide', p.gameObject, q.gameObject);
        }
      }
    });
  };
  function separate(a, b) {
    var overlapX = Math.min(a.right, b.right) - Math.max(a.position.x, b.position.x);
    var overlapY = Math.min(a.bottom, b.bottom) - Math.max(a.position.y, b.position.y);
    if (overlapX <= 0 || overlapY <= 0) return false;
    var av, bv, shift;
    if (overlapX < overlapY) {
      shift = (a.position.x < b.position.x ? overlapX : -overlapX) / 2;
      a.position.x -= shift;
      b.position.x += shift;
      av = a.velocity.x;
      bv = b.velocity.x;
      a.velocity.x = bv * a.bounce.x;
      b.velocity.x = av * b.bounce.x;
      a.touching[shift > 0 ? 'right' : 'left'] = true;
      b.touching[shift > 0 ? 'left' : 'right'] = true;
    } else {
      shift = (a.position.y < b.position.y ? overlapY : -overlapY) / 2;
      a.position.y -= shift;
      b.position.y += shift;
      av = a.velocity.y;
      bv = b.velocity.y;
      a.velocity.y = bv * a.bounce.y;
      b.velocity.y = av * b.bounce.y;
      a.touching[shift > 0 ? 'down' : 'up'] = true;
      b.touching[shift > 0 ? 'up' : 'down'] = true;
    }
    a.touching.none = false;
    b.touching.none = false;
    return true;
  }

  // ---- tilemap -----------------------------------------------------------------

  // examples/phaser-stress's level: 400 tiles wide, ground with gaps, brick platforms, stone pillars.
  function makeLevel() {
    var rand = new Random(7), columns = 400, rows = Math.ceil(WORLD_HEIGHT / TILE);
    var tiles = new Int8Array(columns * rows).fill(-1), ground = 18;
    for (var x = 0; x < columns; x++) {
      if (x > 10 && x < columns - 10 && rand.next() < 0.06) {
        x += 1 + Math.floor(rand.next() * 2);
        continue;
      }
      if (rand.next() < 0.12) ground = Math.min(20, Math.max(13, ground + (rand.next() < 0.5 ? -1 : 1)));
      tiles[ground * columns + x] = 0;
      for (var y = ground + 1; y < rows; y++) tiles[y * columns + x] = 1;
      if (x % 9 === 0 && rand.next() < 0.7) {
        var row = ground - 4 - Math.floor(rand.next() * 3), length = 3 + Math.floor(rand.next() * 4);
        for (var i = 0; i < length && x + i < columns; i++) tiles[row * columns + x + i] = 2;
      }
      if (x % 23 === 5) tiles[(ground - 1) * columns + x] = 3;
    }
    return { columns: columns, rows: rows, tiles: tiles, width: columns * TILE };
  }

  // Phaser's Tile: an object per cell, empty cells included, with collision faces and bags for
  // properties -- which is most of what a tilemap weighs.
  function Tile(layer, index, x, y) {
    this.layer = layer;
    this.index = index;
    this.x = x;
    this.y = y;
    this.width = TILE;
    this.height = TILE;
    this.pixelX = x * TILE;
    this.pixelY = y * TILE;
    this.alpha = 1;
    this.tint = 0xffffff;
    this.visible = true;
    this.flipX = false;
    this.flipY = false;
    this.rotation = 0;
    this.collideUp = false;
    this.collideDown = false;
    this.collideLeft = false;
    this.collideRight = false;
    this.faceTop = false;
    this.faceBottom = false;
    this.faceLeft = false;
    this.faceRight = false;
    this.properties = {};
    this.physics = {};
  }

  function TilemapLayer(scene, level, key) {
    GameObject.call(this, scene, 'TilemapLayer');
    this.texture = scene.textures.get(key);
    this.columns = level.columns;
    this.rows = level.rows;
    this.data = [];
    for (var y = 0; y < level.rows; y++) {
      var row = [];
      for (var x = 0; x < level.columns; x++) row.push(new Tile(this, level.tiles[y * level.columns + x], x, y));
      this.data.push(row);
    }
    this.calculateFaces();
    this.frames = [];
    for (var i = 0; i < 4; i++) this.frames.push(this.texture.get(String(i)));
    this.culledTiles = [];
  }
  TilemapLayer.prototype = Object.create(GameObject.prototype);
  TilemapLayer.prototype.constructor = TilemapLayer;
  TilemapLayer.prototype.tileAt = function (x, y) {
    if (x < 0 || y < 0 || x >= this.columns || y >= this.rows) return null;
    return this.data[y][x];
  };
  TilemapLayer.prototype.solidAt = function (px, py) {
    var tile = this.tileAt(Math.floor(px / TILE), Math.floor(py / TILE));
    return tile !== null && tile.index >= 0;
  };
  TilemapLayer.prototype.calculateFaces = function () {
    for (var y = 0; y < this.rows; y++) {
      for (var x = 0; x < this.columns; x++) {
        var tile = this.data[y][x];
        if (tile.index < 0) continue;
        var above = this.tileAt(x, y - 1), below = this.tileAt(x, y + 1);
        var left = this.tileAt(x - 1, y), right = this.tileAt(x + 1, y);
        tile.faceTop = tile.collideUp = !above || above.index < 0;
        tile.faceBottom = tile.collideDown = !below || below.index < 0;
        tile.faceLeft = tile.collideLeft = !left || left.index < 0;
        tile.faceRight = tile.collideRight = !right || right.index < 0;
      }
    }
  };
  TilemapLayer.prototype.getTilesWithinWorldXY = function (worldX, worldY, width, height) {
    var out = [];
    var x0 = Math.max(0, Math.floor(worldX / TILE)), x1 = Math.min(this.columns - 1, Math.floor((worldX + width) / TILE));
    var y0 = Math.max(0, Math.floor(worldY / TILE)), y1 = Math.min(this.rows - 1, Math.floor((worldY + height) / TILE));
    for (var y = y0; y <= y1; y++) {
      var row = this.data[y];
      for (var x = x0; x <= x1; x++) if (row[x].index >= 0) out.push(row[x]);
    }
    return out;
  };
  // Phaser's CullTiles: the tiles in the camera's view, one tile of padding, into a reused list.
  TilemapLayer.prototype.cull = function (camera) {
    var out = this.culledTiles, view = camera.worldView;
    out.length = 0;
    var x0 = Math.max(0, Math.floor(view.x / TILE) - 1), x1 = Math.min(this.columns, Math.ceil((view.x + view.width) / TILE) + 1);
    var y0 = Math.max(0, Math.floor(view.y / TILE) - 1), y1 = Math.min(this.rows, Math.ceil((view.y + view.height) / TILE) + 1);
    for (var y = y0; y < y1; y++) {
      var row = this.data[y];
      for (var x = x0; x < x1; x++) {
        var tile = row[x];
        if (tile.index !== -1 && tile.visible && tile.alpha !== 0) out.push(tile);
      }
    }
    return out;
  };
  TilemapLayer.prototype.renderWebGL = function (renderer, camera) {
    var tiles = this.cull(camera);
    var calc = renderer.calc(this.x, this.y, 0, 1, 1, this, camera, null);
    var alpha = this._alpha * camera.alpha;
    for (var i = 0; i < tiles.length; i++) {
      var tile = tiles[i], frame = this.frames[tile.index];
      var q = calc.setQuad(tile.pixelX, tile.pixelY, tile.pixelX + tile.width, tile.pixelY + tile.height);
      renderer.batchQuad(q, frame.u0, frame.v0, frame.u1, frame.v1, this.texture, tile.tint, tile.alpha * alpha, BLEND_NORMAL);
    }
  };

  // ---- particles ---------------------------------------------------------------

  var PARTICLE_LIFE = 1000;
  function Particle(emitter) {
    this.emitter = emitter;
    this.frame = emitter.frame;
    this.x = 0;
    this.y = 0;
    this.velocityX = 0;
    this.velocityY = 0;
    this.rotation = 0;
    this.spin = 0;
    this.scaleX = 1;
    this.scaleY = 1;
    this.alpha = 1;
    this.tint = 0xffffff;
    this.life = PARTICLE_LIFE;
    this.lifeCurrent = PARTICLE_LIFE;
    this.lifeT = 0;
  }
  Particle.prototype.fire = function (x, y) {
    var e = this.emitter, rand = e.rand;
    var angle = rand.between(0, 360) * (Math.PI / 180), speed = rand.between(e.speedMin, e.speedMax);
    this.x = x;
    this.y = y;
    this.velocityX = Math.cos(angle) * speed;
    this.velocityY = Math.sin(angle) * speed;
    this.spin = rand.between(-4, 4);
    this.life = this.lifeCurrent = PARTICLE_LIFE;
    this.lifeT = 0;
  };
  // Phaser's EmitterOps: each property eased from its start to its end over the particle's life.
  Particle.prototype.update = function (delta, step) {
    var e = this.emitter;
    this.lifeCurrent -= delta;
    if (this.lifeCurrent <= 0) return true;
    var t = (this.lifeT = 1 - this.lifeCurrent / this.life);
    this.velocityY += e.gravityY * step;
    this.x += this.velocityX * step;
    this.y += this.velocityY * step;
    this.rotation += this.spin * step;
    this.scaleX = this.scaleY = e.scaleStart + (e.scaleEnd - e.scaleStart) * e.scaleEase(t);
    this.alpha = e.alphaStart + (e.alphaEnd - e.alphaStart) * e.alphaEase(t);
    this.tint = lerpColor(e.tintStart, e.tintEnd, t);
    return false;
  };

  function ParticleEmitter(scene, frame, seed) {
    GameObject.call(this, scene, 'ParticleEmitter');
    this.texture = scene.textures.get('atlas');
    this.frame = this.texture.get(frame);
    this.rand = new Random(seed);
    this.alive = [];
    this.dead = [];
    this.speedMin = 60;
    this.speedMax = 240;
    this.gravityY = 400;
    this.scaleStart = 1.5;
    this.scaleEnd = 0.5;
    this.scaleEase = getEase('Linear');
    this.alphaStart = 1;
    this.alphaEnd = 0;
    this.alphaEase = getEase('Quad.easeOut');
    this.tintStart = 0xffffff;
    this.tintEnd = 0xff6600;
    this.blendMode = BLEND_ADD;
    this.particleMatrix = new TransformMatrix();
  }
  ParticleEmitter.prototype = Object.create(GameObject.prototype);
  ParticleEmitter.prototype.constructor = ParticleEmitter;
  ParticleEmitter.prototype.explode = function (count, x, y) {
    for (var i = 0; i < count; i++) {
      var p = this.dead.length ? this.dead.pop() : new Particle(this);
      p.fire(x, y);
      this.alive.push(p);
    }
  };
  // Phaser 3's emitter update: a record per particle that died, then a splice for each.
  ParticleEmitter.prototype.preUpdate = function (time, delta) {
    var step = delta / 1000, alive = this.alive, rip = [];
    for (var i = 0; i < alive.length; i++) {
      if (alive[i].update(delta, step)) rip.push({ index: i, particle: alive[i] });
    }
    for (var r = rip.length - 1; r >= 0; r--) {
      this.dead.push(rip[r].particle);
      alive.splice(rip[r].index, 1);
    }
  };
  ParticleEmitter.prototype.renderWebGL = function (renderer, camera) {
    var alive = this.alive, frame = this.frame, hw = frame.halfWidth, hh = frame.halfHeight;
    var cam = renderer.camMatrix.copyFrom(camera.matrix), pm = this.particleMatrix, calc = renderer.calcMatrix;
    var scrollX = camera.scrollX * this.scrollFactorX, scrollY = camera.scrollY * this.scrollFactorY;
    for (var i = 0; i < alive.length; i++) {
      var p = alive[i];
      if (p.alpha <= 0) continue;
      pm.applyITRS(p.x - scrollX, p.y - scrollY, p.rotation, p.scaleX, p.scaleY);
      cam.multiply(pm, calc);
      var q = calc.setQuad(-hw, -hh, hw, hh);
      if (offscreen(q, camera)) {
        renderer.culled++;
        continue;
      }
      renderer.batchQuad(q, frame.u0, frame.v0, frame.u1, frame.v1, this.texture, p.tint, p.alpha * camera.alpha, this.blendMode);
    }
  };
  ParticleEmitter.prototype.count = function () {
    return this.alive.length;
  };

  // ---- scene -------------------------------------------------------------------

  var RENDERER = new Renderer();

  function Scene(name) {
    this.name = name;
    this.events = new EventEmitter();
    this.textures = TEXTURES;
    this.displayList = new DisplayList();
    this.updateList = [];
    this.tweens = new TweenManager();
    this.physics = null;
    this.camera = new Camera(view.width, view.height);
    this.renderer = RENDERER;
    this.time = 0;
    this.frame = 0;
    this.totals = { quads: 0, triangles: 0, drawCalls: 0, culled: 0 };
  }
  var SC = Scene.prototype;
  SC.image = function (x, y, key, frame) {
    return this.displayList.add(new Sprite(this, x, y, key, frame));
  };
  SC.tileSprite = function (x, y, width, height, key) {
    return this.displayList.add(new TileSprite(this, x, y, width, height, key));
  };
  SC.container = function (x, y) {
    return this.displayList.add(new Container(this, x, y));
  };
  SC.bitmapText = function (x, y, text, size) {
    return this.displayList.add(new BitmapText(this, x, y, text, size));
  };
  SC.graphics = function () {
    return this.displayList.add(new Graphics(this));
  };
  SC.particles = function (frame, seed) {
    var emitter = this.displayList.add(new ParticleEmitter(this, frame, seed));
    this.updateList.push(emitter);
    return emitter;
  };
  SC.enablePhysics = function (width, height) {
    this.physics = new World(width, height);
    return this.physics;
  };
  // One game step, in Phaser's order: preupdate, the update list, update, tweens and physics,
  // postupdate, then the render walk over the depth-sorted display list.
  SC.step = function () {
    var delta = FRAME_MS, events = this.events;
    this.time += delta;
    this.frame++;
    events.emit('preupdate', this.time, delta);
    var list = this.updateList;
    for (var i = 0; i < list.length; i++) {
      var object = list[i];
      if (object.active) object.preUpdate(this.time, delta);
    }
    events.emit('update', this.time, delta);
    this.tweens.update(delta);
    if (this.physics) this.physics.step(delta);
    events.emit('postupdate', this.time, delta);
    this.render();
  };
  SC.render = function () {
    var renderer = this.renderer, camera = this.camera;
    this.displayList.depthSort();
    camera.preRender();
    renderer.resetCounts();
    var objects = this.displayList.list;
    for (var i = 0; i < objects.length; i++) {
      var object = objects[i];
      if (object.willRender(camera)) object.renderWebGL(renderer, camera, null);
    }
    renderer.flush();
    var totals = this.totals;
    totals.quads += renderer.quads;
    totals.triangles += renderer.triangles;
    totals.drawCalls += renderer.drawCalls;
    totals.culled += renderer.culled;
  };
  SC.shutdown = function () {
    var objects = this.displayList.list.slice();
    for (var i = objects.length - 1; i >= 0; i--) objects[i].destroy();
    this.updateList.length = 0;
    this.tweens.tweens.length = 0;
    this.events.removeAllListeners();
    this.physics = null;
  };

  // ---- the scenes, as examples/phaser-stress plays them --------------------------------

  function stepBunny(b, width, dt) {
    b.x += b.vx * dt;
    b.y += b.vy * dt;
    b.vy += 0.5 * dt;
    if (b.x < 0) { b.x = 0; b.vx = -b.vx; } else if (b.x > width) { b.x = width; b.vx = -b.vx; }
    if (b.y > WORLD_HEIGHT) {
      b.y = WORLD_HEIGHT;
      b.vy *= -0.85;
      if (b.x % 2 < 1) b.vy -= 3 + (b.x % 3);
    } else if (b.y < 0) {
      b.y = 0;
      b.vy = 0;
    }
    b.rotation += b.spin * dt;
  }

  function sumPositions(objects) {
    var sum = 0;
    for (var i = 0; i < objects.length; i++) sum += objects[i].x + objects[i].y;
    return sum;
  }

  var SCENES = {
    // Bunnymark: bouncing, spinning, tinted sprites.
    sprites: {
      unit: 'sprites',
      loads: [100, 400, 1600],
      create: function (scene, load) {
        var rand = new Random(1), width = scene.camera.worldWidth(), bunnies = [], images = [];
        for (var i = 0; i < load; i++) {
          var b = {
            x: rand.next() * width, y: rand.next() * 360, vx: rand.next() * 10 - 5, vy: rand.next() * 10 - 5,
            rotation: 0, spin: (rand.next() - 0.5) * 0.2, scale: 0.5 + rand.next() * 0.6, tint: Math.floor(rand.next() * 0xffffff),
          };
          bunnies.push(b);
          images.push(scene.image(b.x, b.y, 'atlas', 'star').setScale(b.scale).setTint(b.tint));
        }
        scene.events.on('update', function (time, delta) {
          var dt = delta / FRAME_MS;
          for (var i = 0; i < bunnies.length; i++) {
            var b = bunnies[i], image = images[i];
            stepBunny(b, width, dt);
            image.x = b.x;
            image.y = b.y;
            image.rotation = b.rotation;
          }
        });
        return { checksum: function () { return sumPositions(images); } };
      },
    },

    // Explosion bursts that fall and fade, additive: pooled particles eased over their life.
    particles: {
      unit: 'particles',
      loads: [250, 1000, 4000],
      create: function (scene, load) {
        var BURST = 40, rand = new Random(13), width = scene.camera.worldWidth(), due = 0, bursts = 0;
        var emitters = [scene.particles('spark', 1), scene.particles('spark', 2), scene.particles('star', 3)];
        scene.events.on('update', function (time, delta) {
          due += (load / BURST / (PARTICLE_LIFE / FRAME_MS)) * (delta / FRAME_MS);
          for (; due >= 1; due--) {
            emitters[bursts++ % emitters.length].explode(BURST, 100 + rand.next() * (width - 200), 100 + rand.next() * 400);
          }
        });
        return {
          checksum: function () {
            var sum = 0;
            for (var i = 0; i < emitters.length; i++) sum += emitters[i].count() + sumPositions(emitters[i].alive);
            return sum;
          },
        };
      },
    },

    // A scrolling tilemap, two parallax layers, a running player, and enemies with arcade
    // bodies that fall, walk, turn at walls and ledges, and animate by frame name.
    platformer: {
      unit: 'enemies',
      loads: [10, 50, 200],
      create: function (scene, load) {
        var level = makeLevel(), rand = new Random(9), camera = scene.camera;
        // Walls at the level's ends only: the bounds reach far above and below, so a gap is
        // still a fall.
        var world = scene.enablePhysics(level.width, 200000);
        world.bounds.y = -100000;
        world.gravity.y = 1800;
        var far = scene.tileSprite(0, WORLD_HEIGHT - 316, camera.worldWidth(), 256, 'hills-far').setScrollFactor(0).setDepth(-2);
        var near = scene.tileSprite(0, WORLD_HEIGHT - 256, camera.worldWidth(), 256, 'hills-near').setScrollFactor(0).setDepth(-1);
        var layer = scene.displayList.add(new TilemapLayer(scene, level, 'tiles'));
        world.addTileCollider(layer);
        var player = scene.image(0, 0, 'atlas', 'player-0').setOrigin(0.5, 1).setDepth(1).play('player-run');
        var enemies = [];
        for (var i = 0; i < load; i++) {
          var enemy = scene.image(16 + rand.next() * (level.width - 32), rand.next() * 200, 'atlas', 'enemy-0').play('enemy-walk');
          enemy.anims.timeScale = 0.8 + rand.next() * 0.4;
          var body = world.add(enemy, 24, 20);
          enemy.speed = 48 + rand.next() * 84;
          body.velocity.x = (rand.next() < 0.5 ? -1 : 1) * enemy.speed;
          body.maxVelocity.y = 600;
          body.collideWorldBounds = true;
          enemies.push(enemy);
        }
        scene.events.on('update', function (time, delta) {
          var dt = delta / FRAME_MS;
          camera.scrollX += 3 * dt;
          if (camera.scrollX > level.width - camera.worldWidth()) camera.scrollX = 0;
          far.tilePositionX = camera.scrollX * 0.2;
          near.tilePositionX = camera.scrollX * 0.5;
          var px = camera.scrollX + 240, py = WORLD_HEIGHT + 40;
          for (var row = 0; row < level.rows; row++) {
            if (layer.solidAt(px, row * TILE)) {
              py = row * TILE;
              break;
            }
          }
          player.setPosition(px, py);
        });
        // Patrol after the physics step: turn at walls and at the edge of a ledge, and drop
        // back in from the top after falling through a gap.
        scene.events.on('postupdate', function () {
          for (var i = 0; i < enemies.length; i++) {
            var enemy = enemies[i], body = enemy.body, dir = body.velocity.x < 0 ? -1 : 1;
            if (body.blocked.left) {
              body.velocity.x = enemy.speed;
            } else if (body.blocked.right) {
              body.velocity.x = -enemy.speed;
            } else if (body.blocked.down && !layer.solidAt(enemy.x + dir * (body.halfWidth + 2), body.bottom + 4)) {
              body.velocity.x = -body.velocity.x;
            }
            if (body.position.y > WORLD_HEIGHT + 32) {
              body.position.y = -32;
              body.velocity.y = 0;
            }
            enemy.flipX = body.velocity.x < 0;
          }
        });
        return { checksum: function () { return sumPositions(enemies) + player.y; } };
      },
    },

    // Five turrets spraying bullets that are allocated on firing and destroyed on leaving
    // the screen, as a game without a pool does; hits on the ship go through an event.
    shooter: {
      unit: 'bullets',
      loads: [100, 400, 1600],
      create: function (scene, load) {
        var width = scene.camera.worldWidth(), bullets = [], shots = 0, hits = 0, t = 0;
        var ship = scene.image(width / 2, WORLD_HEIGHT - 80, 'atlas', 'player-0').setScale(1.5);
        var turrets = [];
        for (var i = 0; i < 5; i++) turrets.push(scene.image((width * (i + 1)) / 6, 110, 'atlas', 'enemy-0').setScale(1.5));
        scene.events.on('hit', function (bullet) {
          hits++;
          ship.setTint(hits % 2 ? 0xff8080 : 0xffffff);
        });
        scene.events.on('update', function (time, delta) {
          var dt = delta / FRAME_MS;
          t += dt;
          ship.x = width / 2 + Math.sin(t * 0.03) * width * 0.35;
          for (var i = 0; i < turrets.length; i++) {
            var turret = turrets[i];
            turret.x = (width * (i + 1)) / 6 + Math.sin(t * 0.02 + i) * 60;
            turret.rotation = Math.atan2(ship.y - turret.y, ship.x - turret.x) - Math.PI / 2;
          }
          var spawn = Math.min(load - bullets.length, Math.max(4, Math.ceil(load / 60)));
          for (var s = 0; s < spawn; s++) {
            var from = turrets[shots % turrets.length];
            var angle = shots * 0.4 + t * 0.05, speed = 2.5 + (shots % 7) * 0.35;
            shots++;
            var bullet = scene.image(from.x, from.y, 'atlas', 'bullet').setBlendMode(BLEND_ADD);
            bullet.vx = Math.cos(angle) * speed;
            bullet.vy = Math.sin(angle) * speed;
            bullets.push(bullet);
          }
          bullets = bullets.filter(function (b) {
            b.x += b.vx * dt;
            b.y += b.vy * dt;
            var gone = b.x < -16 || b.x > width + 16 || b.y < -16 || b.y > WORLD_HEIGHT + 16;
            if (!gone && Math.abs(b.x - ship.x) < 14 && Math.abs(b.y - ship.y) < 14) {
              scene.events.emit('hit', b);
              gone = true;
            }
            if (gone) b.destroy();
            return !gone;
          });
        });
        return { checksum: function () { return sumPositions(bullets) + hits + shots; } };
      },
    },

    // A match-3 board: a bobbing tween per gem, neighbours swapped with tweens that come and
    // go, raised above the board while they move, and the board scanned for runs of three.
    puzzle: {
      unit: 'gems',
      loads: [64, 256, 1024],
      create: function (scene, load) {
        var rand = new Random(5), side = Math.max(1, Math.round(Math.sqrt(load)));
        var cell = Math.min(64, (WORLD_HEIGHT - 100) / side), left = (scene.camera.worldWidth() - side * cell) / 2;
        var base = cell / 44, gems = [], clock = 0, matches = 0;
        for (var i = 0; i < side * side; i++) {
          var color = Math.floor(rand.next() * 6);
          var gem = scene.image(left + (i % side) * cell + cell / 2, 90 + Math.floor(i / side) * cell + cell / 2, 'atlas', 'gem-' + color);
          gem.setScale(base);
          gem.color = color;
          scene.tweens.add({ targets: gem, scale: base * 1.08, duration: 400 + (i % 5) * 40, yoyo: true, repeat: -1, ease: 'Sine.easeInOut' });
          gems.push(gem);
        }
        function landed() {
          for (var i = 0; i < this.targets.length; i++) this.targets[i].setDepth(0);
        }
        scene.events.on('update', function (time, delta) {
          clock += delta / FRAME_MS;
          if (clock < 8) return;
          clock = 0;
          var swaps = Math.max(1, Math.floor(gems.length / 12));
          for (var s = 0; s < swaps; s++) {
            var a = Math.floor(rand.next() * gems.length), b = (a % side) + 1 < side ? a + 1 : a - 1;
            var ga = gems[a], gb = gems[b];
            if (!gb || ga.depth > 0 || gb.depth > 0) continue;
            gems[a] = gb;
            gems[b] = ga;
            ga.setDepth(1);
            gb.setDepth(1);
            scene.tweens.add({ targets: ga, x: gb.x, y: gb.y, duration: 200, ease: 'Back.easeOut', onComplete: landed });
            scene.tweens.add({ targets: gb, x: ga.x, y: ga.y, duration: 200, ease: 'Back.easeOut', onComplete: landed });
          }
          matches += findMatches(gems, side).length;
        });
        return { checksum: function () { return sumPositions(gems) + matches; } };
      },
    },

    // Bodies bouncing inside the screen and off each other: the body-against-body broadphase.
    swarm: {
      unit: 'bodies',
      loads: [50, 200, 500],
      create: function (scene, load) {
        var rand = new Random(17), width = scene.camera.worldWidth();
        var world = scene.enablePhysics(width, WORLD_HEIGHT), bodies = [], collisions = 0;
        world.addGroupCollider();
        for (var i = 0; i < load; i++) {
          var sprite = scene.image(rand.between(20, width - 20), rand.between(20, WORLD_HEIGHT - 20), 'atlas', 'enemy-' + (i % 2));
          var body = world.add(sprite, 20, 16);
          body.velocity.set(rand.between(-120, 120), rand.between(-120, 120));
          body.bounce.set(1, 1);
          body.allowGravity = false;
          body.collideWorldBounds = true;
          bodies.push(sprite);
        }
        world.events.on('collide', function (a, b) {
          collisions++;
          a.setTint(0xffd166);
          b.setTint(0xffd166);
        });
        scene.events.on('preupdate', function () {
          for (var i = 0; i < bodies.length; i++) bodies[i].setTint(0xffffff);
        });
        return { checksum: function () { return sumPositions(bodies) + collisions; } };
      },
    },

    // Units with health bars and markers, redrawn with Graphics every frame: rectangles and
    // circles turned into triangles in JavaScript.
    vector: {
      unit: 'shapes',
      loads: [3, 12, 48],
      create: function (scene, load) {
        var rand = new Random(3), width = scene.camera.worldWidth(), shapes = [];
        for (var i = 0; i < load; i++) {
          shapes.push({
            x: rand.next() * width, y: 60 + rand.next() * (WORLD_HEIGHT - 80), vx: rand.next() * 4 - 2,
            vy: rand.next() * 4 - 2, hp: rand.next(), color: Math.floor(rand.next() * 0xffffff) | 0x404040,
          });
        }
        var graphics = scene.graphics();
        scene.events.on('update', function (time, delta) {
          var dt = delta / FRAME_MS;
          graphics.clear();
          for (var i = 0; i < shapes.length; i++) {
            var s = shapes[i];
            s.x += s.vx * dt;
            s.y += s.vy * dt;
            if (s.x < 10 || s.x > width - 10) s.vx = -s.vx;
            if (s.y < 60 || s.y > WORLD_HEIGHT - 10) s.vy = -s.vy;
            s.hp = (s.hp + 0.004 * dt) % 1;
            graphics.fillStyle(0x222222).fillRect(s.x - 12, s.y - 16, 24, 4);
            graphics.fillStyle(0x06d6a0).fillRect(s.x - 12, s.y - 16, 24 * s.hp, 4);
            graphics.fillStyle(s.color).fillCircle(s.x, s.y, 7);
          }
        });
        return { checksum: function () { return sumPositions(shapes); } };
      },
    },

    // A HUD: panels in nested containers, each with bitmap text rewritten every frame and
    // spinning icons, a pulsing alpha through the containers, and the state saved as JSON.
    ui: {
      unit: 'panels',
      loads: [5, 20, 80],
      create: function (scene, load) {
        var rand = new Random(29), root = scene.container(20, 20), panels = [], group = null, saved = '', t = 0;
        for (var i = 0; i < load; i++) {
          if (i % 4 === 0) {
            group = new Container(scene, (i % 16) * 60, Math.floor(i / 16) * 120);
            root.add(group);
          }
          var panel = new Container(scene, (i % 4) * 150, 0);
          group.add(panel);
          var state = { name: 'P' + i, score: Math.floor(rand.next() * 1000), hp: rand.next(), combo: 0, icons: [] };
          panel.add(new Sprite(scene, 0, 0, 'atlas', 'icon-' + (i % 8)));
          var text = new BitmapText(scene, 16, -8, '', 14);
          panel.add(text);
          for (var k = 0; k < 3; k++) {
            var icon = new Sprite(scene, 20 + k * 26, 24, 'atlas', 'icon-' + ((i + k) % 8));
            panel.add(icon);
            state.icons.push(icon);
          }
          panels.push({ container: panel, text: text, state: state });
        }
        var hud = scene.bitmapText(8, WORLD_HEIGHT - 30, '', 18);
        scene.events.on('update', function (time, delta) {
          var dt = delta / FRAME_MS;
          t += dt;
          for (var i = 0; i < panels.length; i++) {
            var p = panels[i], s = p.state;
            s.score += 1 + (i % 3);
            s.hp = (s.hp + 0.003 * dt) % 1;
            s.combo = (s.combo + 1) % 12;
            p.text.setText(s.name + ' score ' + s.score + ' hp ' + (s.hp * 100).toFixed(1) + '% x' + s.combo);
            for (var k = 0; k < s.icons.length; k++) s.icons[k].rotation += 0.05 * (k + 1) * dt;
            p.container.alpha = 0.75 + 0.25 * Math.sin(t * 0.05 + i);
          }
          hud.setText('frame ' + scene.frame + ' | panels ' + panels.length + ' | t ' + t.toFixed(2));
          if (scene.frame % 60 === 0) {
            saved = JSON.stringify({ frame: scene.frame, panels: panels.map(function (p) { return { name: p.state.name, score: p.state.score, hp: p.state.hp }; }) });
            JSON.parse(saved);
          }
        });
        return {
          checksum: function () {
            var sum = saved.length;
            for (var i = 0; i < panels.length; i++) sum += panels[i].state.score + panels[i].text.width;
            return sum;
          },
        };
      },
    },
  };
  var ORDER = ['sprites', 'particles', 'platformer', 'shooter', 'puzzle', 'swarm', 'vector', 'ui'];

  // Runs of three or more of a colour, across and down: a match object per run.
  function findMatches(gems, side) {
    var found = [];
    for (var y = 0; y < side; y++) {
      for (var x = 0; x < side; x++) {
        var color = gems[y * side + x].color, run;
        if (x === 0 || gems[y * side + x - 1].color !== color) {
          for (run = 1; x + run < side && gems[y * side + x + run].color === color; run++);
          if (run >= 3) found.push({ x: x, y: y, length: run, across: true, color: color });
        }
        if (y === 0 || gems[(y - 1) * side + x].color !== color) {
          for (run = 1; y + run < side && gems[(y + run) * side + x].color === color; run++);
          if (run >= 3) found.push({ x: x, y: y, length: run, across: false, color: color });
        }
      }
    }
    return found;
  }

  // ---- the host's interface ------------------------------------------------------

  var current = null;

  function round(value) {
    return Math.round(value * 1000) / 1000;
  }

  global.bench = {
    info: function () {
      return JSON.stringify({
        workload: 'phaser-shaped',
        version: VERSION,
        view: view,
        scenes: ORDER.map(function (name) {
          return { name: name, unit: SCENES[name].unit, loads: SCENES[name].loads };
        }),
      });
    },
    // options: { scenes: "a,b" | "", loads: { scene: "1,2,3" }, scale: number }
    plan: function (json) {
      var options = JSON.parse(json || '{}'), names = options.scenes ? options.scenes.split(',') : ORDER, lines = [];
      for (var i = 0; i < names.length; i++) {
        var name = names[i], scene = SCENES[name];
        if (!scene) throw new Error('unknown scene "' + name + '" (' + ORDER.join(', ') + ')');
        var loads = options.loads && options.loads[name] ? options.loads[name].split(',').map(Number) : scene.loads;
        for (var k = 0; k < loads.length; k++) lines.push(name + ' ' + Math.max(1, Math.round(loads[k] * (options.scale || 1))));
      }
      return lines.join('\n');
    },
    setView: function (width, height) {
      view.width = width;
      view.height = height;
    },
    enter: function (name, load) {
      if (current) throw new Error('bench.enter: "' + current.name + '" is still running');
      var scene = new Scene(name);
      RENDERER.hash = 0;
      current = { name: name, load: load, scene: scene, logic: SCENES[name].create(scene, load), frames: 0 };
    },
    frame: function () {
      current.scene.step();
      current.frames++;
    },
    exit: function () {
      var c = current, totals = c.scene.totals, frames = Math.max(1, c.frames);
      var result = {
        checksum: round(c.logic.checksum()),
        objects: c.scene.displayList.list.length,
        quadsPerFrame: round(totals.quads / frames),
        trianglesPerFrame: round(totals.triangles / frames),
        drawCallsPerFrame: round(totals.drawCalls / frames),
        culledPerFrame: round(totals.culled / frames),
        vertexHash: RENDERER.hash,
      };
      c.scene.shutdown();
      current = null;
      return JSON.stringify(result);
    },
  };
})(this);
