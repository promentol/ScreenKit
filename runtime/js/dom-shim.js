// Copyright (c) ScreenKit contributors. MIT.
//
// The DOM shim prelude: `document` and a real element tree, a GL context per
// canvas, and everything else a web build reaches for.
//
// This is JavaScript and not C++ on purpose. `Architecture.md` §2 puts the DOM
// layer in JS and keeps C++ for what needs native access; nothing here needs
// native access, because the graphics bootstrap has already installed the
// vendored `WebGLRenderingContext` as the `gl` global by the time this runs.
// Writing it in C++ would mean host functions for property getters that a JS
// object does for free.
//
// **Scope is measured, not specified.** What is here is chosen by the evidence --
// `_bmad-output/implementation-artifacts/m2-lightning-dom-trace.json` (runtime
// trace), `m2-lightning-dom-usage.json` (static analysis), or a spec's matrix
// row. Once the evidence names an interface (Node, Element, EventTarget,
// CSSStyleDeclaration, DOMTokenList...), that interface is implemented as a
// whole, to the DOM's semantics, rather than member by member; an interface
// nothing names is not here. `runtime/js/README.md` carries the rule and the list.
//
// Evaluation order is load-bearing: the host runs this after `startGraphics`
// and before the app bundle. Evaluating it earlier would leave `getContext`
// with nothing to hand out.

(function (global) {
  'use strict';

  var SHIM = 'screenkit-dom-shim/1';

  // -------------------------------------------------------------------------
  // The drawable
  //
  // The page's frame is one GL surface and it fills the display. Its size is
  // read from `gl.drawingBufferWidth`/`Height` on every access rather than
  // cached here, which is what makes `canvas.width` and `gl.drawingBufferWidth`
  // unable to disagree on the canvas that *is* the frame: they are literally
  // the same number.
  //
  // Every *other* canvas that takes a WebGL context gets a real GL context of
  // its own and a compositor layer at its CSS rect (`__screenkit.canvas`,
  // `Architecture.md` §3.1), and there `canvas.width` is that layer's own
  // drawing buffer. `drawableWidth`/`drawableHeight` stay the window's, because
  // that is what percentages, viewport units, `<body>` and `window.screen` mean.
  // -------------------------------------------------------------------------

  function glContext() {
    var value = global.gl;
    return value !== null && typeof value === 'object' ? value : null;
  }

  function drawableWidth() {
    var context = glContext();
    var size = context === null ? 0 : context.drawingBufferWidth;
    return typeof size === 'number' ? size : 0;
  }

  function drawableHeight() {
    var context = glContext();
    var size = context === null ? 0 : context.drawingBufferHeight;
    return typeof size === 'number' ? size : 0;
  }

  // A gap that returns null or ignores a write is invisible unless it says so
  // once. Once, not every time: Lightning reads `canvas.width` eleven times a
  // frame, and a log per read would be its own bug.
  var announced = Object.create(null);

  function announce(key, message) {
    if (announced[key] === true) return;
    announced[key] = true;
    global.console.warn(message);
  }

  // -------------------------------------------------------------------------
  // Backed elements
  //
  // The three that ever paint (`Architecture.md` §3.1): `<canvas>`, `<video>`
  // and `<iframe>`. Being backed is what makes an element's style parse the CSS
  // subset, which is the same subset that places every one of them.
  //
  // A canvas's *size* still has two answers, and which one it gets is decided
  // when it asks for its context:
  //
  //  - the canvas that is the page's frame reads the drawable, and a write to
  //    it is accepted and ignored (said once), because Lightning writes a size
  //    and then trusts `gl.drawingBufferWidth`;
  //  - a canvas with a layer of its own reads *its* drawing buffer, and a write
  //    resizes it, as a browser's canvas does;
  //  - a canvas with no GL context at all -- a 2D scratchpad -- keeps whatever
  //    was written to it, and reads the drawable until something is.
  //
  // Evidence: HTMLCanvasElement `width` x11, `height` x11, `width =` x1,
  // `height =` x1.
  // -------------------------------------------------------------------------

  function ignoreSizeWrite(element, name, value) {
    // Writing the size it already has changes nothing, so it says nothing:
    // three.js's setSize(innerWidth, innerHeight) writes exactly that. The value
    // is converted as the IDL `unsigned long` it is.
    var current = name === 'width' ? drawableWidth() : drawableHeight();
    if ((Number(value) >>> 0) === current) return;
    announce('size:' + name,
      'ScreenKit: <' + element.tagName.toLowerCase() + '>.' + name + ' = ' + value +
      ' was ignored. This canvas is the page\'s frame and its size comes from the GL ' +
      'surface, so ' + name + ' still reads ' + (name === 'width' ? drawableWidth() : drawableHeight()) +
      '. Letting the write stick would make canvas.' + name + ' and gl.drawingBuffer' +
      (name === 'width' ? 'Width' : 'Height') + ' disagree. A canvas the CSS subset places has a ' +
      'drawing buffer of its own and does honour the write.');
  }

  // Per-canvas state: the size a script wrote, which kind of context the canvas
  // has ('webgl' or '2d', or null before it asks), the context object, and --
  // for a canvas that is not the frame -- the id of its own GL context and
  // layer (`__screenkit.canvas`).
  var canvasStates = new WeakMap();

  function canvasBufferSize(state, name) {
    var size = state.gl === null ? 0
             : state.gl[name === 'width' ? 'drawingBufferWidth' : 'drawingBufferHeight'];
    return typeof size === 'number' ? size : 0;
  }

  function defineBackedSize(element) {
    // Backed: its style parses the CSS subset (see CSSStyleDeclaration).
    var node = nodeOf(element);
    node.backed = true;
    var state = { element: element, node: node, canvas: true,
                  width: null, height: null, kind: null,
                  context2d: null, gl: null, id: null, sized: false, plane: null, connected: false,
                  // A `create` that answered null; see getContext.
                  refused: false,
                  // `planeFor` reads these for a <video>'s intrinsic size. A
                  // canvas has none of its own, so CSS's 300x150 default stands,
                  // as it does in a browser.
                  videoWidth: 0, videoHeight: 0 };
    canvasStates.set(element, state);
    function size(name, drawable) {
      return {
        enumerable: true,
        configurable: true,
        get: function () {
          if (state.id !== null) return canvasBufferSize(state, name);
          return element === backedCanvas || state[name] === null ? drawable() : state[name];
        },
        set: function (value) {
          // A detached canvas holding the surface -- an engine's probe, before
          // it gives the surface up -- is not on screen, so nothing is being
          // ignored that anyone sees: record the write quietly.
          if (element === backedCanvas && element.isConnected) {
            ignoreSizeWrite(element, name, value);
            return;
          }
          state[name] = Number(value) >>> 0;
          // Its own layer, its own drawing buffer: the write is honoured, and
          // from here on the buffer stops following the element's CSS rect.
          if (state.id !== null) {
            state.sized = true;
            resizeCanvasLayer(state);
            return;
          }
          if (state.context2d !== null) resizeContext2d(context2dStates.get(state.context2d));
        }
      };
    }
    Object.defineProperty(element, 'width', size('width', drawableWidth));
    Object.defineProperty(element, 'height', size('height', drawableHeight));
    return element;
  }

  // -------------------------------------------------------------------------
  // Events: EventTarget, and dispatch along the tree
  //
  // Every node is an EventTarget, and so is `window`. A dispatch walks the
  // DOM's event path: capture from window down to the target's parent, the
  // target itself (its capture listeners, then the rest), and -- for an event
  // that bubbles -- back up to window. The path is fixed when dispatch starts,
  // so a listener that moves a node does not reroute the event in flight.
  //
  // Listeners live in a WeakMap rather than on the objects, so a node carries
  // no bookkeeping a bundle can see or trample. A listener that throws is
  // reported and the rest still run, as in a browser.
  // -------------------------------------------------------------------------

  var NONE = 0, CAPTURING_PHASE = 1, AT_TARGET = 2, BUBBLING_PHASE = 3;
  var listenerTable = new WeakMap();

  function domError(message, name) {
    return new global.DOMException(message, name);
  }

  function inherit(Child, Parent) {
    Child.prototype = Object.create(Parent.prototype);
    Object.defineProperty(Child.prototype, 'constructor',
                          { value: Child, writable: true, enumerable: false, configurable: true });
    Object.setPrototypeOf(Child, Parent);
  }

  function define(proto, members) {
    Object.keys(members).forEach(function (name) {
      var member = members[name];
      if (typeof member === 'function') {
        proto[name] = member;
      } else {
        Object.defineProperty(proto, name, { get: member.get, set: member.set, enumerable: true, configurable: true });
      }
    });
  }

  function captureFlag(options) {
    return typeof options === 'boolean' ? options : !!(options && options.capture);
  }

  function listenersFor(target, type, create) {
    var byType = listenerTable.get(target);
    if (!byType) {
      if (!create) return null;
      byType = Object.create(null);
      listenerTable.set(target, byType);
    }
    return byType[type] || (create ? (byType[type] = []) : null);
  }

  function addListener(target, type, listener, options) {
    if (listener === null || listener === undefined) return;
    if (typeof listener !== 'function' && typeof listener !== 'object') {
      throw new TypeError("Failed to execute 'addEventListener' on 'EventTarget': parameter 2 is not of type 'Object'.");
    }
    var list = listenersFor(target, String(type), true);
    var capture = captureFlag(options);
    for (var i = 0; i < list.length; i++) {
      if (list[i].listener === listener && list[i].capture === capture) return;
    }
    list.push({ listener: listener, capture: capture, removed: false,
                once: !!(options && typeof options === 'object' && options.once) });
  }

  function removeListener(target, type, listener, options) {
    var list = listenersFor(target, String(type), false);
    if (!list) return;
    var capture = captureFlag(options);
    for (var i = 0; i < list.length; i++) {
      if (list[i].listener === listener && list[i].capture === capture) {
        list[i].removed = true;
        list.splice(i, 1);
        return;
      }
    }
  }

  // One node's listeners for one pass, as they stand now: a listener added
  // during a dispatch does not run in it.
  function listenerSnapshot(target, type, capture) {
    var list = listenersFor(target, type, false);
    var out = [];
    for (var i = 0; list && i < list.length; i++) {
      if (list[i].capture === capture) out.push(list[i]);
    }
    return out;
  }

  // One listener callback. A `once` listener is unregistered before it runs.
  // Returns false once stopImmediatePropagation has been called.
  function invokeListener(target, event, entry) {
    if (entry.removed) return true;
    if (entry.once) removeListener(target, event.type, entry.listener, entry.capture);
    event.currentTarget = target;
    try {
      if (typeof entry.listener === 'function') entry.listener.call(target, event);
      else if (typeof entry.listener.handleEvent === 'function') entry.listener.handleEvent(event);
    } catch (err) {
      global.console.error('Uncaught in ' + event.type + ' listener: ' + (err && err.stack || err));
    }
    return !event._stopImmediate;
  }

  // The next object on the event path: a node's parent, then the document,
  // then window. A detached subtree's path ends at its root.
  function eventParent(target, event) {
    if (target === global) return null;
    var node = nodeOf(target);
    if (node === null) return null;
    if (node.type === DOCUMENT_NODE) return target === documentNode && event.type !== 'load' ? global : null;
    return node.parent;
  }

  // A dispatch as a stepper: each call runs one listener callback and returns
  // true, then false once the dispatch is over. A script's dispatchEvent runs
  // it to the end in one go; the host's key events step it from native, with a
  // microtask checkpoint after every listener (see Input below).
  function dispatcher(target, event) {
    if (!event || typeof event.type !== 'string') {
      throw new TypeError("Failed to execute 'dispatchEvent' on 'EventTarget': parameter 1 is not of type 'Event'.");
    }
    if (event._dispatching) {
      throw domError("Failed to execute 'dispatchEvent' on 'EventTarget': The event is already being dispatched.",
                     'InvalidStateError');
    }
    var path = [];
    for (var at = target; at !== null; at = eventParent(at, event)) path.push(at);
    var passes = [];
    for (var i = path.length - 1; i > 0; i--) passes.push([path[i], CAPTURING_PHASE, true]);
    passes.push([target, AT_TARGET, true], [target, AT_TARGET, false]);
    for (var j = 1; event.bubbles && j < path.length; j++) passes.push([path[j], BUBBLING_PHASE, false]);

    event._dispatching = true;
    event._path = path;
    event.target = target;
    var pass = -1, node = null, list = [], index = 0, done = false;
    return function step() {
      if (done) return false;
      for (;;) {
        if (index < list.length) {
          var entry = list[index++];
          if (entry.removed) continue;
          if (!invokeListener(node, event, entry)) index = list.length;
          return true;
        }
        if (event._stopPropagation || ++pass >= passes.length) {
          done = true;
          event._dispatching = false;
          event._stopPropagation = false;
          event._stopImmediate = false;
          event.eventPhase = NONE;
          event.currentTarget = null;
          return false;
        }
        node = passes[pass][0];
        event.eventPhase = passes[pass][1];
        list = listenerSnapshot(node, event.type, passes[pass][2]);
        index = 0;
      }
    };
  }

  function dispatchNow(target, event) {
    var step = dispatcher(target, event);
    while (step()) { /* one listener per step */ }
    return !event.defaultPrevented;
  }

  // Event handler IDL attributes -- `el.onclick`, `document.onkeydown`,
  // `xhr.onload`. As HTML defines them, a handler is a listener: the first
  // non-null assignment registers it at the end of the target's listeners for
  // that type, a later assignment swaps the function in without moving it, and
  // null unregisters it. It runs in the target and bubble passes, and returning
  // false cancels the event.
  var handlerTable = new WeakMap();

  function handlerSlot(target, type, create) {
    var byType = handlerTable.get(target);
    if (!byType) {
      if (!create) return null;
      byType = Object.create(null);
      handlerTable.set(target, byType);
    }
    return byType[type] || (create ? (byType[type] = { value: null, entry: null }) : null);
  }

  function setHandler(target, type, value) {
    var slot = handlerSlot(target, type, true);
    // [LegacyTreatNonObjectAsNull]: anything that is not an object is null.
    slot.value = value !== null && (typeof value === 'object' || typeof value === 'function') ? value : null;
    if (slot.value === null) {
      if (slot.entry !== null) {
        var list = listenersFor(target, type, false);
        var at = list ? list.indexOf(slot.entry) : -1;
        if (at >= 0) list.splice(at, 1);
        slot.entry.removed = true;
        slot.entry = null;
      }
      return;
    }
    if (slot.entry !== null) return;
    slot.entry = {
      listener: function (event) {
        if (typeof slot.value === 'function' && slot.value.call(this, event) === false) event.preventDefault();
      },
      capture: false, removed: false, once: false
    };
    listenersFor(target, type, true).push(slot.entry);
  }

  function defineEventHandlers(target, types) {
    types.forEach(function (type) {
      Object.defineProperty(target, 'on' + type, {
        get: function () {
          var slot = handlerSlot(this, type, false);
          return slot ? slot.value : null;
        },
        set: function (value) { setHandler(this, type, value); },
        enumerable: true,
        configurable: true
      });
    });
  }

  // GlobalEventHandlers and DocumentAndElementEventHandlers, as Chromium exposes
  // them on a desktop without touch: no `ontouch*`, because `'ontouchstart' in
  // window` is how libraries detect a touch screen, and a TV has none.
  var GLOBAL_EVENT_HANDLERS = [
    'abort', 'animationend', 'animationiteration', 'animationstart', 'auxclick', 'beforeinput', 'blur',
    'cancel', 'canplay', 'canplaythrough', 'change', 'click', 'close', 'contextmenu', 'copy', 'cut',
    'dblclick', 'drag', 'dragend', 'dragenter', 'dragleave', 'dragover', 'dragstart', 'drop',
    'durationchange', 'emptied', 'ended', 'error', 'focus', 'gotpointercapture', 'input', 'invalid',
    'keydown', 'keypress', 'keyup', 'load', 'loadeddata', 'loadedmetadata', 'loadstart',
    'lostpointercapture', 'mousedown', 'mouseenter', 'mouseleave', 'mousemove', 'mouseout', 'mouseover',
    'mouseup', 'paste', 'pause', 'play', 'playing', 'pointercancel', 'pointerdown', 'pointerenter',
    'pointerleave', 'pointermove', 'pointerout', 'pointerover', 'pointerup', 'progress', 'ratechange',
    'reset', 'resize', 'scroll', 'scrollend', 'seeked', 'seeking', 'select', 'selectionchange',
    'selectstart', 'stalled', 'submit', 'suspend', 'timeupdate', 'toggle', 'transitioncancel',
    'transitionend', 'transitionrun', 'transitionstart', 'volumechange', 'waiting', 'wheel'
  ];
  var WINDOW_EVENT_HANDLERS = [
    'afterprint', 'beforeprint', 'beforeunload', 'gamepadconnected', 'gamepaddisconnected', 'hashchange',
    'languagechange', 'message',
    'messageerror', 'offline', 'online', 'pagehide', 'pageshow', 'popstate', 'rejectionhandled',
    // `resume` is this runtime's own, the other half of the `pause` a paused
    // instance gets (Architecture.md 5.1). `onpause` is already a global
    // handler, so only this one is new here.
    'resume',
    'storage', 'unhandledrejection', 'unload'
  ];

  function EventTarget() {
    if (!(this instanceof EventTarget)) {
      throw new TypeError("Failed to construct 'EventTarget': Please use the 'new' operator.");
    }
  }
  define(EventTarget.prototype, {
    addEventListener: function (type, listener, options) { addListener(this, type, listener, options); },
    removeEventListener: function (type, listener, options) { removeListener(this, type, listener, options); },
    dispatchEvent: function (event) { return dispatchNow(this, event); }
  });
  global.EventTarget = EventTarget;

  // -------------------------------------------------------------------------
  // The element tree
  //
  // Hand-written, the M2 fallback (Architecture.md 3): Node, Element,
  // HTMLElement and Document, with parent/child links, attributes, the tree
  // operations and their DOM exceptions. Per-node state lives in a WeakMap, so
  // an element's own properties stay the bundle's -- `link.rel = ...` still sets
  // and reads back an ordinary property.
  //
  // Only three elements ever paint (Architecture.md 3.1). Everything else lives
  // in the tree, dispatches events and never renders.
  //
  // Collections from a query (querySelectorAll, getElementsBy*) are snapshots.
  // `childNodes`, `children` and `classList` are live.
  // -------------------------------------------------------------------------

  var ELEMENT_NODE = 1, TEXT_NODE = 3, DOCUMENT_NODE = 9;
  var HTML_NAMESPACE = 'http://www.w3.org/1999/xhtml';
  var nodes = new WeakMap();
  var documentNode = null;  // the one document, made at the end of this section

  function nodeOf(value) {
    return value !== null && typeof value === 'object' ? nodes.get(value) || null : null;
  }
  function self(value) {
    var node = nodeOf(value);
    if (node === null) throw new TypeError('Illegal invocation');
    return node;
  }
  function requireNode(value, method, iface, position) {
    if (nodeOf(value) === null) {
      throw new TypeError("Failed to execute '" + method + "' on '" + iface + "': parameter " + (position || 1) +
                          " is not of type 'Node'.");
    }
    return value;
  }
  function requireArgs(count, needed, method, iface) {
    if (count < needed) {
      throw new TypeError("Failed to execute '" + method + "' on '" + iface + "': " + needed + ' argument' +
                          (needed > 1 ? 's' : '') + ' required, but only ' + count + ' present.');
    }
  }

  function register(object, type, localName) {
    var node = { object: object, type: type, parent: null, kids: [], localName: localName, attrs: [],
                 namespace: type === ELEMENT_NODE ? HTML_NAMESPACE : null,
                 // The document the node belongs to; null means `document`. XML
                 // documents from DOMParser set their own.
                 ownerDocument: null, data: '', prefix: null,
                 childNodes: null, children: null, classList: null, style: null, backed: false,
                 observers: null };
    nodes.set(object, node);
    return node;
  }

  // Array-like collections. Live ones are refilled whenever their node's
  // children or class change, so an index read is a plain property read.
  function fill(list, items) {
    var previous = list.length | 0;
    for (var i = 0; i < items.length; i++) {
      Object.defineProperty(list, i, { value: items[i], writable: false, enumerable: true, configurable: true });
    }
    for (var j = items.length; j < previous; j++) delete list[j];
    Object.defineProperty(list, 'length', { value: items.length, writable: false, enumerable: false, configurable: true });
    return list;
  }
  function collection(proto, items) {
    return fill(Object.create(proto), items);
  }
  function listMembers(proto) {
    define(proto, {
      item: function (index) {
        var i = Number(index) >>> 0;
        return i < this.length ? this[i] : null;
      },
      forEach: function (callback, thisArg) {
        for (var i = 0; i < this.length; i++) callback.call(thisArg, this[i], i, this);
      },
      entries: Array.prototype.entries,
      keys: Array.prototype.keys,
      values: Array.prototype.values
    });
    proto[Symbol.iterator] = Array.prototype.values;
  }

  function NodeList() { throw new TypeError("Failed to construct 'NodeList': Illegal constructor"); }
  listMembers(NodeList.prototype);
  global.NodeList = NodeList;

  function HTMLCollection() { throw new TypeError("Failed to construct 'HTMLCollection': Illegal constructor"); }
  listMembers(HTMLCollection.prototype);
  HTMLCollection.prototype.namedItem = function (name) {
    var key = String(name);
    for (var i = 0; key !== '' && i < this.length; i++) {
      var node = nodeOf(this[i]);
      if (getAttr(node, 'id') === key || getAttr(node, 'name') === key) return this[i];
    }
    return null;
  };
  delete HTMLCollection.prototype.forEach;
  delete HTMLCollection.prototype.entries;
  delete HTMLCollection.prototype.keys;
  delete HTMLCollection.prototype.values;
  global.HTMLCollection = HTMLCollection;

  function elementKids(node) {
    return node.kids.filter(function (kid) { return nodes.get(kid).type === ELEMENT_NODE; });
  }
  function childrenChanged(node) {
    if (node.childNodes !== null) fill(node.childNodes, node.kids);
    if (node.children !== null) fill(node.children, elementKids(node));
  }
  function sibling(object, offset, elementsOnly) {
    var parent = self(object).parent;
    if (parent === null) return null;
    var kids = elementsOnly ? elementKids(nodes.get(parent)) : nodes.get(parent).kids;
    var at = kids.indexOf(object) + offset;
    return at >= 0 && at < kids.length ? kids[at] : null;
  }
  function elementParent(object) {
    var parent = nodes.get(object).parent;
    return parent !== null && nodes.get(parent).type === ELEMENT_NODE ? parent : null;
  }

  // Document order, not counting `root` itself. `visit` returning true stops.
  function walk(root, visit) {
    var stack = nodes.get(root).kids.slice().reverse();
    while (stack.length > 0) {
      var object = stack.pop();
      var node = nodes.get(object);
      if (node.type === ELEMENT_NODE && visit(object, node) === true) return;
      for (var i = node.kids.length - 1; i >= 0; i--) stack.push(node.kids[i]);
    }
  }

  function isInclusiveAncestor(ancestor, object) {
    for (var at = object; at !== null; at = nodes.get(at).parent) {
      if (at === ancestor) return true;
    }
    return false;
  }

  // The DOM's "ensure pre-insert validity" (and its replace counterpart), for
  // the two node types that exist here.
  function checkInsert(method, parent, object, child, replacing) {
    var node = nodes.get(object), parentNode = nodes.get(parent);
    if (isInclusiveAncestor(object, parent)) {
      throw domError("Failed to execute '" + method + "' on 'Node': The new child element contains the parent.",
                     'HierarchyRequestError');
    }
    if (child !== null && nodes.get(child).parent !== parent) {
      throw domError("Failed to execute '" + method + "' on 'Node': The node " +
                     (replacing ? 'to be replaced' : 'before which the new node is to be inserted') +
                     ' is not a child of this node.', 'NotFoundError');
    }
    if (node.type === DOCUMENT_NODE || parentNode.type === TEXT_NODE ||
        (node.type === TEXT_NODE && parentNode.type === DOCUMENT_NODE)) {
      throw domError("Failed to execute '" + method + "' on 'Node': Nodes of type '" + nameOf(node) +
                     "' may not be inserted inside nodes of type '" + nameOf(parentNode) + "'.", 'HierarchyRequestError');
    }
    if (parentNode.type === DOCUMENT_NODE) {
      var others = elementKids(parentNode).filter(function (kid) { return !(replacing && kid === child); });
      if (others.length > 0) {
        throw domError("Failed to execute '" + method + "' on 'Node': Only one element on document allowed.",
                       'HierarchyRequestError');
      }
    }
  }

  // `quiet` leaves the mutation record to the caller: replaceChild reports its
  // removal and insertion as one record, as the DOM does.
  function detach(object, quiet) {
    var node = nodes.get(object);
    if (node.parent === null) return;
    var parent = node.parent, parentNode = nodes.get(parent);
    var index = parentNode.kids.indexOf(object);
    var previous = index > 0 ? parentNode.kids[index - 1] : null;
    var next = index + 1 < parentNode.kids.length ? parentNode.kids[index + 1] : null;
    parentNode.kids.splice(index, 1);
    node.parent = null;
    childrenChanged(parentNode);
    keepObserving(parent, object);
    if (!quiet) queueMutationRecord('childList', parent, null, null, [], [object], previous, next);
    // A <video> leaving the document pauses and hides its plane; an <iframe>
    // leaving it is terminated, as HTML terminates a removed browsing context.
    if (mediaElementCount > 0) mediaTreeChanged(object);
    if (frameElementCount > 0) frameTreeChanged(object);
    if (canvasLayerCount > 0) canvasTreeChanged(object);
  }

  // Insert `object` into `parent` before `child` (null: at the end), moving it
  // out of wherever it was first.
  function insert(parent, object, child, quiet) {
    var reference = child === object ? sibling(object, 1, false) : child;
    detach(object);
    var parentNode = nodes.get(parent);
    var index = reference === null ? parentNode.kids.length : parentNode.kids.indexOf(reference);
    var previous = index > 0 ? parentNode.kids[index - 1] : null;
    parentNode.kids.splice(index, 0, object);
    nodes.get(object).parent = parent;
    childrenChanged(parentNode);
    if (!quiet) queueMutationRecord('childList', parent, null, null, [object], [], previous, reference);
    if (mediaElementCount > 0) mediaTreeChanged(object);
    if (frameElementCount > 0) frameTreeChanged(object);
    if (canvasLayerCount > 0) canvasTreeChanged(object);
    return object;
  }

  // --- mutation observers ----------------------------------------------------
  // The DOM's "queue a mutation record": a tree or attribute change is offered to
  // the observers registered on its target and, with `subtree`, on the target's
  // ancestors, and each observer gets its records together from one microtask.
  // A node removed from an observed subtree stays observed (a transient
  // registration) until that delivery, so a change made to it straight after its
  // removal still arrives. `characterData` records come from text nodes' data.

  var observerStates = new WeakMap();
  var observersToNotify = [];
  var observerCount = 0;
  var mutationDeliveryQueued = false;

  function scheduleMutationDelivery(observer) {
    if (observersToNotify.indexOf(observer) < 0) observersToNotify.push(observer);
    if (mutationDeliveryQueued) return;
    mutationDeliveryQueued = true;
    Promise.resolve().then(deliverMutations);
  }

  function queueMutationRecord(type, target, name, oldValue, addedNodes, removedNodes, previous, next) {
    var interested = [], oldValues = [];
    for (var at = target; at !== null; at = nodes.get(at).parent) {
      var registrations = nodes.get(at).observers || [];
      for (var i = 0; i < registrations.length; i++) {
        var options = registrations[i].options;
        if (at !== target && !options.subtree) continue;
        if (type === 'childList' && !options.childList) continue;
        if (type === 'characterData' && !options.characterData) continue;
        if (type === 'attributes' && (!options.attributes ||
            (options.attributeFilter !== null && options.attributeFilter.indexOf(name) < 0))) continue;
        var index = interested.indexOf(registrations[i].observer);
        if (index < 0) {
          index = interested.push(registrations[i].observer) - 1;
          oldValues.push(null);
        }
        if (type === 'attributes' && options.attributeOldValue) oldValues[index] = oldValue;
        if (type === 'characterData' && options.characterDataOldValue) oldValues[index] = oldValue;
      }
    }
    interested.forEach(function (observer, index) {
      var record = Object.create(MutationRecord.prototype);
      record.type = type;
      record.target = target;
      record.addedNodes = collection(NodeList.prototype, addedNodes);
      record.removedNodes = collection(NodeList.prototype, removedNodes);
      record.previousSibling = previous;
      record.nextSibling = next;
      record.attributeName = name;
      record.attributeNamespace = null;
      record.oldValue = oldValues[index];
      observerStates.get(observer).records.push(record);
      scheduleMutationDelivery(observer);
    });
  }

  // The removing steps' transient registrations: whoever watched `parent`'s
  // subtree keeps watching `object` until the next delivery.
  function keepObserving(parent, object) {
    for (var at = parent; at !== null; at = nodes.get(at).parent) {
      var registrations = nodes.get(at).observers || [];
      for (var i = 0; i < registrations.length; i++) {
        if (!registrations[i].options.subtree) continue;
        var node = nodes.get(object);
        if (node.observers === null) node.observers = [];
        node.observers.push({ observer: registrations[i].observer, options: registrations[i].options,
                              source: registrations[i] });
        var state = observerStates.get(registrations[i].observer);
        if (state.nodes.indexOf(object) < 0) state.nodes.push(object);
        scheduleMutationDelivery(registrations[i].observer);
      }
    }
  }

  function dropRegistrations(observer, state, keep) {
    state.nodes = state.nodes.filter(function (object) {
      var node = nodes.get(object);
      node.observers = node.observers.filter(function (r) { return r.observer !== observer || keep(r); });
      return node.observers.some(function (r) { return r.observer === observer; });
    });
  }

  function deliverMutations() {
    mutationDeliveryQueued = false;
    var observers = observersToNotify.sort(function (a, b) {
      return observerStates.get(a).id - observerStates.get(b).id;
    });
    observersToNotify = [];
    observers.forEach(function (observer) {
      var state = observerStates.get(observer);
      var records = state.records;
      state.records = [];
      dropRegistrations(observer, state, function (r) { return r.source === null; });
      if (records.length === 0) return;
      try {
        state.callback.call(observer, records, observer);
      } catch (err) {
        global.console.error('Uncaught in MutationObserver callback: ' + (err && err.stack || err));
      }
    });
  }

  function MutationRecord() { throw new TypeError("Failed to construct 'MutationRecord': Illegal constructor"); }
  global.MutationRecord = MutationRecord;

  function observerOf(value, method) {
    var state = value !== null && typeof value === 'object' ? observerStates.get(value) : undefined;
    if (!state) {
      throw new TypeError("Failed to execute '" + method + "' on 'MutationObserver': Illegal invocation");
    }
    return state;
  }

  function MutationObserver(callback) {
    if (!(this instanceof MutationObserver)) {
      throw new TypeError("Failed to construct 'MutationObserver': Please use the 'new' operator.");
    }
    if (typeof callback !== 'function') {
      throw new TypeError("Failed to construct 'MutationObserver': parameter 1 is not of type 'Function'.");
    }
    observerStates.set(this, { id: ++observerCount, callback: callback, records: [], nodes: [] });
  }
  define(MutationObserver.prototype, {
    observe: function (target, init) {
      var state = observerOf(this, 'observe');
      requireArgs(arguments.length, 1, 'observe', 'MutationObserver');
      requireNode(target, 'observe', 'MutationObserver');
      var fail = function (message) {
        throw new TypeError("Failed to execute 'observe' on 'MutationObserver': " + message);
      };
      init = init === undefined || init === null ? {} : init;
      var options = {
        childList: !!init.childList,
        attributes: init.attributes,
        characterData: init.characterData,
        subtree: !!init.subtree,
        attributeOldValue: init.attributeOldValue,
        characterDataOldValue: init.characterDataOldValue,
        attributeFilter: init.attributeFilter === undefined ? null : Array.prototype.map.call(init.attributeFilter, String)
      };
      if ((options.attributeOldValue !== undefined || options.attributeFilter !== null) &&
          options.attributes === undefined) {
        options.attributes = true;
      }
      if (options.characterDataOldValue !== undefined && options.characterData === undefined) {
        options.characterData = true;
      }
      options.attributes = !!options.attributes;
      options.characterData = !!options.characterData;
      options.attributeOldValue = !!options.attributeOldValue;
      options.characterDataOldValue = !!options.characterDataOldValue;
      if (!options.childList && !options.attributes && !options.characterData) {
        fail("The options object must set at least one of 'attributes', 'characterData', or 'childList' to true.");
      }
      if (options.attributeOldValue && !options.attributes) {
        fail("The options object may only set 'attributeOldValue' to true when 'attributes' is true or not present.");
      }
      if (options.attributeFilter !== null && !options.attributes) {
        fail("The options object may only set 'attributeFilter' when 'attributes' is true or not present.");
      }
      if (options.characterDataOldValue && !options.characterData) {
        fail("The options object may only set 'characterDataOldValue' to true when 'characterData' is true or not present.");
      }

      var observer = this, node = nodes.get(target);
      if (node.observers === null) node.observers = [];
      var existing = node.observers.filter(function (r) { return r.observer === observer && r.source === null; })[0];
      if (existing) {
        // Observing the same node again replaces its options, and ends the
        // transient registrations the old ones produced.
        dropRegistrations(observer, state, function (r) { return r.source !== existing; });
        existing.options = options;
        if (state.nodes.indexOf(target) < 0) state.nodes.push(target);
      } else {
        node.observers.push({ observer: observer, options: options, source: null });
        if (state.nodes.indexOf(target) < 0) state.nodes.push(target);
      }
    },
    disconnect: function () {
      var state = observerOf(this, 'disconnect');
      dropRegistrations(this, state, function () { return false; });
      state.records = [];
    },
    takeRecords: function () {
      var state = observerOf(this, 'takeRecords');
      var records = state.records;
      state.records = [];
      return records;
    }
  });
  global.MutationObserver = MutationObserver;

  function nameOf(node) {
    if (node.type === DOCUMENT_NODE) return '#document';
    if (node.type === TEXT_NODE) return '#text';
    return node.namespace === HTML_NAMESPACE ? node.localName.toUpperCase() : qualifiedNameOf(node);
  }
  function qualifiedNameOf(node) {
    return node.prefix === null ? node.localName : node.prefix + ':' + node.localName;
  }

  // The text of a node's subtree, in document order: what textContent reads.
  function textOf(node) {
    if (node.type === TEXT_NODE) return node.data;
    var out = '';
    for (var i = 0; i < node.kids.length; i++) out += textOf(nodes.get(node.kids[i]));
    return out;
  }

  // CharacterData's "replace data": the new text, and a characterData record.
  function setTextData(node, value) {
    var old = node.data;
    queueMutationRecord('characterData', node.object, null, old, [], [], null, null);
    node.data = value;
  }

  function Node() { throw new TypeError("Failed to construct 'Node': Illegal constructor"); }
  inherit(Node, EventTarget);
  var NODE_TYPES = {
    ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4, ENTITY_REFERENCE_NODE: 5,
    ENTITY_NODE: 6, PROCESSING_INSTRUCTION_NODE: 7, COMMENT_NODE: 8, DOCUMENT_NODE: 9,
    DOCUMENT_TYPE_NODE: 10, DOCUMENT_FRAGMENT_NODE: 11, NOTATION_NODE: 12
  };
  Object.keys(NODE_TYPES).forEach(function (name) {
    Node[name] = NODE_TYPES[name];
    Node.prototype[name] = NODE_TYPES[name];
  });
  define(Node.prototype, {
    nodeType: { get: function () { return self(this).type; } },
    nodeName: { get: function () { return nameOf(self(this)); } },
    nodeValue: {
      get: function () { var node = self(this); return node.type === TEXT_NODE ? node.data : null; },
      set: function (value) {
        var node = self(this);
        if (node.type === TEXT_NODE) setTextData(node, value === null ? '' : String(value));
      }
    },
    ownerDocument: { get: function () {
      var node = self(this);
      return node.type === DOCUMENT_NODE ? null : node.ownerDocument || documentNode;
    } },
    // The text of the subtree. Setting it on an element replaces every child with
    // one text node (none for ''); on a document it does nothing, as in a browser.
    textContent: {
      get: function () {
        var node = self(this);
        return node.type === DOCUMENT_NODE ? null : textOf(node);
      },
      set: function (value) {
        var node = self(this);
        var text = value === null || value === undefined ? '' : String(value);
        if (node.type === TEXT_NODE) {
          setTextData(node, text);
          return;
        }
        if (node.type === DOCUMENT_NODE) return;
        while (node.kids.length > 0) detach(node.kids[0]);
        if (text !== '') insert(this, makeText(text, node.ownerDocument), null);
      }
    },
    parentNode: { get: function () { return self(this).parent; } },
    parentElement: { get: function () { self(this); return elementParent(this); } },
    isConnected: { get: function () {
      var at = this;
      while (self(at).parent !== null) at = nodes.get(at).parent;
      return at === documentNode;
    } },
    childNodes: { get: function () {
      var node = self(this);
      if (node.childNodes === null) node.childNodes = collection(NodeList.prototype, node.kids);
      return node.childNodes;
    } },
    firstChild: { get: function () { var kids = self(this).kids; return kids.length ? kids[0] : null; } },
    lastChild: { get: function () { var kids = self(this).kids; return kids.length ? kids[kids.length - 1] : null; } },
    previousSibling: { get: function () { return sibling(this, -1, false); } },
    nextSibling: { get: function () { return sibling(this, 1, false); } },
    hasChildNodes: function () { return self(this).kids.length > 0; },
    getRootNode: function () {
      var at = this;
      while (self(at).parent !== null) at = nodes.get(at).parent;
      return at;
    },
    contains: function (other) {
      self(this);
      if (other === null || other === undefined) return false;
      requireNode(other, 'contains', 'Node');
      return isInclusiveAncestor(this, other);
    },
    appendChild: function (object) {
      self(this);
      requireArgs(arguments.length, 1, 'appendChild', 'Node');
      requireNode(object, 'appendChild', 'Node');
      checkInsert('appendChild', this, object, null, false);
      return insert(this, object, null);
    },
    insertBefore: function (object, child) {
      self(this);
      requireArgs(arguments.length, 2, 'insertBefore', 'Node');
      requireNode(object, 'insertBefore', 'Node');
      if (child === undefined) child = null;
      if (child !== null) requireNode(child, 'insertBefore', 'Node', 2);
      checkInsert('insertBefore', this, object, child, false);
      return insert(this, object, child);
    },
    removeChild: function (child) {
      self(this);
      requireArgs(arguments.length, 1, 'removeChild', 'Node');
      requireNode(child, 'removeChild', 'Node');
      if (nodes.get(child).parent !== this) {
        throw domError("Failed to execute 'removeChild' on 'Node': The node to be removed is not a child of this node.",
                       'NotFoundError');
      }
      detach(child);
      return child;
    },
    replaceChild: function (object, child) {
      self(this);
      requireArgs(arguments.length, 2, 'replaceChild', 'Node');
      requireNode(object, 'replaceChild', 'Node');
      requireNode(child, 'replaceChild', 'Node', 2);
      checkInsert('replaceChild', this, object, child, true);
      if (object === child) return child;
      var reference = sibling(child, 1, false);
      if (reference === object) reference = sibling(object, 1, false);
      detach(object);
      var previous = sibling(child, -1, false);
      detach(child, true);
      insert(this, object, reference, true);
      queueMutationRecord('childList', this, null, null, [object], [child], previous, reference);
      return child;
    }
  });
  global.Node = Node;

  // --- attributes ------------------------------------------------------------
  // Names are lower-cased, as for an HTML element in an HTML document.

  function attributeName(name, method) {
    var text = String(name);
    if (text === '' || /[\t\n\f\r \u0000\/>=]/.test(text)) {
      throw domError("Failed to execute '" + method + "' on 'Element': '" + text + "' is not a valid attribute name.",
                     'InvalidCharacterError');
    }
    return text.toLowerCase();
  }
  function foldName(node, name) {
    return node.namespace === HTML_NAMESPACE ? String(name).toLowerCase() : String(name);
  }
  function attrIndex(node, name) {
    for (var i = 0; i < node.attrs.length; i++) if (node.attrs[i][0] === name) return i;
    return -1;
  }
  function getAttr(node, name) {
    var i = attrIndex(node, name);
    return i < 0 ? null : node.attrs[i][1];
  }
  var reflectingStyle = false;
  function setAttr(node, name, value) {
    var i = attrIndex(node, name);
    queueMutationRecord('attributes', node.object, name, i < 0 ? null : node.attrs[i][1], [], [], null, null);
    if (i < 0) node.attrs.push([name, value]);
    else node.attrs[i][1] = value;
    attributeChanged(node, name, value);
  }
  function removeAttr(node, name) {
    var i = attrIndex(node, name);
    if (i < 0) return;
    queueMutationRecord('attributes', node.object, name, node.attrs[i][1], [], [], null, null);
    node.attrs.splice(i, 1);
    attributeChanged(node, name, null);
  }
  function attributeChanged(node, name, value) {
    if (name === 'class' && node.classList !== null) fill(node.classList, classTokens(node));
    // `src` loads a media element; `width` and `height` size a video's plane.
    if (mediaElementCount > 0 && (name === 'src' || name === 'width' || name === 'height')) {
      mediaAttributeChanged(node, name);
    }
    // An <iframe>'s `src` loads a package, `sandbox` is its token list, and
    // `width`/`height` size its layer.
    if (frameElementCount > 0 &&
        (name === 'src' || name === 'sandbox' || name === 'width' || name === 'height')) {
      frameAttributeChanged(node, name);
    }
    // On the web `width`/`height` are content attributes that reflect into the
    // IDL ones, and libraries size canvases both ways. Now that a canvas with a
    // layer of its own honours a size write, setAttribute has to arrive at the
    // same place canvas.width does -- otherwise the drawing buffer silently
    // ignores half the API.
    if (canvasLayerCount > 0 && (name === 'width' || name === 'height')) {
      var canvasState = canvasStates.get(node.object);
      if (canvasState !== undefined && canvasState.id !== null) {
        // `null` is the attribute being removed, which puts the canvas back on
        // the web's default for that side.
        canvasState.element[name] = value === null ? (name === 'width' ? 300 : 150)
                                                   : Number(value) >>> 0;
      }
    }
    // A backed element parses at once, so setAttribute('style') warns when a
    // property does, not later when something first reads `style`.
    if (name === 'style' && node.style === null && node.backed && !reflectingStyle) {
      styleOf(node);
      return;
    }
    if (name === 'style' && node.style !== null && !reflectingStyle) {
      reflectingStyle = true;
      try {
        replaceDeclarations(node.style, value === null ? '' : value);
      } finally {
        reflectingStyle = false;
      }
    }
  }

  // --- classList -----------------------------------------------------------

  // `class` by default, `sandbox` for an <iframe>: the list is the same object
  // model over whichever attribute owns it.
  function classTokens(node, attribute) {
    var value = getAttr(node, attribute || 'class');
    var out = [];
    (value === null ? '' : value).split(/[\t\n\f\r ]+/).forEach(function (token) {
      if (token !== '' && out.indexOf(token) < 0) out.push(token);
    });
    return out;
  }
  // `{node, name}`: which element, and which attribute the tokens live in.
  var tokenOwners = new WeakMap();
  function tokenOwner(list) {
    var owner = tokenOwners.get(list);
    if (!owner) throw new TypeError('Illegal invocation');
    return owner;
  }
  function checkToken(token, method) {
    var text = String(token);
    if (text === '') {
      throw domError("Failed to execute '" + method + "' on 'DOMTokenList': The token provided must not be empty.",
                     'SyntaxError');
    }
    if (/[\t\n\f\r ]/.test(text)) {
      throw domError("Failed to execute '" + method + "' on 'DOMTokenList': The token provided ('" + text +
                     "') contains HTML space characters, which are not valid in tokens.", 'InvalidCharacterError');
    }
    return text;
  }
  function writeTokens(owner, tokens) {
    if (getAttr(owner.node, owner.name) === null && tokens.length === 0) return;
    setAttr(owner.node, owner.name, tokens.join(' '));
  }

  function DOMTokenList() { throw new TypeError("Failed to construct 'DOMTokenList': Illegal constructor"); }
  listMembers(DOMTokenList.prototype);
  define(DOMTokenList.prototype, {
    value: {
      get: function () {
        var owner = tokenOwner(this), value = getAttr(owner.node, owner.name);
        return value === null ? '' : value;
      },
      set: function (value) { var owner = tokenOwner(this); setAttr(owner.node, owner.name, String(value)); }
    },
    toString: function () { return this.value; },
    contains: function (token) {
      var owner = tokenOwner(this);
      return classTokens(owner.node, owner.name).indexOf(String(token)) >= 0;
    },
    add: function () {
      var owner = tokenOwner(this), tokens = classTokens(owner.node, owner.name);
      for (var i = 0; i < arguments.length; i++) {
        var token = checkToken(arguments[i], 'add');
        if (tokens.indexOf(token) < 0) tokens.push(token);
      }
      writeTokens(owner, tokens);
    },
    remove: function () {
      var owner = tokenOwner(this), tokens = classTokens(owner.node, owner.name);
      for (var i = 0; i < arguments.length; i++) {
        var token = checkToken(arguments[i], 'remove');
        tokens = tokens.filter(function (t) { return t !== token; });
      }
      writeTokens(owner, tokens);
    },
    toggle: function (token, force) {
      var owner = tokenOwner(this), text = checkToken(token, 'toggle');
      var tokens = classTokens(owner.node, owner.name);
      if (tokens.indexOf(text) >= 0) {
        if (force === undefined || !force) {
          writeTokens(owner, tokens.filter(function (t) { return t !== text; }));
          return false;
        }
        return true;
      }
      if (force === undefined || force) {
        tokens.push(text);
        writeTokens(owner, tokens);
        return true;
      }
      return false;
    },
    replace: function (token, replacement) {
      var owner = tokenOwner(this), from = checkToken(token, 'replace'), to = checkToken(replacement, 'replace');
      var tokens = classTokens(owner.node, owner.name), at = tokens.indexOf(from);
      if (at < 0) return false;
      var out = [];
      tokens.forEach(function (t, i) {
        var next = i === at ? to : t;
        if (!(next === to && out.indexOf(to) >= 0)) out.push(next);
      });
      writeTokens(owner, out);
      return true;
    }
  });
  global.DOMTokenList = DOMTokenList;

  // --- selectors -----------------------------------------------------------
  //
  // The subset: type, `*`, `#id`, `.class`, `[attr]`, `[attr=value]` (value
  // quoted or an identifier), compound selectors, the descendant and `>`
  // combinators, and selector lists. Anything else throws a SyntaxError
  // DOMException, as a browser does for a selector it cannot parse -- a
  // selector this runtime cannot answer must not quietly match nothing.

  var selectorCache = Object.create(null);
  var selectorCacheSize = 0;
  var SELECTOR_SUBSET = 'type, *, #id, .class, [attr], [attr=value], compound selectors, the descendant ' +
                        'and > combinators, and selector lists';

  function parseSelectors(source, method, iface) {
    var text = String(source);
    var cached = selectorCache[text];
    if (cached) return cached;
    var pos = 0, len = text.length;

    function fail(unsupported) {
      throw domError("Failed to execute '" + method + "' on '" + iface + "': '" + text + "' is " +
                     (unsupported ? 'not a selector this runtime supports: ' + unsupported +
                                    ' is outside ScreenKit\'s subset (' + SELECTOR_SUBSET + ').'
                                  : 'not a valid selector.'), 'SyntaxError');
    }
    function isSpace(c) { return c === ' ' || c === '\t' || c === '\n' || c === '\r' || c === '\f'; }
    function skipSpace() {
      var start = pos;
      while (pos < len && isSpace(text.charAt(pos))) pos++;
      return pos > start;
    }
    function nameStart(c) { return /[a-zA-Z_]/.test(c) || c.charCodeAt(0) >= 0x80; }
    function nameChar(c) { return /[a-zA-Z0-9_\-]/.test(c) || c.charCodeAt(0) >= 0x80; }
    function escape() {
      pos++;
      if (pos >= len) return '�';
      var hex = /^[0-9a-fA-F]{1,6}/.exec(text.slice(pos, pos + 6));
      if (hex) {
        pos += hex[0].length;
        if (isSpace(text.charAt(pos))) pos++;
        var code = parseInt(hex[0], 16);
        return code === 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF) ? '�' : String.fromCodePoint(code);
      }
      if (text.charAt(pos) === '\n') fail();
      return text.charAt(pos++);
    }
    function identifier() {
      var start = pos, out = '';
      if (text.charAt(pos) === '-') { out = '-'; pos++; }
      var c = text.charAt(pos);
      if (out === '-' && c === '-') { out = '--'; pos++; }
      else if (c === '\\' && text.charAt(pos + 1) !== '\n' && pos + 1 < len) out += escape();
      else if (pos < len && nameStart(c)) { out += c; pos++; }
      else { pos = start; return null; }
      while (pos < len) {
        c = text.charAt(pos);
        if (c === '\\') out += escape();
        else if (nameChar(c)) { out += c; pos++; }
        else break;
      }
      return out;
    }
    function quoted() {
      var quote = text.charAt(pos++), out = '';
      while (pos < len) {
        var c = text.charAt(pos);
        if (c === quote) { pos++; return out; }
        if (c === '\n') fail();
        if (c === '\\') {
          if (text.charAt(pos + 1) === '\n') { pos += 2; continue; }
          out += escape();
          continue;
        }
        out += c;
        pos++;
      }
      return fail();
    }
    function attribute() {
      skipSpace();
      if (text.charAt(pos) === '*' || text.charAt(pos) === '|') fail('a namespace prefix');
      var name = identifier();
      if (name === null) fail();
      if (text.charAt(pos) === '|' && text.charAt(pos + 1) !== '=') fail('a namespace prefix');
      skipSpace();
      var c = text.charAt(pos);
      if (c === ']') { pos++; return { name: name.toLowerCase(), value: null }; }
      if (c !== '=') {
        if ('~|^$*'.indexOf(c) >= 0 && c !== '' && text.charAt(pos + 1) === '=') fail('the [attr' + c + '=value] operator');
        fail();
      }
      pos++;
      skipSpace();
      c = text.charAt(pos);
      var value = c === '"' || c === "'" ? quoted() : identifier();
      if (value === null) fail();
      skipSpace();
      if (text.charAt(pos) === ']') { pos++; return { name: name.toLowerCase(), value: value }; }
      var flag = identifier();
      skipSpace();
      if (flag !== null && /^[is]$/i.test(flag) && text.charAt(pos) === ']') fail('the attribute flag ' + flag);
      return fail();
    }
    function compound() {
      var part = { tag: null, ids: [], classes: [], attrs: [] }, any = false;
      if (text.charAt(pos) === '*') {
        pos++;
        part.tag = '*';
        any = true;
      } else {
        var tag = identifier();
        if (tag !== null) { part.tag = tag.toLowerCase(); any = true; }
      }
      if (text.charAt(pos) === '|') fail('a namespace prefix');
      for (;;) {
        var c = text.charAt(pos);
        if (c === '#') {
          pos++;
          var id = identifier();
          if (id === null) fail();
          part.ids.push(id);
        } else if (c === '.') {
          pos++;
          var name = identifier();
          if (name === null) fail();
          part.classes.push(name);
        } else if (c === '[') {
          pos++;
          part.attrs.push(attribute());
        } else if (c === ':') {
          fail(text.charAt(pos + 1) === ':' ? 'a pseudo-element' : 'a pseudo-class');
        } else {
          break;
        }
        any = true;
      }
      return any ? part : null;
    }
    function complex() {
      var selector = { compounds: [], combinators: [null] };
      var first = compound();
      if (first === null) fail();
      selector.compounds.push(first);
      for (;;) {
        var spaced = skipSpace();
        var c = text.charAt(pos);
        if (pos >= len || c === ',') return selector;
        var combinator;
        if (c === '>') {
          pos++;
          skipSpace();
          combinator = '>';
        } else if (c === '+' || c === '~') {
          fail('the ' + c + ' combinator');
        } else if (spaced) {
          combinator = ' ';
        } else {
          fail();
        }
        var next = compound();
        if (next === null) {
          c = text.charAt(pos);
          if (c === '+' || c === '~') fail('the ' + c + ' combinator');
          fail();
        }
        selector.combinators.push(combinator);
        selector.compounds.push(next);
      }
    }

    skipSpace();
    var list = [complex()];
    while (pos < len) {
      if (text.charAt(pos) !== ',') fail();
      pos++;
      skipSpace();
      list.push(complex());
    }
    if (selectorCacheSize >= 256) {
      selectorCache = Object.create(null);
      selectorCacheSize = 0;
    }
    selectorCache[text] = list;
    selectorCacheSize++;
    return list;
  }

  function matchesCompound(node, part) {
    if (part.tag !== null && part.tag !== '*' && node.localName !== part.tag) return false;
    var i;
    for (i = 0; i < part.ids.length; i++) if (getAttr(node, 'id') !== part.ids[i]) return false;
    if (part.classes.length > 0) {
      var tokens = classTokens(node);
      for (i = 0; i < part.classes.length; i++) if (tokens.indexOf(part.classes[i]) < 0) return false;
    }
    for (i = 0; i < part.attrs.length; i++) {
      var value = getAttr(node, part.attrs[i].name);
      if (value === null || (part.attrs[i].value !== null && value !== part.attrs[i].value)) return false;
    }
    return true;
  }
  // Right to left, backtracking over the descendant combinator.
  function matchesFrom(object, selector, index) {
    if (!matchesCompound(nodes.get(object), selector.compounds[index])) return false;
    if (index === 0) return true;
    var parent = elementParent(object);
    if (selector.combinators[index] === '>') return parent !== null && matchesFrom(parent, selector, index - 1);
    for (; parent !== null; parent = elementParent(parent)) {
      if (matchesFrom(parent, selector, index - 1)) return true;
    }
    return false;
  }
  function matchesList(object, list) {
    for (var i = 0; i < list.length; i++) {
      if (matchesFrom(object, list[i], list[i].compounds.length - 1)) return true;
    }
    return false;
  }

  // querySelector / querySelectorAll / getElementsBy*, shared by Document and
  // Element.
  function parentNodeMembers(iface) {
    return {
      children: { get: function () {
        var node = self(this);
        if (node.children === null) node.children = collection(HTMLCollection.prototype, elementKids(node));
        return node.children;
      } },
      firstElementChild: { get: function () { var kids = elementKids(self(this)); return kids.length ? kids[0] : null; } },
      lastElementChild: { get: function () {
        var kids = elementKids(self(this));
        return kids.length ? kids[kids.length - 1] : null;
      } },
      childElementCount: { get: function () { return elementKids(self(this)).length; } },
      querySelector: function (selectors) {
        self(this);
        requireArgs(arguments.length, 1, 'querySelector', iface);
        var list = parseSelectors(selectors, 'querySelector', iface), found = null;
        walk(this, function (object) {
          if (!matchesList(object, list)) return false;
          found = object;
          return true;
        });
        return found;
      },
      querySelectorAll: function (selectors) {
        self(this);
        requireArgs(arguments.length, 1, 'querySelectorAll', iface);
        var list = parseSelectors(selectors, 'querySelectorAll', iface), found = [];
        walk(this, function (object) { if (matchesList(object, list)) found.push(object); });
        return collection(NodeList.prototype, found);
      },
      getElementsByTagName: function (name) {
        self(this);
        requireArgs(arguments.length, 1, 'getElementsByTagName', iface);
        // Case-insensitive for HTML elements, exact for XML ones.
        var wanted = String(name), lower = wanted.toLowerCase(), found = [];
        walk(this, function (object, node) {
          if (wanted === '*' ||
              (node.namespace === HTML_NAMESPACE ? node.localName === lower : qualifiedNameOf(node) === wanted)) {
            found.push(object);
          }
        });
        return collection(HTMLCollection.prototype, found);
      },
      getElementsByClassName: function (names) {
        self(this);
        requireArgs(arguments.length, 1, 'getElementsByClassName', iface);
        var wanted = String(names).split(/[\t\n\f\r ]+/).filter(function (t) { return t !== ''; }), found = [];
        if (wanted.length > 0) {
          walk(this, function (object, node) {
            var tokens = classTokens(node);
            if (wanted.every(function (t) { return tokens.indexOf(t) >= 0; })) found.push(object);
          });
        }
        return collection(HTMLCollection.prototype, found);
      }
    };
  }

  // --- Element and HTMLElement ------------------------------------------------

  function Element() { throw new TypeError("Failed to construct 'Element': Illegal constructor"); }
  inherit(Element, Node);
  define(Element.prototype, parentNodeMembers('Element'));
  define(Element.prototype, {
    // An HTML element's tagName is upper-cased; any other namespace's is the
    // qualified name as given.
    tagName: { get: function () { return nameOf(self(this)); } },
    prefix: { get: function () { return self(this).prefix; } },
    localName: { get: function () { return self(this).localName; } },
    namespaceURI: { get: function () { return self(this).namespace; } },
    id: {
      get: function () { var value = getAttr(self(this), 'id'); return value === null ? '' : value; },
      set: function (value) { setAttr(self(this), 'id', String(value)); }
    },
    className: {
      get: function () { var value = getAttr(self(this), 'class'); return value === null ? '' : value; },
      set: function (value) { setAttr(self(this), 'class', String(value)); }
    },
    classList: {
      get: function () {
        var node = self(this);
        if (node.classList === null) {
          node.classList = collection(DOMTokenList.prototype, classTokens(node));
          tokenOwners.set(node.classList, { node: node, name: 'class' });
        }
        return node.classList;
      },
      set: function (value) { this.classList.value = value; }
    },
    previousElementSibling: { get: function () { return sibling(this, -1, true); } },
    nextElementSibling: { get: function () { return sibling(this, 1, true); } },
    // Attribute names fold to lower case on an HTML element only; XML's are
    // case-sensitive (a bitmap font's `lineHeight`, `xoffset`).
    getAttribute: function (name) {
      var node = self(this);
      requireArgs(arguments.length, 1, 'getAttribute', 'Element');
      return getAttr(node, foldName(node, name));
    },
    setAttribute: function (name, value) {
      var node = self(this);
      requireArgs(arguments.length, 2, 'setAttribute', 'Element');
      attributeName(name, 'setAttribute');
      setAttr(node, foldName(node, name), String(value));
    },
    removeAttribute: function (name) {
      var node = self(this);
      requireArgs(arguments.length, 1, 'removeAttribute', 'Element');
      removeAttr(node, foldName(node, name));
    },
    hasAttribute: function (name) {
      var node = self(this);
      requireArgs(arguments.length, 1, 'hasAttribute', 'Element');
      return attrIndex(node, foldName(node, name)) >= 0;
    },
    hasAttributes: function () { return self(this).attrs.length > 0; },
    getAttributeNames: function () { return self(this).attrs.map(function (pair) { return pair[0]; }); },
    remove: function () {
      self(this);
      detach(this);
    },
    matches: function (selectors) {
      self(this);
      requireArgs(arguments.length, 1, 'matches', 'Element');
      return matchesList(this, parseSelectors(selectors, 'matches', 'Element'));
    },
    closest: function (selectors) {
      self(this);
      requireArgs(arguments.length, 1, 'closest', 'Element');
      var list = parseSelectors(selectors, 'closest', 'Element');
      for (var at = this; at !== null; at = elementParent(at)) {
        if (matchesList(at, list)) return at;
      }
      return null;
    },
    // No layout, so an element that does not paint occupies nothing -- except
    // the document's <html> and <body>, which are the screen: engines size a
    // "fill the parent" game from them (Phaser's RESIZE scale mode reads
    // body.getBoundingClientRect). A canvas overrides this with its own size;
    // Blits reads it to map mouse coordinates.
    getBoundingClientRect: function () {
      return fillsScreen(self(this)) ? rect(drawableWidth(), drawableHeight()) : rect(0, 0);
    },
    clientWidth: { get: function () { return fillsScreen(self(this)) ? drawableWidth() : 0; } },
    clientHeight: { get: function () { return fillsScreen(self(this)) ? drawableHeight() : 0; } },
    clientLeft: { get: function () { self(this); return 0; } },
    clientTop: { get: function () { self(this); return 0; } }
  });
  function fillsScreen(node) {
    if (node.namespace !== HTML_NAMESPACE || (node.localName !== 'html' && node.localName !== 'body')) return false;
    var html = documentElementOf();
    return node.object === html || (node.parent === html && html !== null);
  }
  global.Element = Element;

  function HTMLElement() { throw new TypeError("Failed to construct 'HTMLElement': Illegal constructor"); }
  inherit(HTMLElement, Element);
  defineEventHandlers(HTMLElement.prototype, GLOBAL_EVENT_HANDLERS);
  define(HTMLElement.prototype, {
    // [PutForwards=cssText], as in a browser: `el.style = 'left: 0'` sets it.
    style: {
      get: function () { return styleOf(self(this)); },
      set: function (value) { styleOf(self(this)).cssText = value; }
    }
  });
  global.HTMLElement = HTMLElement;

  function rect(width, height) {
    return { x: 0, y: 0, left: 0, top: 0, width: width, height: height, right: width, bottom: height };
  }

  // A valid element name: an ASCII letter and then no whitespace, NUL, '/' or
  // '>', or the DOM's wider rule for names that start elsewhere.
  function elementName(tagName) {
    var text = String(tagName);
    if (!/^[a-zA-Z][^\t\n\f\r \u0000\/>]*$/.test(text) &&
        !/^[:_-￿][:_\-.0-9a-zA-Z-￿]*$/.test(text)) {
      throw domError("Failed to execute 'createElement' on 'Document': The tag name provided ('" + text +
                     "') is not a valid name.", 'InvalidCharacterError');
    }
    return text.toLowerCase();
  }

  function newElement(localName, proto) {
    var element = Object.create(proto || HTMLElement.prototype);
    register(element, ELEMENT_NODE, localName);
    return element;
  }

  // --- CSSStyleDeclaration ----------------------------------------------------
  //
  // Declarations are stored and read back on every element: `cssText`,
  // `setProperty`/`getPropertyValue`/`removeProperty` and camelCase properties.
  // There is no cascade and no layout, so on an element that does not paint a
  // declaration is only data.
  //
  // On a backed element -- a canvas, a <video> or an <iframe> -- the
  // Architecture.md 3.1 subset (position, left/top/right/bottom, width/height,
  // z-index, opacity, display, transform: translate|scale) is parsed as a
  // browser parses it: a value outside the subset is not applied and says so
  // once, and an accepted one reads back normalised ('0' is '0px'). The parsed
  // values are what the compositor maps onto a layer rect. On a canvas they
  // also decide what that canvas is: one the subset has placed before it asks
  // for a context becomes a layer at that rect, and the one canvas that took
  // the page's frame cannot be moved at all -- it is the drawable, and it says
  // so once.

  var styleStates = new WeakMap();

  // Lookup tables keyed by bundle-supplied names, so without a prototype: a
  // property called `constructor` or `__proto__` must miss, not find
  // Object.prototype's.
  function table(entries) {
    var out = Object.create(null);
    Object.keys(entries).forEach(function (key) { out[key] = entries[key]; });
    return out;
  }
  var GLOBAL_KEYWORDS = table({ inherit: 1, initial: 1, unset: 1, revert: 1, 'revert-layer': 1 });
  var LENGTH_UNITS = table({ px: 1, em: 1, rem: 1, vw: 1, vh: 1, vmin: 1, vmax: 1, pt: 1, pc: 1, cm: 1, mm: 1,
                             'in': 1, q: 1, ch: 1, ex: 1 });

  // CSS <number>: a digit after any decimal point, an exponent in either case,
  // and finite -- `1.` and `1e400` are not numbers a stylesheet can hold.
  var NUMBER = '[+-]?(?:\\d+(?:\\.\\d+)?|\\.\\d+)(?:[eE][+-]?\\d+)?';
  function finite(value) { return isFinite(value) ? value : null; }
  function numberToken(text) {
    return new RegExp('^' + NUMBER + '$').test(text) ? finite(Number(text)) : null;
  }
  function lengthToken(text, negative) {
    var m = new RegExp('^(' + NUMBER + ')(%|[a-zA-Z]+)?$').exec(text);
    if (!m || finite(Number(m[1])) === null) return null;
    var value = Number(m[1]), unit = m[2] ? m[2].toLowerCase() : '';
    if (!negative && value < 0) return null;
    if (unit === '') return value === 0 ? { value: 0, unit: 'px' } : null;
    return unit === '%' || LENGTH_UNITS[unit] === 1 ? { value: value, unit: unit } : null;
  }
  function showNumber(value) { return String(value === 0 ? 0 : value); }
  function showLength(length) { return showNumber(length.value) + length.unit; }

  function offsetValue(text) {
    if (/^auto$/i.test(text)) return { text: 'auto', auto: true };
    var length = lengthToken(text, true);
    return length && { text: showLength(length), length: length };
  }
  function sizeValue(text) {
    if (/^auto$/i.test(text)) return { text: 'auto', auto: true };
    var length = lengthToken(text, false);
    return length && { text: showLength(length), length: length };
  }
  var TRANSFORM_FUNCTIONS = table({ translate: 'translate', translatex: 'translateX', translatey: 'translateY',
                                    scale: 'scale', scalex: 'scaleX', scaley: 'scaleY' });
  function transformValue(text) {
    if (/^none$/i.test(text)) return { text: 'none', functions: [] };
    var functions = [], re = /\s*([a-zA-Z]+)\(\s*([^()]*?)\s*\)\s*/g, m, consumed = 0;
    while ((m = re.exec(text)) !== null && m.index === consumed) {
      consumed = re.lastIndex;
      var name = TRANSFORM_FUNCTIONS[m[1].toLowerCase()];
      var args = m[2] === '' ? [] : m[2].split(/\s*,\s*/);
      if (!name || args.length < 1 || args.length > (name === 'translate' || name === 'scale' ? 2 : 1)) return null;
      var parsed = [];
      for (var i = 0; i < args.length; i++) {
        var arg;
        if (name.indexOf('translate') === 0) {
          arg = lengthToken(args[i], true);
          if (arg === null) return null;
        } else {
          var percent = /^(.*)%$/.exec(args[i]);
          arg = percent ? numberToken(percent[1]) : numberToken(args[i]);
          if (arg === null) return null;
          if (percent) arg /= 100;
        }
        parsed.push(arg);
      }
      functions.push({ name: name, args: parsed });
    }
    if (functions.length === 0 || consumed !== text.length) return null;
    return {
      text: functions.map(function (f) {
        return f.name + '(' + f.args.map(function (a) { return typeof a === 'number' ? showNumber(a) : showLength(a); })
                                   .join(', ') + ')';
      }).join(' '),
      functions: functions
    };
  }
  // Display keywords a browser accepts: one, or an outer and an inner.
  var DISPLAY_KEYWORDS = /^(none|contents|block|inline|inline-block|run-in|flow|flow-root|flex|inline-flex|grid|inline-grid|table|inline-table|table-row-group|table-header-group|table-footer-group|table-row|table-cell|table-column-group|table-column|table-caption|list-item|ruby|ruby-base|ruby-text|ruby-base-container|ruby-text-container|(block|inline|run-in) (flow|flow-root|table|flex|grid|ruby))$/;
  var SUBSET = table({
    position: function (text) {
      var value = text.toLowerCase();
      return /^(static|relative|absolute|fixed|sticky)$/.test(value) ? { text: value } : null;
    },
    left: offsetValue, top: offsetValue, right: offsetValue, bottom: offsetValue,
    width: sizeValue, height: sizeValue,
    'z-index': function (text) {
      if (/^auto$/i.test(text)) return { text: 'auto' };
      return /^[+-]?\d+$/.test(text) && finite(parseInt(text, 10)) !== null
        ? { text: showNumber(parseInt(text, 10)) } : null;
    },
    opacity: function (text) {
      var percent = /^(.*)%$/.exec(text);
      var value = numberToken(percent ? percent[1] : text);
      if (value === null) return null;
      if (percent) value /= 100;
      return { text: showNumber(value), opacity: value };
    },
    display: function (text) {
      var value = text.toLowerCase().replace(/\s+/g, ' ');
      return DISPLAY_KEYWORDS.test(value) ? { text: value } : null;
    },
    transform: transformValue
  });

  function parseSubset(name, text) {
    if (GLOBAL_KEYWORDS[text.toLowerCase()] === 1) return { text: text.toLowerCase(), global: true };
    return SUBSET[name](text);
  }

  // Would this declaration move, resize or hide the element away from being the
  // whole drawable? That is what turns a canvas into a layer of its own when it
  // asks for a context, and what the one canvas that *is* the frame cannot
  // honour. A size within half a pixel of the drawable is the drawable:
  // Lightning computes its CSS size in floating point and writes `1280.0000064px`.
  function fullLength(length, pixels, viewportUnit) {
    return (length.unit === '%' || length.unit === viewportUnit) ? length.value === 100
                                                                 : length.unit === 'px' && Math.abs(length.value - pixels) < 0.5;
  }
  function divergence(name, parsed) {
    if (parsed.global) return null;
    switch (name) {
      case 'left': case 'top': case 'right': case 'bottom':
        return parsed.auto || parsed.length.value === 0 ? null : 'position';
      case 'width':
        return parsed.auto || fullLength(parsed.length, drawableWidth(), 'vw') ? null : 'size';
      case 'height':
        return parsed.auto || fullLength(parsed.length, drawableHeight(), 'vh') ? null : 'size';
      case 'transform':
        return parsed.functions.some(function (f) {
          return f.name.indexOf('translate') === 0
            ? f.args.some(function (a) { return a.value !== 0; })
            : f.args.some(function (a) { return a !== 1; });
        }) ? 'position' : null;
      case 'display':
        return parsed.text === 'none' ? 'visibility' : null;
      case 'opacity':
        return parsed.opacity < 1 ? 'visibility' : null;
    }
    return null;
  }

  function backedTag(state) { return '<' + state.node.localName + '>'; }

  function propertyName(name) {
    var text = String(name);
    if (text.indexOf('--') === 0) return text.length > 2 ? text : null;
    text = text.toLowerCase();
    return /^-?[a-z_][a-z0-9_-]*$/.test(text) ? text : null;
  }

  function findDeclaration(state, name) {
    for (var i = 0; i < state.declarations.length; i++) if (state.declarations[i].name === name) return i;
    return -1;
  }

  function setDeclaration(state, name, rawValue, important) {
    var value = String(rawValue).trim();
    if (value === '') return removeDeclaration(state, name);
    var existing = findDeclaration(state, name);
    if (state.node.backed && SUBSET[name]) {
      var parsed = parseSubset(name, value);
      if (parsed === null) {
        announce('style-value:' + name,
          'ScreenKit: ' + backedTag(state) + '.style ' + name + ': ' + value + ' was not applied -- it is outside ' +
          'the CSS subset (Architecture.md 3.1), so ' + name + ' keeps ' +
          (existing < 0 ? 'no value' : state.declarations[existing].value) + '.');
        return false;
      }
      value = parsed.text;
      state.layer[name] = parsed;
      // A <video>'s rect is its plane and an <iframe>'s is its instance's
      // layer; a canvas's is its own layer, unless it is the canvas that *is*
      // the page's frame -- the one thing CSS here cannot move, because moving
      // it would mean resizing the window.
      if (state.node.localName === 'canvas') canvasDeclarationApplied(state, name, parsed, value);
      if (mediaElementCount > 0) mediaLayerChanged(state.node);
      if (frameElementCount > 0) frameLayerChanged(state.node);
    }
    var declaration = { name: name, value: value, important: !!important };
    if (existing < 0) state.declarations.push(declaration);
    else state.declarations[existing] = declaration;
    return true;
  }

  function removeDeclaration(state, name) {
    var at = findDeclaration(state, name);
    if (at < 0) return false;
    state.declarations.splice(at, 1);
    delete state.layer[name];
    if (mediaElementCount > 0) mediaLayerChanged(state.node);
    if (frameElementCount > 0) frameLayerChanged(state.node);
    if (canvasLayerCount > 0) canvasLayerChanged(state.node);
    return true;
  }

  // `a: b; c: d !important` -> [[name, value, important]], split on the
  // semicolons that are not inside parentheses or quotes.
  function splitDeclarations(text) {
    var out = [], depth = 0, quote = null, start = 0;
    function chunk(end) {
      var piece = text.slice(start, end), colon = piece.indexOf(':');
      if (colon < 0) return;
      var value = piece.slice(colon + 1).trim(), important = /!\s*important\s*$/i.exec(value);
      if (important) value = value.slice(0, important.index).trim();
      out.push([piece.slice(0, colon).trim(), value, !!important]);
    }
    for (var i = 0; i < text.length; i++) {
      var c = text.charAt(i);
      if (quote !== null) {
        if (c === '\\') i++;
        else if (c === quote) quote = null;
      } else if (c === '"' || c === "'") {
        quote = c;
      } else if (c === '(') {
        depth++;
      } else if (c === ')' && depth > 0) {
        depth--;
      } else if (c === ';' && depth === 0) {
        chunk(i);
        start = i + 1;
      }
    }
    chunk(text.length);
    return out;
  }

  function serialize(state) {
    return state.declarations.map(function (d) {
      return d.name + ': ' + d.value + (d.important ? ' !important' : '') + ';';
    }).join(' ');
  }

  function stateOf(style) {
    var state = styleStates.get(style);
    if (!state) throw new TypeError('Illegal invocation');
    return state;
  }

  // Keep the element's style attribute in step, as a browser does.
  function styleChanged(state) {
    if (reflectingStyle) return;
    reflectingStyle = true;
    try {
      setAttr(state.node, 'style', serialize(state));
    } finally {
      reflectingStyle = false;
    }
  }

  function replaceDeclarations(style, text) {
    var state = stateOf(style);
    state.declarations = [];
    state.layer = Object.create(null);
    if (mediaElementCount > 0) mediaLayerChanged(state.node);
    if (frameElementCount > 0) frameLayerChanged(state.node);
    if (canvasLayerCount > 0) canvasLayerChanged(state.node);
    splitDeclarations(String(text)).forEach(function (d) {
      var name = propertyName(d[0]);
      if (name !== null) setDeclaration(state, name, d[1], d[2]);
    });
  }

  function CSSStyleDeclaration() { throw new TypeError("Failed to construct 'CSSStyleDeclaration': Illegal constructor"); }
  define(CSSStyleDeclaration.prototype, {
    cssText: {
      get: function () { return serialize(stateOf(this)); },
      set: function (value) {
        var state = stateOf(this);
        replaceDeclarations(this, value === null ? '' : value);
        styleChanged(state);
      }
    },
    length: { get: function () { return stateOf(this).declarations.length; } },
    parentRule: { get: function () { stateOf(this); return null; } },
    item: function (index) {
      var declarations = stateOf(this).declarations, i = Number(index) >>> 0;
      return i < declarations.length ? declarations[i].name : '';
    },
    getPropertyValue: function (name) {
      var state = stateOf(this), at = findDeclaration(state, propertyName(name));
      return at < 0 ? '' : state.declarations[at].value;
    },
    getPropertyPriority: function (name) {
      var state = stateOf(this), at = findDeclaration(state, propertyName(name));
      return at >= 0 && state.declarations[at].important ? 'important' : '';
    },
    setProperty: function (name, value, priority) {
      var state = stateOf(this), key = propertyName(name);
      var important = priority === undefined || priority === null ? '' : String(priority).toLowerCase();
      if (key === null || (important !== '' && important !== 'important')) return;
      if (setDeclaration(state, key, value === null || value === undefined ? '' : value, important === 'important')) {
        styleChanged(state);
      }
    },
    removeProperty: function (name) {
      var state = stateOf(this), key = propertyName(name), at = findDeclaration(state, key);
      if (at < 0) return '';
      var old = state.declarations[at].value;
      removeDeclaration(state, key);
      styleChanged(state);
      return old;
    }
  });
  // camelCase accessors for the properties bundles set that way. Anything else
  // assigned as `style.fooBar` is an ordinary property, as it is in a browser
  // for a name that is not a CSS property.
  ['position', 'left', 'top', 'right', 'bottom', 'inset', 'width', 'height', 'min-width', 'min-height',
   'max-width', 'max-height', 'z-index', 'opacity', 'display', 'visibility', 'transform', 'transform-origin',
   'overflow', 'overflow-x', 'overflow-y', 'margin', 'margin-top', 'margin-right', 'margin-bottom',
   'margin-left', 'padding', 'padding-top', 'padding-right', 'padding-bottom', 'padding-left', 'border',
   'border-radius', 'box-sizing', 'background', 'background-color', 'background-image', 'color', 'font',
   'font-family', 'font-size', 'font-weight', 'font-style', 'line-height', 'letter-spacing', 'text-align',
   'white-space', 'pointer-events', 'cursor', 'user-select', 'outline', 'transition', 'animation', 'flex',
   'filter', 'object-fit', 'will-change', 'content', 'float'].forEach(function (name) {
    var camel = name === 'float' ? 'cssFloat' : name.replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); });
    Object.defineProperty(CSSStyleDeclaration.prototype, camel, {
      get: function () { return this.getPropertyValue(name); },
      set: function (value) { this.setProperty(name, value === null ? '' : value); },
      enumerable: true,
      configurable: true
    });
  });
  global.CSSStyleDeclaration = CSSStyleDeclaration;

  function styleOf(node) {
    if (node.style === null) {
      node.style = Object.create(CSSStyleDeclaration.prototype);
      styleStates.set(node.style, { node: node, declarations: [], layer: Object.create(null) });
      var attribute = getAttr(node, 'style');
      if (attribute !== null) {
        reflectingStyle = true;
        try {
          replaceDeclarations(node.style, attribute);
        } finally {
          reflectingStyle = false;
        }
      }
    }
    return node.style;
  }

  // --- Text ----------------------------------------------------------------------
  // Text nodes carry character data and nothing else. They exist for
  // `textContent`, `createTextNode` and XML documents from DOMParser; a text node
  // in the page never renders, like every element but the three that paint.

  function CharacterData() { throw new TypeError("Failed to construct 'CharacterData': Illegal constructor"); }
  inherit(CharacterData, Node);
  define(CharacterData.prototype, {
    data: {
      get: function () { return textNode(this).data; },
      set: function (value) { setTextData(textNode(this), value === null ? '' : String(value)); }
    },
    length: { get: function () { return textNode(this).data.length; } }
  });
  global.CharacterData = CharacterData;

  function Text(data) {
    if (!(this instanceof Text)) throw new TypeError("Failed to construct 'Text': Please use the 'new' operator.");
    return makeText(data === undefined ? '' : String(data), null);
  }
  inherit(Text, CharacterData);
  global.Text = Text;

  function textNode(value) {
    var node = self(value);
    if (node.type !== TEXT_NODE) throw new TypeError('Illegal invocation');
    return node;
  }
  function makeText(data, ownerDocument) {
    var text = Object.create(Text.prototype);
    var node = register(text, TEXT_NODE, null);
    node.data = data;
    node.ownerDocument = ownerDocument;
    return text;
  }

  // --- Document ---------------------------------------------------------------

  function Document() {
    throw new TypeError("Failed to construct 'Document': this runtime has exactly one document -- use `document`.");
  }
  inherit(Document, Node);
  define(Document.prototype, parentNodeMembers('Document'));
  defineEventHandlers(Document.prototype, GLOBAL_EVENT_HANDLERS.concat(['readystatechange', 'visibilitychange']));

  // -------------------------------------------------------------------------
  // WebGL1 extensions
  //
  // The native getExtension answers whether an extension exists and hands back
  // an empty object; a WebGL1 extension is methods and constants, and a WebGL1
  // engine calls them. On Apple the context is WebGL2 and nothing asks, but a
  // GLES2 GPU -- a Raspberry Pi 3's VideoCore IV -- gives a WebGL1 context: Pixi
  // draws through OES_vertex_array_object there, and Phaser will not start
  // without ANGLE_instanced_arrays. The methods forward to the WebGL2 natives,
  // which make the same GL calls (on GLES2 they reach the OES/EXT entry points).
  //
  // A GPU with no instancing at all -- the Pi 3 again -- gets an emulated
  // ANGLE_instanced_arrays: an instanced draw repeats a plain draw, which is
  // exact while no attribute has a divisor. One that has a divisor cannot be
  // drawn that way; it draws nothing and says so once. Phaser's sprite batches
  // never set one.
  // -------------------------------------------------------------------------

  var EXTENSION_SHAPES = {
    OES_vertex_array_object: {
      constants: { VERTEX_ARRAY_BINDING_OES: 0x85B5 },
      methods: { createVertexArrayOES: 'createVertexArray', deleteVertexArrayOES: 'deleteVertexArray',
                 isVertexArrayOES: 'isVertexArray', bindVertexArrayOES: 'bindVertexArray' }
    },
    ANGLE_instanced_arrays: {
      constants: { VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE: 0x88FE },
      methods: { drawArraysInstancedANGLE: 'drawArraysInstanced', drawElementsInstancedANGLE: 'drawElementsInstanced',
                 vertexAttribDivisorANGLE: 'vertexAttribDivisor' }
    },
    WEBGL_draw_buffers: {
      constants: (function () {
        var c = { MAX_COLOR_ATTACHMENTS_WEBGL: 0x8CDF, MAX_DRAW_BUFFERS_WEBGL: 0x8824 };
        for (var i = 0; i < 16; i++) {
          c['COLOR_ATTACHMENT' + i + '_WEBGL'] = 0x8CE0 + i;
          c['DRAW_BUFFER' + i + '_WEBGL'] = 0x8825 + i;
        }
        return c;
      })(),
      methods: { drawBuffersWEBGL: 'drawBuffers' }
    },
    EXT_blend_minmax: { constants: { MIN_EXT: 0x8007, MAX_EXT: 0x8008 } },
    OES_standard_derivatives: { constants: { FRAGMENT_SHADER_DERIVATIVE_HINT_OES: 0x8B8B } },
    OES_texture_half_float: { constants: { HALF_FLOAT_OES: 0x8D61 } },
    WEBGL_depth_texture: { constants: { UNSIGNED_INT_24_8_WEBGL: 0x84FA } },
    WEBGL_compressed_texture_s3tc: {
      constants: { COMPRESSED_RGB_S3TC_DXT1_EXT: 0x83F0, COMPRESSED_RGBA_S3TC_DXT1_EXT: 0x83F1,
                   COMPRESSED_RGBA_S3TC_DXT3_EXT: 0x83F2, COMPRESSED_RGBA_S3TC_DXT5_EXT: 0x83F3 }
    },
    WEBGL_compressed_texture_etc1: { constants: { COMPRESSED_RGB_ETC1_WEBGL: 0x8D64 } }
  };

  function webgl2Native(name) {
    var proto = typeof global.WebGL2RenderingContext === 'function' ? global.WebGL2RenderingContext.prototype : null;
    return proto !== null && typeof proto[name] === 'function' ? proto[name] : null;
  }

  function emulatedInstancing(context) {
    var divisors = [];
    function withoutDivisors(what, draw, instances) {
      for (var i = 0; i < divisors.length; i++) {
        if (divisors[i]) {
          announce('webgl:instancing',
            'ScreenKit: this GPU has no instancing, and ' + what + ' with a per-instance attribute (a divisor) ' +
            'cannot be emulated -- the draw is skipped. Draws without divisors are repeated per instance.');
          return;
        }
      }
      for (var n = 0; n < instances; n++) draw();
    }
    return {
      VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE: 0x88FE,
      vertexAttribDivisorANGLE: function (index, divisor) { divisors[index >>> 0] = divisor >>> 0; },
      drawArraysInstancedANGLE: function (mode, first, count, instances) {
        withoutDivisors('drawArraysInstanced', function () { context.drawArrays(mode, first, count); }, instances);
      },
      drawElementsInstancedANGLE: function (mode, count, type, offset, instances) {
        withoutDivisors('drawElementsInstanced', function () { context.drawElements(mode, count, type, offset); }, instances);
      }
    };
  }

  function installExtensionObjects(proto) {
    if (!proto || typeof proto.getExtension !== 'function' || proto.getExtension.__screenkit) return;
    var nativeGetExtension = proto.getExtension;
    var nativeSupported = proto.getSupportedExtensions;
    var cache = new WeakMap();
    function nativeHas(context, name) {
      return (nativeSupported.call(context) || []).indexOf(name) >= 0;
    }
    function emulatesInstancing(context) {
      return !(context instanceof global.WebGL2RenderingContext) && !nativeHas(context, 'ANGLE_instanced_arrays');
    }
    proto.getExtension = function (name) {
      var key = String(name);
      var made = cache.get(this);
      if (!made) { made = Object.create(null); cache.set(this, made); }
      if (key in made) return made[key];
      var extension = null;
      if (key === 'ANGLE_instanced_arrays' && emulatesInstancing(this)) {
        extension = emulatedInstancing(this);
      } else {
        extension = nativeGetExtension.call(this, key);
        var shape = extension !== null && typeof extension === 'object' ? EXTENSION_SHAPES[key] : undefined;
        if (shape) {
          var context = this;
          Object.keys(shape.constants || {}).forEach(function (c) { extension[c] = shape.constants[c]; });
          Object.keys(shape.methods || {}).forEach(function (method) {
            var native = webgl2Native(shape.methods[method]);
            if (native !== null) extension[method] = function () { return native.apply(context, arguments); };
          });
        }
      }
      made[key] = extension;
      return extension;
    };
    proto.getExtension.__screenkit = true;
    proto.getSupportedExtensions = function () {
      var list = nativeSupported.call(this) || [];
      if (emulatesInstancing(this) && list.indexOf('ANGLE_instanced_arrays') < 0) list = list.concat('ANGLE_instanced_arrays');
      return list;
    };
  }

  // The vendored bindings put every constant on both interfaces, but a browser's
  // WebGLRenderingContext has none of WebGL2's -- and engines read one to decide
  // what the context can do: Pixi takes ACTIVE_UNIFORM_BLOCKS to mean uniform
  // blocks exist and calls getActiveUniformBlockName, which a WebGL1 context does
  // not have. These are the WebGL2 IDL's constants that WebGL 1.0's lacks.
  var WEBGL2_ONLY_CONSTANTS = (
    'ACTIVE_UNIFORM_BLOCKS ALREADY_SIGNALED ANY_SAMPLES_PASSED ANY_SAMPLES_PASSED_CONSERVATIVE COLOR ' +
    'COLOR_ATTACHMENT1 COLOR_ATTACHMENT10 COLOR_ATTACHMENT11 COLOR_ATTACHMENT12 COLOR_ATTACHMENT13 ' +
    'COLOR_ATTACHMENT14 COLOR_ATTACHMENT15 COLOR_ATTACHMENT2 COLOR_ATTACHMENT3 COLOR_ATTACHMENT4 ' +
    'COLOR_ATTACHMENT5 COLOR_ATTACHMENT6 COLOR_ATTACHMENT7 COLOR_ATTACHMENT8 COLOR_ATTACHMENT9 ' +
    'COMPARE_REF_TO_TEXTURE CONDITION_SATISFIED COPY_READ_BUFFER COPY_READ_BUFFER_BINDING COPY_WRITE_BUFFER ' +
    'COPY_WRITE_BUFFER_BINDING CURRENT_QUERY DEPTH DEPTH24_STENCIL8 DEPTH32F_STENCIL8 DEPTH_COMPONENT24 ' +
    'DEPTH_COMPONENT32F DRAW_BUFFER0 DRAW_BUFFER1 DRAW_BUFFER10 DRAW_BUFFER11 DRAW_BUFFER12 DRAW_BUFFER13 ' +
    'DRAW_BUFFER14 DRAW_BUFFER15 DRAW_BUFFER2 DRAW_BUFFER3 DRAW_BUFFER4 DRAW_BUFFER5 DRAW_BUFFER6 DRAW_BUFFER7 ' +
    'DRAW_BUFFER8 DRAW_BUFFER9 DRAW_FRAMEBUFFER DRAW_FRAMEBUFFER_BINDING DYNAMIC_COPY DYNAMIC_READ ' +
    'FLOAT_32_UNSIGNED_INT_24_8_REV FRAGMENT_SHADER_DERIVATIVE_HINT FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE ' +
    'FRAMEBUFFER_ATTACHMENT_BLUE_SIZE FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE ' +
    'FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE FRAMEBUFFER_ATTACHMENT_GREEN_SIZE FRAMEBUFFER_ATTACHMENT_RED_SIZE ' +
    'FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER FRAMEBUFFER_DEFAULT ' +
    'FRAMEBUFFER_INCOMPLETE_MULTISAMPLE HALF_FLOAT INTERLEAVED_ATTRIBS INT_2_10_10_10_REV INT_SAMPLER_2D ' +
    'INT_SAMPLER_2D_ARRAY INT_SAMPLER_3D INT_SAMPLER_CUBE INVALID_INDEX MAX MAX_3D_TEXTURE_SIZE ' +
    'MAX_ARRAY_TEXTURE_LAYERS MAX_CLIENT_WAIT_TIMEOUT_WEBGL MAX_COLOR_ATTACHMENTS ' +
    'MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS MAX_COMBINED_UNIFORM_BLOCKS MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS ' +
    'MAX_DRAW_BUFFERS MAX_ELEMENTS_INDICES MAX_ELEMENTS_VERTICES MAX_ELEMENT_INDEX MAX_FRAGMENT_INPUT_COMPONENTS ' +
    'MAX_FRAGMENT_UNIFORM_BLOCKS MAX_FRAGMENT_UNIFORM_COMPONENTS MAX_PROGRAM_TEXEL_OFFSET MAX_SAMPLES ' +
    'MAX_SERVER_WAIT_TIMEOUT MAX_TEXTURE_LOD_BIAS MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS ' +
    'MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS MAX_UNIFORM_BLOCK_SIZE ' +
    'MAX_UNIFORM_BUFFER_BINDINGS MAX_VARYING_COMPONENTS MAX_VERTEX_OUTPUT_COMPONENTS MAX_VERTEX_UNIFORM_BLOCKS ' +
    'MAX_VERTEX_UNIFORM_COMPONENTS MIN MIN_PROGRAM_TEXEL_OFFSET OBJECT_TYPE PACK_ROW_LENGTH PACK_SKIP_PIXELS ' +
    'PACK_SKIP_ROWS PIXEL_PACK_BUFFER PIXEL_PACK_BUFFER_BINDING PIXEL_UNPACK_BUFFER PIXEL_UNPACK_BUFFER_BINDING ' +
    'QUERY_RESULT QUERY_RESULT_AVAILABLE R11F_G11F_B10F R16F R16I R16UI R32F R32I R32UI R8 R8I R8UI R8_SNORM ' +
    'RASTERIZER_DISCARD READ_BUFFER READ_FRAMEBUFFER READ_FRAMEBUFFER_BINDING RED RED_INTEGER ' +
    'RENDERBUFFER_SAMPLES RG RG16F RG16I RG16UI RG32F RG32I RG32UI RG8 RG8I RG8UI RG8_SNORM RGB10_A2 RGB10_A2UI ' +
    'RGB16F RGB16I RGB16UI RGB32F RGB32I RGB32UI RGB8 RGB8I RGB8UI RGB8_SNORM RGB9_E5 RGBA16F RGBA16I RGBA16UI ' +
    'RGBA32F RGBA32I RGBA32UI RGBA8I RGBA8UI RGBA8_SNORM RGBA_INTEGER RGB_INTEGER RG_INTEGER SAMPLER_2D_ARRAY ' +
    'SAMPLER_2D_ARRAY_SHADOW SAMPLER_2D_SHADOW SAMPLER_3D SAMPLER_BINDING SAMPLER_CUBE_SHADOW SEPARATE_ATTRIBS ' +
    'SIGNALED SIGNED_NORMALIZED SRGB SRGB8 SRGB8_ALPHA8 STATIC_COPY STATIC_READ STENCIL STREAM_COPY STREAM_READ ' +
    'SYNC_CONDITION SYNC_FENCE SYNC_FLAGS SYNC_FLUSH_COMMANDS_BIT SYNC_GPU_COMMANDS_COMPLETE SYNC_STATUS ' +
    'TEXTURE_2D_ARRAY TEXTURE_3D TEXTURE_BASE_LEVEL TEXTURE_BINDING_2D_ARRAY TEXTURE_BINDING_3D ' +
    'TEXTURE_COMPARE_FUNC TEXTURE_COMPARE_MODE TEXTURE_IMMUTABLE_FORMAT TEXTURE_IMMUTABLE_LEVELS ' +
    'TEXTURE_MAX_LEVEL TEXTURE_MAX_LOD TEXTURE_MIN_LOD TEXTURE_WRAP_R TIMEOUT_EXPIRED TIMEOUT_IGNORED ' +
    'TRANSFORM_FEEDBACK TRANSFORM_FEEDBACK_ACTIVE TRANSFORM_FEEDBACK_BINDING TRANSFORM_FEEDBACK_BUFFER ' +
    'TRANSFORM_FEEDBACK_BUFFER_BINDING TRANSFORM_FEEDBACK_BUFFER_MODE TRANSFORM_FEEDBACK_BUFFER_SIZE ' +
    'TRANSFORM_FEEDBACK_BUFFER_START TRANSFORM_FEEDBACK_PAUSED TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN ' +
    'TRANSFORM_FEEDBACK_VARYINGS UNIFORM_ARRAY_STRIDE UNIFORM_BLOCK_ACTIVE_UNIFORMS ' +
    'UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES UNIFORM_BLOCK_BINDING UNIFORM_BLOCK_DATA_SIZE UNIFORM_BLOCK_INDEX ' +
    'UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER UNIFORM_BUFFER ' +
    'UNIFORM_BUFFER_BINDING UNIFORM_BUFFER_OFFSET_ALIGNMENT UNIFORM_BUFFER_SIZE UNIFORM_BUFFER_START ' +
    'UNIFORM_IS_ROW_MAJOR UNIFORM_MATRIX_STRIDE UNIFORM_OFFSET UNIFORM_SIZE UNIFORM_TYPE UNPACK_IMAGE_HEIGHT ' +
    'UNPACK_ROW_LENGTH UNPACK_SKIP_IMAGES UNPACK_SKIP_PIXELS UNPACK_SKIP_ROWS UNSIGNALED ' +
    'UNSIGNED_INT_10F_11F_11F_REV UNSIGNED_INT_24_8 UNSIGNED_INT_2_10_10_10_REV UNSIGNED_INT_5_9_9_9_REV ' +
    'UNSIGNED_INT_SAMPLER_2D UNSIGNED_INT_SAMPLER_2D_ARRAY UNSIGNED_INT_SAMPLER_3D UNSIGNED_INT_SAMPLER_CUBE ' +
    'UNSIGNED_INT_VEC2 UNSIGNED_INT_VEC3 UNSIGNED_INT_VEC4 UNSIGNED_NORMALIZED VERTEX_ARRAY_BINDING ' +
    'VERTEX_ATTRIB_ARRAY_DIVISOR VERTEX_ATTRIB_ARRAY_INTEGER WAIT_FAILED').split(' ');

  function withoutWebGL2Constants(target) {
    WEBGL2_ONLY_CONSTANTS.forEach(function (name) {
      if (Object.prototype.hasOwnProperty.call(target, name)) delete target[name];
    });
  }

  if (typeof global.WebGLRenderingContext === 'function') {
    installExtensionObjects(global.WebGLRenderingContext.prototype);
    withoutWebGL2Constants(global.WebGLRenderingContext.prototype);
    withoutWebGL2Constants(global.WebGLRenderingContext);
  }

  // -------------------------------------------------------------------------
  // The canvas, and the one GL context behind it
  // -------------------------------------------------------------------------

  // All three spellings resolve to the same object. Lightning asks for
  // `e ? 'webgl2' : 'webgl'` and falls back to `experimental-webgl`; ANGLE on
  // Apple grants ES 3.0, which is exactly what WebGL2 is defined against
  // (`Architecture.md` §9), so one context satisfies every spelling truthfully.
  // A GLES2-only GPU -- a Raspberry Pi 3's VideoCore IV -- gets a WebGL1 context,
  // and there 'webgl2' answers null, so an engine falls back as in a browser.
  var WEBGL_CONTEXT_IDS = {
    'webgl': true,
    'webgl2': true,
    'experimental-webgl': true
  };

  // -------------------------------------------------------------------------
  // One GL context per canvas
  //
  // The first canvas of a page that CSS has not placed *is* the frame: it gets
  // the context the graphics bootstrap made (`gl`), the window's framebuffer,
  // and the size and present path a page has had since M4. Every other canvas
  // that asks for WebGL gets a real GL context of its own -- its own bound
  // program, textures and blend state, as WebGL says two canvases should have --
  // drawing into an FBO-backed layer the page's own present composites at the
  // element's CSS rect (`__screenkit.canvas`, `Architecture.md` §3.1).
  //
  // Which of the two a canvas gets is decided when it asks. A canvas the CSS
  // subset has already placed, resized or hidden becomes a layer; one it has not
  // takes the frame, if the frame is still free. A canvas that took the frame
  // and is placed *afterwards* keeps it and says so once: its context is the
  // window's, and there is no moving a window into a layer after the fact.
  // -------------------------------------------------------------------------

  // The one canvas that is the page's frame. Null until somebody asks.
  var backedCanvas = null;
  // How many canvases were ever given a layer of their own: tree, style and attribute
  // changes only look for one once there is one.
  var canvasLayerCount = 0;

  function canvasApi() {
    var io = global.__screenkit;
    return io && io.canvas && typeof io.canvas.create === 'function' ? io.canvas : null;
  }

  // `webgl2` on a context that is not one answers null, as a browser does on a
  // GLES2-only GPU; `webgl` and `experimental-webgl` take whatever there is.
  function contextForSpelling(id, context) {
    if (context === null || context === undefined) return null;
    if (id === 'webgl2' &&
        !(typeof global.WebGL2RenderingContext === 'function' &&
          context instanceof global.WebGL2RenderingContext)) {
      return null;
    }
    return context;
  }

  // Has the CSS subset given this canvas a rect of its own -- a position, a
  // size, a transform, or a `display`/`opacity` that hides it? That is what
  // makes it a layer rather than the page's frame.
  function placedCanvas(node) {
    if (node.style === null) return false;  // nothing was ever set on it
    var layer = layerOf(node);
    for (var name in layer) {
      if (divergence(name, layer[name]) !== null) return true;
    }
    return false;
  }

  function noSecondContext(why) {
    announce('canvas:layer',
      'ScreenKit: canvas.getContext("webgl") on a second canvas returned null -- ' + why +
      '. The web allows getContext to answer null, and this runtime says so once rather than ' +
      'handing out a context that draws nowhere (Architecture.md 3.1).');
  }

  // A canvas that is not the frame: its own GL context, its own drawing buffer,
  // and a layer at its CSS rect. Null when this page cannot composite one.
  function makeCanvasLayer(canvas, state) {
    var api = canvasApi();
    if (api === null) {
      noSecondContext('this runtime has no canvas compositor (__screenkit.canvas is missing)');
      return null;
    }
    var plane = planeFor(state);
    // A size written before the context was asked for is the drawing buffer's,
    // as it is in a browser; otherwise the buffer starts at the rect's pixels.
    var sized = state.width !== null || state.height !== null;
    var width = state.width !== null ? state.width : Math.round(plane.w);
    var height = state.height !== null ? state.height : Math.round(plane.h);
    var made = api.create(canvas, width, height);
    if (!made || !made.gl) {
      noSecondContext('this page has no compositor for it, or the driver would give no more contexts');
      // Remembered, because `getContext` is answered from a render loop as often
      // as not: without this every frame would attempt the whole native
      // allocation again, silently, since the message above is said once.
      state.refused = true;
      return null;
    }
    state.id = made.id;
    state.gl = made.gl;
    state.kind = 'webgl';
    state.sized = sized;
    state.connected = canvas.isConnected;
    canvasLayerCount++;
    // WebGL specifies `gl.canvas` as the canvas that owns the context.
    try {
      state.gl.canvas = canvas;
    } catch (e) {
      announce('gl.canvas', 'ScreenKit: could not set gl.canvas: ' + (e && e.message));
    }
    // Its own paint flag: "present only a frame that painted" is per layer, so
    // an idle canvas keeps its last image and costs nothing.
    trackPaint(state.gl, String(state.id));
    updateCanvasPlane(state);
    return state.gl;
  }

  function resizeCanvasLayer(state) {
    var api = canvasApi();
    if (api === null || state.id === null) return;
    api.setSize(state.id,
                state.width !== null ? state.width : canvasBufferSize(state, 'width'),
                state.height !== null ? state.height : canvasBufferSize(state, 'height'));
  }

  function getContext(canvas, contextId, options) {
    var id = String(contextId);
    var state = canvasStates.get(canvas);

    // A canvas has one kind of context: asked for the other kind, a browser
    // answers null.
    if (id === '2d') {
      if (state.kind === 'webgl') return null;
      if (state.context2d === null) {
        state.kind = '2d';
        state.context2d = make2dContext(canvas, state, options);
      }
      return state.context2d;
    }

    if (WEBGL_CONTEXT_IDS[id] !== true) {
      // The web spec lets getContext return null for a context type the
      // implementation does not support. A stub returning `undefined` would turn
      // this into a crash somewhere unrelated.
      announce('context:' + id,
        'ScreenKit: canvas.getContext("' + id + '") is not supported and returned null. ' +
        'This runtime draws through WebGL and a software 2D subset -- there is no ' + id +
        ' context (Architecture.md 3.2).');
      return null;
    }
    if (state.kind === '2d') return null;
    // Every later getContext on the same canvas gets the same object back.
    if (state.gl !== null) return contextForSpelling(id, state.gl);
    // A canvas refused once is refused for good: the answer cannot change, and
    // asking again is what an engine's render loop does.
    if (state.refused === true) return null;

    var frame = glContext();
    if (frame === null) {
      throw new Error(
        'ScreenKit: canvas.getContext("' + id + '") has no context to hand out -- the `gl` ' +
        'global is missing, so the graphics bootstrap never ran. The DOM shim does not ' +
        'create GL contexts; it hands out the one the host already made.');
    }
    // Every context here comes from one driver, so what the frame's spelling
    // answers is what any of them answers: refuse `webgl2` on a GLES2 GPU before
    // anything is allocated for it.
    if (contextForSpelling(id, frame) === null) return null;

    // A canvas that asked first but never made it into the document gives the
    // frame up to the next one that asks. That is how engines feature-detect:
    // Pixi's isWebGLSupported and Phaser's Features.webGL each make a throwaway
    // canvas, ask it for a context and drop it, and only then create the canvas
    // they draw on -- so a probe costs no context and leaves no layer behind. A
    // canvas that is on the page keeps the frame.
    if (backedCanvas !== null && backedCanvas !== canvas && !backedCanvas.isConnected) {
      backedCanvas = null;
    }
    if (backedCanvas === null && !placedCanvas(nodeOf(canvas))) {
      backedCanvas = canvas;
      state.kind = 'webgl';
      state.gl = frame;
      if ((state.width !== null && state.width !== drawableWidth()) ||
          (state.height !== null && state.height !== drawableHeight())) {
        announce('size:gl',
          'ScreenKit: the canvas was sized ' + (state.width === null ? drawableWidth() : state.width) + 'x' +
          (state.height === null ? drawableHeight() : state.height) + ' before it took the GL ' +
          'surface, which is ' + drawableWidth() + 'x' + drawableHeight() + '. It reads the surface size ' +
          'from here on: this canvas is the page\'s frame.');
      }
      // WebGL specifies `gl.canvas` as the canvas that owns the context, and
      // Lightning reads `glw.canvas.width` on every render op to size its
      // resolution uniform. Left unset it is `undefined.width`, thrown from inside
      // the first requestAnimationFrame -- the app launches and then draws nothing.
      try {
        frame.canvas = canvas;
      } catch (e) {
        announce('gl.canvas', 'ScreenKit: could not set gl.canvas: ' + (e && e.message));
      }
      return frame;
    }

    return makeCanvasLayer(canvas, state);
  }

  function makeCanvas() {
    // Lightning checks `x instanceof HTMLCanvasElement` to tell a canvas it was
    // given from one it should make, so the prototype is part of the contract.
    var canvas = defineBackedSize(newElement('canvas', HTMLCanvasElement.prototype));
    // A closure rather than a shared method, so a destructured
    // `const { getContext } = canvas` still knows which canvas it came from --
    // which context a canvas owns is about identity and must not hinge on `this`.
    canvas.getContext = function (contextId, options) { return getContext(canvas, contextId, options); };
    // The frame's canvas is the whole drawable, at 0,0. A canvas with a layer of
    // its own reports the rect the CSS subset placed it at, which is what it is.
    canvas.getBoundingClientRect = function () {
      var state = canvasStates.get(canvas);
      if (state.id === null) return rect(canvas.width, canvas.height);
      var plane = state.plane !== null ? state.plane : planeFor(state);
      return { x: plane.x, y: plane.y, left: plane.x, top: plane.y, width: plane.w, height: plane.h,
               right: plane.x + plane.w, bottom: plane.y + plane.h };
    };
    // A 2D canvas's pixels, which is what texImage2D and createImageBitmap read
    // for a canvas source (readImageSource in VendoredWebGL.cpp). Undefined for
    // any other canvas.
    Object.defineProperty(canvas, 'data', {
      enumerable: false,
      configurable: true,
      get: function () {
        var state = canvasStates.get(canvas);
        return state.context2d === null ? undefined : context2dStates.get(state.context2d).pixels;
      }
    });
    return canvas;
  }
  function HTMLCanvasElement() {
    throw new TypeError('Illegal constructor: use document.createElement("canvas")');
  }
  inherit(HTMLCanvasElement, HTMLElement);
  global.HTMLCanvasElement = HTMLCanvasElement;

  // --- a canvas's layer -------------------------------------------------------
  //
  // The same rect the CSS subset computes for a `<video>`'s plane and an
  // `<iframe>`'s layer (`planeFor`), sent to `__screenkit.canvas.setPlane`.
  // There is no second geometry path.

  function updateCanvasPlane(state) {
    var api = canvasApi();
    if (state.id === null || api === null) return;
    var plane = planeFor(state);
    var layer = layerOf(state.node);
    var opacity = layer.opacity && !layer.opacity.global ? layer.opacity.opacity : 1;
    var last = state.plane;
    if (last !== null && last.x === plane.x && last.y === plane.y && last.w === plane.w &&
        last.h === plane.h && last.visible === plane.visible && last.z === plane.z &&
        last.opacity === opacity) {
      return;
    }
    plane.opacity = opacity;
    state.plane = plane;
    api.setPlane(state.id, plane.x, plane.y, plane.w, plane.h, plane.visible, plane.z, opacity);
    // Until a script writes `canvas.width`, the drawing buffer follows the rect:
    // a HUD placed over half the screen is drawn at half the screen's pixels,
    // which is what `canvas.width` then reads back.
    if (!state.sized) {
      api.setSize(state.id, Math.max(1, Math.round(plane.w)), Math.max(1, Math.round(plane.h)));
    }
  }

  function canvasLayerChanged(node) {
    var state = canvasStates.get(node.object);
    if (state !== undefined && state.id !== null) planeChanged(state);
  }

  // A declaration the CSS subset accepted, on a canvas. One with a layer of its
  // own follows it; the one that *is* the page's frame cannot, and says so once.
  function canvasDeclarationApplied(styleState, name, parsed, value) {
    var state = canvasStates.get(styleState.node.object);
    if (state === undefined) return;
    if (state.id !== null) {
      planeChanged(state);
      return;
    }
    // A canvas with no context yet is neither: what CSS says is simply where it
    // will be placed the moment it asks for one.
    if (state.element !== backedCanvas) return;
    var effect = divergence(name, parsed);
    if (effect === 'position' || effect === 'size') {
      announce('style-layout',
        'ScreenKit: <canvas>.style ' + name + ': ' + value + ' is stored and reads back, but ' +
        'position and size in CSS do not move or resize the <canvas> that is the page\'s frame: it ' +
        'is the drawable itself. Give a canvas a rect before it asks for its context and it gets a ' +
        'layer of its own at that rect instead (Architecture.md 3.1). This is said once.');
    } else if (effect === 'visibility') {
      announce('style-visibility',
        'ScreenKit: <canvas>.style ' + name + ': ' + value + ' is stored and reads back, but the ' +
        '<canvas> that is the page\'s frame stays visible: it is the drawable itself. A canvas placed ' +
        'before it asks for its context is a layer, and a layer can be hidden (Architecture.md 3.1). ' +
        'This is said once.');
    }
  }

  // Leaving the document hides a canvas's layer, and coming back shows it
  // again -- `planeFor` reads `isConnected`. Deferred to a microtask, as the
  // media and iframe paths are, so a node moved within the document does not
  // flicker through invisible.
  function canvasTreeChanged(object) {
    if (canvasLayerCount === 0) return;
    var found = [];
    var own = canvasStates.get(object);
    if (own !== undefined && own.id !== null) found.push(own);
    var node = nodes.get(object);
    if (node.type === ELEMENT_NODE || node.type === DOCUMENT_NODE) {
      walk(object, function (kid) {
        var state = canvasStates.get(kid);
        if (state !== undefined && state.id !== null) found.push(state);
        return false;
      });
    }
    found.forEach(planeChanged);
  }

  function allCanvasLayers(visit) {
    if (canvasLayerCount === 0) return;
    walk(documentNode, function (object) {
      var state = canvasStates.get(object);
      if (state !== undefined && state.id !== null) visit(state);
      return false;
    });
  }
  // Percent and viewport units are the drawable's.
  addListener(global, 'resize', function () { allCanvasLayers(planeChanged); });

  // Video is always beneath every canvas, whatever its z-index (Architecture.md
  // 4). With more than one canvas that is a question per canvas, not a question
  // about the one that holds the drawable.
  function aboveACanvas(element, plane) {
    var above = false;
    walk(documentNode, function (object) {
      var state = canvasStates.get(object);
      if (state === undefined || state.kind !== 'webgl') return false;
      var z = zIndexOf(layerOf(state.node));
      if (plane.z > z || (plane.z === z && precedes(state.element, element))) above = true;
      return above;
    });
    return above;
  }

  // -------------------------------------------------------------------------
  // Canvas 2D: a software subset
  //
  // Engines use a 2D canvas as a scratchpad even when they draw with WebGL:
  // Phaser probes blend modes and alpha with fillRect/getImageData/drawImage the
  // moment it is imported and keeps a 2D context for pixel reads; Pixi measures
  // on one. So getContext('2d') gives a real CanvasRenderingContext2D, drawn on
  // the CPU into an RGBA buffer the canvas owns -- and that canvas uploads to
  // WebGL (texImage2D) and feeds createImageBitmap like any image.
  //
  // The subset: fillRect, clearRect, strokeRect, drawImage (canvases, images,
  // bitmaps, ImageData; nearest or bilinear by imageSmoothingEnabled),
  // getImageData / putImageData / createImageData, save / restore, the
  // transform methods, globalAlpha, and the common globalCompositeOperation
  // modes. Colours are CSS colour strings. Shapes are not antialiased.
  //
  // Text: fillText, strokeText and measureText, in fonts from document.fonts or
  // installed on the system. Glyphs come from SDL3_ttf (__screenkit.text) as
  // coverage masks and are composited here like any other pixel. Text is drawn
  // upright at the transform's scale; letter spacing is left to the engine.
  //
  // Not drawn: paths (fill / stroke / clip), patterns and shadows. Those methods
  // exist, draw nothing and say so once. A gradient fills in its first colour.
  //
  // Pixels are stored unpremultiplied, which is what getImageData returns and
  // what texImage2D reads for a canvas source.
  // -------------------------------------------------------------------------

  var NAMED_COLORS = table({
    aliceblue: 'f0f8ff', antiquewhite: 'faebd7', aqua: '00ffff', aquamarine: '7fffd4', azure: 'f0ffff',
    beige: 'f5f5dc', bisque: 'ffe4c4', black: '000000', blanchedalmond: 'ffebcd', blue: '0000ff',
    blueviolet: '8a2be2', brown: 'a52a2a', burlywood: 'deb887', cadetblue: '5f9ea0', chartreuse: '7fff00',
    chocolate: 'd2691e', coral: 'ff7f50', cornflowerblue: '6495ed', cornsilk: 'fff8dc', crimson: 'dc143c',
    cyan: '00ffff', darkblue: '00008b', darkcyan: '008b8b', darkgoldenrod: 'b8860b', darkgray: 'a9a9a9',
    darkgreen: '006400', darkgrey: 'a9a9a9', darkkhaki: 'bdb76b', darkmagenta: '8b008b', darkolivegreen: '556b2f',
    darkorange: 'ff8c00', darkorchid: '9932cc', darkred: '8b0000', darksalmon: 'e9967a', darkseagreen: '8fbc8f',
    darkslateblue: '483d8b', darkslategray: '2f4f4f', darkslategrey: '2f4f4f', darkturquoise: '00ced1',
    darkviolet: '9400d3', deeppink: 'ff1493', deepskyblue: '00bfff', dimgray: '696969', dimgrey: '696969',
    dodgerblue: '1e90ff', firebrick: 'b22222', floralwhite: 'fffaf0', forestgreen: '228b22', fuchsia: 'ff00ff',
    gainsboro: 'dcdcdc', ghostwhite: 'f8f8ff', gold: 'ffd700', goldenrod: 'daa520', gray: '808080',
    green: '008000', greenyellow: 'adff2f', grey: '808080', honeydew: 'f0fff0', hotpink: 'ff69b4',
    indianred: 'cd5c5c', indigo: '4b0082', ivory: 'fffff0', khaki: 'f0e68c', lavender: 'e6e6fa',
    lavenderblush: 'fff0f5', lawngreen: '7cfc00', lemonchiffon: 'fffacd', lightblue: 'add8e6', lightcoral: 'f08080',
    lightcyan: 'e0ffff', lightgoldenrodyellow: 'fafad2', lightgray: 'd3d3d3', lightgreen: '90ee90',
    lightgrey: 'd3d3d3', lightpink: 'ffb6c1', lightsalmon: 'ffa07a', lightseagreen: '20b2aa',
    lightskyblue: '87cefa', lightslategray: '778899', lightslategrey: '778899', lightsteelblue: 'b0c4de',
    lightyellow: 'ffffe0', lime: '00ff00', limegreen: '32cd32', linen: 'faf0e6', magenta: 'ff00ff',
    maroon: '800000', mediumaquamarine: '66cdaa', mediumblue: '0000cd', mediumorchid: 'ba55d3',
    mediumpurple: '9370db', mediumseagreen: '3cb371', mediumslateblue: '7b68ee', mediumspringgreen: '00fa9a',
    mediumturquoise: '48d1cc', mediumvioletred: 'c71585', midnightblue: '191970', mintcream: 'f5fffa',
    mistyrose: 'ffe4e1', moccasin: 'ffe4b5', navajowhite: 'ffdead', navy: '000080', oldlace: 'fdf5e6',
    olive: '808000', olivedrab: '6b8e23', orange: 'ffa500', orangered: 'ff4500', orchid: 'da70d6',
    palegoldenrod: 'eee8aa', palegreen: '98fb98', paleturquoise: 'afeeee', palevioletred: 'db7093',
    papayawhip: 'ffefd5', peachpuff: 'ffdab9', peru: 'cd853f', pink: 'ffc0cb', plum: 'dda0dd',
    powderblue: 'b0e0e6', purple: '800080', rebeccapurple: '663399', red: 'ff0000', rosybrown: 'bc8f8f',
    royalblue: '4169e1', saddlebrown: '8b4513', salmon: 'fa8072', sandybrown: 'f4a460', seagreen: '2e8b57',
    seashell: 'fff5ee', sienna: 'a0522d', silver: 'c0c0c0', skyblue: '87ceeb', slateblue: '6a5acd',
    slategray: '708090', slategrey: '708090', snow: 'fffafa', springgreen: '00ff7f', steelblue: '4682b4',
    tan: 'd2b48c', teal: '008080', thistle: 'd8bfd8', tomato: 'ff6347', turquoise: '40e0d0', violet: 'ee82ee',
    wheat: 'f5deb3', white: 'ffffff', whitesmoke: 'f5f5f5', yellow: 'ffff00', yellowgreen: '9acd32'
  });

  // A CSS colour string -> [r, g, b, a] with r,g,b 0-255 and a 0-1, or null.
  function parseCssColor(value) {
    var text = String(value).trim().toLowerCase();
    if (text === 'transparent') return [0, 0, 0, 0];
    if (text === 'currentcolor') return [0, 0, 0, 1];
    if (NAMED_COLORS[text]) text = '#' + NAMED_COLORS[text];
    var hex = /^#([0-9a-f]{3,4}|[0-9a-f]{6}|[0-9a-f]{8})$/.exec(text);
    if (hex) {
      var digits = hex[1];
      if (digits.length <= 4) digits = digits.replace(/./g, '$&$&');
      return [parseInt(digits.slice(0, 2), 16), parseInt(digits.slice(2, 4), 16), parseInt(digits.slice(4, 6), 16),
              digits.length === 8 ? parseInt(digits.slice(6, 8), 16) / 255 : 1];
    }
    var fn = /^(rgba?|hsla?)\(\s*([^)]*)\)$/.exec(text);
    if (!fn) return null;
    var parts = fn[2].split(/\s*[,\/]\s*|\s+/).filter(function (p) { return p !== ''; });
    if (parts.length !== 3 && parts.length !== 4) return null;
    function number(p, percentOf) {
      var n = parseFloat(p);
      if (isNaN(n)) return NaN;
      return p.charAt(p.length - 1) === '%' ? n / 100 * percentOf : n;
    }
    var alpha = parts.length === 4 ? number(parts[3], 1) : 1;
    var out;
    if (fn[1].charAt(0) === 'r') {
      out = [number(parts[0], 255), number(parts[1], 255), number(parts[2], 255)];
    } else {
      var h = (((parseFloat(parts[0]) % 360) + 360) % 360) / 360;
      var s = number(parts[1], 1), l = number(parts[2], 1);
      var q = l < 0.5 ? l * (1 + s) : l + s - l * s, p2 = 2 * l - q;
      var channel = function (t) {
        t = t < 0 ? t + 1 : t > 1 ? t - 1 : t;
        if (t < 1 / 6) return p2 + (q - p2) * 6 * t;
        if (t < 1 / 2) return q;
        if (t < 2 / 3) return p2 + (q - p2) * (2 / 3 - t) * 6;
        return p2;
      };
      out = [channel(h + 1 / 3) * 255, channel(h) * 255, channel(h - 1 / 3) * 255];
    }
    out.push(alpha);
    for (var i = 0; i < 4; i++) if (isNaN(out[i])) return null;
    return [Math.round(Math.min(255, Math.max(0, out[0]))), Math.round(Math.min(255, Math.max(0, out[1]))),
            Math.round(Math.min(255, Math.max(0, out[2]))), Math.min(1, Math.max(0, out[3]))];
  }

  // The serialisation a browser reads back from fillStyle.
  function serializeColor(c) {
    if (c[3] === 1) {
      return '#' + [c[0], c[1], c[2]].map(function (v) { return (v < 16 ? '0' : '') + v.toString(16); }).join('');
    }
    return 'rgba(' + c[0] + ', ' + c[1] + ', ' + c[2] + ', ' + (Math.round(c[3] * 1000) / 1000) + ')';
  }

  var COMPOSITE_MODES = table({
    'source-over': 1, 'source-in': 1, 'source-out': 1, 'source-atop': 1, 'destination-over': 1,
    'destination-in': 1, 'destination-out': 1, 'destination-atop': 1, 'lighter': 1, 'copy': 1, 'xor': 1,
    'multiply': 1, 'screen': 1, 'darken': 1, 'lighten': 1
  });
  var OTHER_COMPOSITE_MODES = table({
    'overlay': 1, 'color-dodge': 1, 'color-burn': 1, 'hard-light': 1, 'soft-light': 1, 'difference': 1,
    'exclusion': 1, 'hue': 1, 'saturation': 1, 'color': 1, 'luminosity': 1
  });

  var context2dStates = new WeakMap();

  function CanvasRenderingContext2D() {
    throw new TypeError("Failed to construct 'CanvasRenderingContext2D': Illegal constructor");
  }
  global.CanvasRenderingContext2D = CanvasRenderingContext2D;

  function context2dOf(value) {
    var state = value !== null && typeof value === 'object' ? context2dStates.get(value) : undefined;
    if (!state) throw new TypeError('Illegal invocation');
    return state;
  }

  function freshDrawState() {
    return {
      fill: [0, 0, 0, 1], stroke: [0, 0, 0, 1], alpha: 1, composite: 'source-over', smoothing: true,
      smoothingQuality: 'low', matrix: [1, 0, 0, 1, 0, 0], lineWidth: 1, lineCap: 'butt', lineJoin: 'miter',
      miterLimit: 10, lineDashOffset: 0, font: parseFont('10px sans-serif'), textAlign: 'start', textBaseline: 'alphabetic',
      direction: 'inherit', shadowBlur: 0, shadowColor: [0, 0, 0, 0], shadowOffsetX: 0, shadowOffsetY: 0,
      filter: 'none', fillStyleObject: null, strokeStyleObject: null
    };
  }
  function copyDrawState(s) {
    var out = {};
    for (var key in s) out[key] = Array.isArray(s[key]) ? s[key].slice() : s[key];
    return out;
  }

  function make2dContext(canvas, canvasStateObject, options) {
    var context = Object.create(CanvasRenderingContext2D.prototype);
    var state = {
      canvas: canvas, owner: canvasStateObject, pixels: null, width: 0, height: 0,
      draw: freshDrawState(), stack: [],
      attributes: { alpha: !(options && options.alpha === false), colorSpace: 'srgb', desynchronized: false,
                    willReadFrequently: !!(options && options.willReadFrequently) }
    };
    context2dStates.set(context, state);
    resizeContext2d(state);
    return context;
  }

  // Setting width or height resets a 2D canvas: a new, transparent buffer and
  // the default drawing state.
  function resizeContext2d(state) {
    state.width = state.canvas.width;
    state.height = state.canvas.height;
    state.pixels = new Uint8ClampedArray(state.width * state.height * 4);
    state.draw = freshDrawState();
    state.stack = [];
  }

  function unsupported2d(what) {
    announce('2d:' + what,
      'ScreenKit: CanvasRenderingContext2D.' + what + ' draws nothing here. The 2D context is a software ' +
      'subset -- rectangles, images, pixel data, text, transforms and compositing; paths, patterns and ' +
      'shadows are not drawn.');
  }

  // Blend one source pixel (unpremultiplied r,g,b 0-255; a 0-1) into the buffer.
  function blendPixel(pixels, i, sr, sg, sb, sa, mode) {
    var dr = pixels[i], dg = pixels[i + 1], db = pixels[i + 2], da = pixels[i + 3] / 255;
    var ao, cr, cg, cb;
    switch (mode) {
      case 'copy':
        ao = sa; cr = sr; cg = sg; cb = sb;
        break;
      case 'source-in':
        ao = sa * da; cr = sr; cg = sg; cb = sb;
        break;
      case 'source-out':
        ao = sa * (1 - da); cr = sr; cg = sg; cb = sb;
        break;
      case 'source-atop':
        ao = da;
        cr = sr * sa + dr * (1 - sa); cg = sg * sa + dg * (1 - sa); cb = sb * sa + db * (1 - sa);
        break;
      case 'destination-over':
        ao = da + sa * (1 - da);
        if (ao === 0) { cr = cg = cb = 0; break; }
        cr = (dr * da + sr * sa * (1 - da)) / ao; cg = (dg * da + sg * sa * (1 - da)) / ao;
        cb = (db * da + sb * sa * (1 - da)) / ao;
        break;
      case 'destination-in':
        ao = da * sa; cr = dr; cg = dg; cb = db;
        break;
      case 'destination-out':
        ao = da * (1 - sa); cr = dr; cg = dg; cb = db;
        break;
      case 'destination-atop':
        ao = sa;
        cr = dr * da + sr * (1 - da); cg = dg * da + sg * (1 - da); cb = db * da + sb * (1 - da);
        break;
      case 'xor':
        ao = sa * (1 - da) + da * (1 - sa);
        if (ao === 0) { cr = cg = cb = 0; break; }
        cr = (sr * sa * (1 - da) + dr * da * (1 - sa)) / ao; cg = (sg * sa * (1 - da) + dg * da * (1 - sa)) / ao;
        cb = (sb * sa * (1 - da) + db * da * (1 - sa)) / ao;
        break;
      case 'lighter':
        ao = Math.min(1, sa + da);
        if (ao === 0) { cr = cg = cb = 0; break; }
        cr = Math.min(255, sr * sa + dr * da) / ao; cg = Math.min(255, sg * sa + dg * da) / ao;
        cb = Math.min(255, sb * sa + db * da) / ao;
        break;
      case 'multiply':
      case 'screen':
      case 'darken':
      case 'lighten': {
        ao = sa + da - sa * da;
        if (ao === 0) { cr = cg = cb = 0; break; }
        var mix = function (s, d) {
          var b = mode === 'multiply' ? s * d / 255 : mode === 'screen' ? s + d - s * d / 255 :
                  mode === 'darken' ? Math.min(s, d) : Math.max(s, d);
          return (s * sa * (1 - da) + d * da * (1 - sa) + sa * da * b) / ao;
        };
        cr = mix(sr, dr); cg = mix(sg, dg); cb = mix(sb, db);
        break;
      }
      default:  // source-over
        ao = sa + da * (1 - sa);
        if (ao === 0) { cr = cg = cb = 0; break; }
        cr = (sr * sa + dr * da * (1 - sa)) / ao; cg = (sg * sa + dg * da * (1 - sa)) / ao;
        cb = (sb * sa + db * da * (1 - sa)) / ao;
    }
    pixels[i] = cr; pixels[i + 1] = cg; pixels[i + 2] = cb; pixels[i + 3] = ao * 255;
  }

  function invert(m) {
    var det = m[0] * m[3] - m[1] * m[2];
    if (det === 0 || !isFinite(det)) return null;
    return [m[3] / det, -m[1] / det, -m[2] / det, m[0] / det,
            (m[2] * m[5] - m[3] * m[4]) / det, (m[1] * m[4] - m[0] * m[5]) / det];
  }

  // Visit every buffer pixel whose centre falls inside the user-space rectangle
  // (x, y, w, h) under the current transform, with the rectangle-local position.
  function scanRect(state, x, y, w, h, visit) {
    if (!(w > 0 && h > 0) || !isFinite(x + y + w + h)) return;
    var m = state.draw.matrix, inv = invert(m);
    if (inv === null) return;
    var xs = [], ys = [];
    [[x, y], [x + w, y], [x, y + h], [x + w, y + h]].forEach(function (p) {
      xs.push(m[0] * p[0] + m[2] * p[1] + m[4]);
      ys.push(m[1] * p[0] + m[3] * p[1] + m[5]);
    });
    var left = Math.max(0, Math.floor(Math.min.apply(null, xs))), right = Math.min(state.width, Math.ceil(Math.max.apply(null, xs)));
    var top = Math.max(0, Math.floor(Math.min.apply(null, ys))), bottom = Math.min(state.height, Math.ceil(Math.max.apply(null, ys)));
    for (var py = top; py < bottom; py++) {
      for (var px = left; px < right; px++) {
        var cx = px + 0.5, cy = py + 0.5;
        var ux = inv[0] * cx + inv[2] * cy + inv[4], uy = inv[1] * cx + inv[3] * cy + inv[5];
        if (ux >= x && ux < x + w && uy >= y && uy < y + h) visit((py * state.width + px) * 4, ux - x, uy - y);
      }
    }
  }

  function isAxisAligned(m) {
    return m[1] === 0 && m[2] === 0 && m[0] > 0 && m[3] > 0;
  }

  function fillRect2d(state, x, y, w, h, color) {
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    var draw = state.draw, sa = color[3] * draw.alpha;
    var pixels = state.pixels;
    // The common case, done by rows: an opaque colour, source-over or copy, no
    // rotation. A full-screen clear to a colour is one row pattern copied down.
    var m = draw.matrix;
    if (sa === 1 && (draw.composite === 'source-over' || draw.composite === 'copy') && isAxisAligned(m)) {
      var left = Math.max(0, Math.round(m[0] * x + m[4])), right = Math.min(state.width, Math.round(m[0] * (x + w) + m[4]));
      var top = Math.max(0, Math.round(m[3] * y + m[5])), bottom = Math.min(state.height, Math.round(m[3] * (y + h) + m[5]));
      if (left >= right || top >= bottom) return;
      var row = new Uint8ClampedArray((right - left) * 4);
      for (var i = 0; i < row.length; i += 4) {
        row[i] = color[0]; row[i + 1] = color[1]; row[i + 2] = color[2]; row[i + 3] = 255;
      }
      for (var py = top; py < bottom; py++) pixels.set(row, (py * state.width + left) * 4);
      return;
    }
    var mode = draw.composite;
    scanRect(state, x, y, w, h, function (index) {
      blendPixel(pixels, index, color[0], color[1], color[2], sa, mode);
    });
  }

  // The pixels behind a drawImage source, or null when there are none to read.
  var decodedAssets = new WeakMap();
  function imagePixels(source) {
    if (source === null || typeof source !== 'object') return null;
    var canvasState = canvasStates.get(source);
    if (canvasState) {
      if (canvasState.context2d !== null) {
        var s = context2dStates.get(canvasState.context2d);
        return { width: s.width, height: s.height, data: s.pixels };
      }
      unsupported2d('drawImage(a WebGL canvas)');
      return null;
    }
    if (source._pixels) return source._pixels;
    if (source.data && typeof source.width === 'number' && typeof source.height === 'number') {
      return { width: source.width, height: source.height, data: source.data };
    }
    if (source._assetPath) {
      var cached = decodedAssets.get(source);
      if (!cached || cached.path !== source._assetPath) {
        cached = { path: source._assetPath, pixels: decodePixels(io.readFile(source._assetPath)) };
        decodedAssets.set(source, cached);
      }
      return cached.pixels;
    }
    return null;
  }

  function drawImage2d(state, pixelsIn, sx, sy, sw, sh, dx, dy, dw, dh) {
    if (sw < 0) { sx += sw; sw = -sw; }
    if (sh < 0) { sy += sh; sh = -sh; }
    if (dw < 0) { dx += dw; dw = -dw; }
    if (dh < 0) { dy += dh; dh = -dh; }
    if (sw === 0 || sh === 0 || dw === 0 || dh === 0) return;
    var draw = state.draw, alpha = draw.alpha, mode = draw.composite, smooth = draw.smoothing;
    var src = pixelsIn.data, width = pixelsIn.width, height = pixelsIn.height, out = state.pixels;
    var scaleX = sw / dw, scaleY = sh / dh;
    function texel(x, y, channel) {
      x = x < 0 ? 0 : x >= width ? width - 1 : x;
      y = y < 0 ? 0 : y >= height ? height - 1 : y;
      return src[(y * width + x) * 4 + channel];
    }
    scanRect(state, dx, dy, dw, dh, function (index, lx, ly) {
      var u = sx + lx * scaleX, v = sy + ly * scaleY;
      var r, g, b, a;
      if (!smooth) {
        var tx = Math.floor(u), ty = Math.floor(v);
        if (tx < 0 || ty < 0 || tx >= width || ty >= height) return;
        var k = (ty * width + tx) * 4;
        r = src[k]; g = src[k + 1]; b = src[k + 2]; a = src[k + 3];
      } else {
        // Bilinear, clamped to the source rectangle's edge texels. Colour is
        // weighted by alpha so transparent texels do not bleed their RGB.
        var fu = Math.min(Math.max(u - 0.5, sx), sx + sw - 1), fv = Math.min(Math.max(v - 0.5, sy), sy + sh - 1);
        var x0 = Math.floor(fu), y0 = Math.floor(fv), wx = fu - x0, wy = fv - y0;
        var x1 = Math.min(x0 + 1, Math.ceil(sx + sw) - 1), y1 = Math.min(y0 + 1, Math.ceil(sy + sh) - 1);
        var w00 = (1 - wx) * (1 - wy), w10 = wx * (1 - wy), w01 = (1 - wx) * wy, w11 = wx * wy;
        var a00 = texel(x0, y0, 3), a10 = texel(x1, y0, 3), a01 = texel(x0, y1, 3), a11 = texel(x1, y1, 3);
        a = a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11;
        if (a === 0) return;
        var weighted = function (channel) {
          return (texel(x0, y0, channel) * a00 * w00 + texel(x1, y0, channel) * a10 * w10 +
                  texel(x0, y1, channel) * a01 * w01 + texel(x1, y1, channel) * a11 * w11) / a;
        };
        r = weighted(0); g = weighted(1); b = weighted(2);
      }
      if (a === 0 && mode !== 'copy' && mode !== 'source-in' && mode !== 'destination-in' && mode !== 'destination-atop') return;
      blendPixel(out, index, r, g, b, (a / 255) * alpha, mode);
    });
  }

  // ---- text ------------------------------------------------------------------

  // A CSS font shorthand -> {text, size (px), bold, italic, families}, or null
  // when it is not one -- which the `font` setter ignores, as a browser does.
  var FONT_SHORTHAND = /^\s*((?:(?:normal|italic|oblique|small-caps|bold|bolder|lighter|[1-9]00)\s+)*)(\d*\.?\d+)(px|pt|em|rem)(?:\s*\/\s*\S+)?\s+(\S.*?)\s*$/i;
  function parseFont(value) {
    var match = FONT_SHORTHAND.exec(String(value));
    if (!match) return null;
    var font = { size: parseFloat(match[2]) * { px: 1, pt: 4 / 3, em: 10, rem: 10 }[match[3].toLowerCase()],
                 bold: false, italic: false, families: [] };
    match[1].toLowerCase().split(/\s+/).forEach(function (token) {
      if (token === 'italic' || token === 'oblique') font.italic = true;
      else if (token === 'bold' || token === 'bolder' || Number(token) >= 600) font.bold = true;
    });
    font.families = match[4].split(',').map(function (family) {
      return family.trim().replace(/^(["'])(.*)\1$/, '$2');
    }).filter(function (family) { return family !== ''; });
    if (!(font.size > 0) || font.families.length === 0) return null;
    font.text = (font.italic ? 'italic ' : '') + (font.bold ? 'bold ' : '') + font.size + 'px ' +
                font.families.join(', ');
    return font;
  }

  function textApi() {
    return io && io.text ? io.text : null;
  }

  // Which loaded face draws a font: a FontFace in document.fonts with the family,
  // else an installed one, else the system's sans-serif -- {face, flags}, where
  // flags ask SDL_ttf to synthesise bold (1) or italic (2) the face lacks.
  // Cached per font until document.fonts changes.
  var GENERIC_FAMILIES = table({ 'serif': 1, 'sans-serif': 1, 'monospace': 1, 'cursive': 1, 'fantasy': 1, 'system-ui': 1 });
  var fontFacesChanged = 0, resolvedFonts = Object.create(null), resolvedFontsAt = -1;
  var systemFamilies = Object.create(null), systemFaceIds = Object.create(null);

  function resolveFont(font) {
    var api = textApi();
    if (api === null) return null;
    if (resolvedFontsAt !== fontFacesChanged) { resolvedFonts = Object.create(null); resolvedFontsAt = fontFacesChanged; }
    if (resolvedFonts[font.text] !== undefined) return resolvedFonts[font.text];
    var families = font.families.concat('sans-serif'), result = null;
    for (var i = 0; i < families.length && result === null; i++) {
      result = documentFontFace(families[i], font) || systemFontFace(api, families[i], font);
    }
    resolvedFonts[font.text] = result;
    return result;
  }
  // The face nearest the font: the right slant first, then the nearest weight to
  // 400 or 700 -- so a family's Regular wins over its ExtraLight for plain text.
  function closestFace(candidates, font) {
    var best = null, bestScore = Infinity, wanted = font.bold ? 700 : 400;
    candidates.forEach(function (candidate) {
      var score = (candidate.italic !== font.italic ? 10000 : 0) + Math.abs(candidate.weight - wanted);
      if (score < bestScore) { best = candidate; bestScore = score; }
    });
    var bold = best.weight >= 600;
    return { face: best.face, flags: (font.bold && !bold ? 1 : 0) | (font.italic && !best.italic ? 2 : 0) };
  }
  function documentFontFace(family, font) {
    var wanted = family.toLowerCase(), candidates = [];
    documentNode.fonts._faces.forEach(function (face) {
      var state = fontFaceStates.get(face);
      if (!state || state.face < 0 || String(face.family).replace(/^(["'])(.*)\1$/, '$2').toLowerCase() !== wanted) return;
      var weight = String(face.weight);
      candidates.push({ face: state.face, weight: weight === 'bold' ? 700 : parseInt(weight, 10) || 400,
                        italic: face.style === 'italic' || face.style === 'oblique' });
    });
    return candidates.length > 0 ? closestFace(candidates, font) : null;
  }
  function systemFontFace(api, family, font) {
    var name = GENERIC_FAMILIES[family.toLowerCase()] === 1 ? api.genericFamily(family.toLowerCase()) : family;
    if (!name) return null;
    var key = name.toLowerCase();
    if (systemFamilies[key] === undefined) systemFamilies[key] = api.systemFaces(name);
    if (systemFamilies[key].length === 0) return null;
    var picked = closestFace(systemFamilies[key].map(function (face) {
      return { face: face.name, weight: face.weight, italic: face.italic };
    }), font);
    if (systemFaceIds[picked.face] === undefined) {
      var added = api.addSystemFont(picked.face);
      systemFaceIds[picked.face] = added === null ? -1 : added.face;
    }
    return systemFaceIds[picked.face] < 0 ? null : { face: systemFaceIds[picked.face], flags: picked.flags };
  }

  // What a text call works with, or null -- said once -- when no font draws it.
  function textRun(state, value) {
    var d = state.draw, api = textApi(), resolved = api === null ? null : resolveFont(d.font);
    if (resolved === null) {
      announce('2d:no-font:' + d.font.text,
        'ScreenKit: no font draws "' + d.font.text + '" -- its text is not drawn. Load one into document.fonts ' +
        'with FontFace, or name an installed family.');
      return null;
    }
    // The canvas spec replaces every ASCII whitespace character with a space.
    var text = String(value).replace(/[\t\n\f\r]/g, ' ');
    var flags = resolved.flags | (d.fontKerning === 'none' ? 4 : 0);
    return { api: api, face: resolved.face, flags: flags, text: text,
             extent: api.measure(resolved.face, d.font.size, flags, text),
             metrics: api.metrics(resolved.face, d.font.size, flags) };
  }
  function textAlignOffset(d, width) {
    var rtl = d.direction === 'rtl', align = d.textAlign;
    if (align === 'start') align = rtl ? 'right' : 'left';
    else if (align === 'end') align = rtl ? 'left' : 'right';
    return align === 'center' ? -width / 2 : align === 'right' ? -width : 0;
  }
  // How far below y the alphabetic baseline sits, by Chromium's rules: the em
  // box for top, middle and bottom, 80% of the ascent for hanging.
  function textBaselineOffset(d, metrics) {
    var total = metrics.ascent + metrics.descent;
    var emAscent = total > 0 ? d.font.size * metrics.ascent / total : d.font.size * 0.8;
    var emDescent = d.font.size - emAscent;
    switch (d.textBaseline) {
      case 'top': return emAscent;
      case 'hanging': return metrics.ascent * 0.8;
      case 'middle': return (emAscent - emDescent) / 2;
      case 'bottom': case 'ideographic': return -emDescent;
      default: return 0;
    }
  }
  // The colour a fill or stroke paints: its colour, or a gradient's first stop.
  function textColor(d, stroke) {
    var object = stroke ? d.strokeStyleObject : d.fillStyleObject;
    if (object === null) return stroke ? d.stroke : d.fill;
    var stops = gradientStops.get(object);
    if (stops && stops.length > 0) {
      unsupported2d('a gradient (drawn in its first colour)');
      return stops.slice().sort(function (a, b) { return a[0] - b[0]; })[0][1];
    }
    unsupported2d('a pattern');
    return null;
  }

  function drawText2d(state, value, x, y, maxWidth, stroke) {
    var d = state.draw;
    x = Number(x); y = Number(y);
    if (!allFinite(x, y) || (maxWidth !== undefined && !(Number(maxWidth) > 0))) return;
    var color = textColor(d, stroke);
    if (color === null) return;
    if (d.shadowColor[3] > 0 && (d.shadowBlur > 0 || d.shadowOffsetX !== 0 || d.shadowOffsetY !== 0)) unsupported2d('shadows');
    var run = textRun(state, value);
    if (run === null || run.text === '') return;
    var m = d.matrix, scale = Math.sqrt(Math.abs(m[0] * m[3] - m[1] * m[2]));
    if (!(scale > 0)) return;
    if (m[1] !== 0 || m[2] !== 0) unsupported2d('text under a rotating or skewing transform (drawn upright)');
    // Too wide for maxWidth: drawn smaller, where a browser would condense it.
    var fit = maxWidth !== undefined && run.extent.width > Number(maxWidth) ? Number(maxWidth) / run.extent.width : 1;
    var penX = x + textAlignOffset(d, run.extent.width * fit), baseline = y + textBaselineOffset(d, run.metrics);
    var flags = run.flags | (stroke ? (d.lineJoin === 'round' ? 8 : d.lineJoin === 'bevel' ? 16 : 0) : 0);
    var outline = stroke ? Math.max(1, Math.round(d.lineWidth * scale / 2)) : 0;
    var glyphs = run.api.render(run.face, d.font.size * scale * fit, flags, outline, run.text, d.miterLimit);
    if (glyphs === null) return;
    var left = Math.round(m[0] * penX + m[2] * baseline + m[4]) - glyphs.originX;
    var top = Math.round(m[1] * penX + m[3] * baseline + m[5]) - glyphs.baseline;
    var coverage = new Uint8Array(glyphs.data), pixels = state.pixels, width = state.width;
    var alpha = color[3] * d.alpha / 255, mode = d.composite;
    var r = color[0], g = color[1], b = color[2];
    for (var gy = Math.max(0, -top); gy < glyphs.height && top + gy < state.height; gy++) {
      var source = gy * glyphs.width, target = (top + gy) * width + left;
      for (var gx = Math.max(0, -left); gx < glyphs.width && left + gx < width; gx++) {
        var c = coverage[source + gx];
        if (c === 0) continue;
        var i = (target + gx) * 4, sa = c * alpha;
        if (mode !== 'source-over') {
          blendPixel(pixels, i, r, g, b, sa, mode);
        } else if (sa >= 1) {
          // Source-over inline: text is drawn a glyph pixel at a time, and on a
          // slow CPU a call per pixel is most of the cost.
          pixels[i] = r; pixels[i + 1] = g; pixels[i + 2] = b; pixels[i + 3] = 255;
        } else {
          var keep = pixels[i + 3] / 255 * (1 - sa), ao = sa + keep;
          pixels[i] = (r * sa + pixels[i] * keep) / ao;
          pixels[i + 1] = (g * sa + pixels[i + 1] * keep) / ao;
          pixels[i + 2] = (b * sa + pixels[i + 2] * keep) / ao;
          pixels[i + 3] = ao * 255;
        }
      }
    }
  }

  function TextMetrics() {
    throw new TypeError("Failed to construct 'TextMetrics': Illegal constructor");
  }
  global.TextMetrics = TextMetrics;

  function measureText2d(state, value) {
    var d = state.draw, out = Object.create(TextMetrics.prototype), run = textRun(state, value);
    var extent = run === null ? { width: 0, left: 0, right: 0, ascent: 0, descent: 0 } : run.extent;
    var metrics = run === null ? { ascent: 0, descent: 0 } : run.metrics;
    var dx = textAlignOffset(d, extent.width), dy = run === null ? 0 : textBaselineOffset(d, metrics);
    var total = metrics.ascent + metrics.descent;
    var emAscent = total > 0 ? d.font.size * metrics.ascent / total : 0, emDescent = total > 0 ? d.font.size - emAscent : 0;
    // Every distance is from the point (x, y) textAlign and textBaseline name.
    out.width = extent.width;
    out.actualBoundingBoxLeft = extent.left - dx;
    out.actualBoundingBoxRight = extent.right + dx;
    out.actualBoundingBoxAscent = extent.ascent - dy;
    out.actualBoundingBoxDescent = extent.descent + dy;
    out.fontBoundingBoxAscent = metrics.ascent - dy;
    out.fontBoundingBoxDescent = metrics.descent + dy;
    out.emHeightAscent = emAscent - dy;
    out.emHeightDescent = emDescent + dy;
    out.hangingBaseline = metrics.ascent * 0.8 - dy;
    out.alphabeticBaseline = -dy;
    out.ideographicBaseline = -emDescent - dy;
    return out;
  }

  function makeImageData(width, height, data) {
    var image = Object.create(ImageData.prototype);
    image.width = width;
    image.height = height;
    image.data = data || new Uint8ClampedArray(width * height * 4);
    image.colorSpace = 'srgb';
    return image;
  }

  function allFinite() {
    for (var i = 0; i < arguments.length; i++) if (!isFinite(arguments[i])) return false;
    return true;
  }

  define(CanvasRenderingContext2D.prototype, {
    canvas: { get: function () { return context2dOf(this).canvas; } },
    getContextAttributes: function () {
      var a = context2dOf(this).attributes;
      return { alpha: a.alpha, colorSpace: a.colorSpace, desynchronized: a.desynchronized, willReadFrequently: a.willReadFrequently };
    },
    fillStyle: {
      get: function () { var d = context2dOf(this).draw; return d.fillStyleObject || serializeColor(d.fill); },
      set: function (value) {
        var d = context2dOf(this).draw;
        if (value !== null && typeof value === 'object') {
          d.fillStyleObject = value;
          return;
        }
        var color = parseCssColor(value);
        if (color !== null) { d.fill = color; d.fillStyleObject = null; }
      }
    },
    strokeStyle: {
      get: function () { var d = context2dOf(this).draw; return d.strokeStyleObject || serializeColor(d.stroke); },
      set: function (value) {
        var d = context2dOf(this).draw;
        if (value !== null && typeof value === 'object') {
          d.strokeStyleObject = value;
          return;
        }
        var color = parseCssColor(value);
        if (color !== null) { d.stroke = color; d.strokeStyleObject = null; }
      }
    },
    globalAlpha: {
      get: function () { return context2dOf(this).draw.alpha; },
      set: function (value) { var n = Number(value); if (n >= 0 && n <= 1) context2dOf(this).draw.alpha = n; }
    },
    globalCompositeOperation: {
      get: function () { return context2dOf(this).draw.composite; },
      set: function (value) {
        var mode = String(value), d = context2dOf(this).draw;
        if (COMPOSITE_MODES[mode] === 1) {
          d.composite = mode;
        } else if (OTHER_COMPOSITE_MODES[mode] === 1) {
          // A valid mode this subset does not blend: it reads back, and draws as source-over.
          unsupported2d('globalCompositeOperation "' + mode + '"');
          d.composite = mode;
        }
      }
    },
    imageSmoothingEnabled: {
      get: function () { return context2dOf(this).draw.smoothing; },
      set: function (value) { context2dOf(this).draw.smoothing = !!value; }
    },
    imageSmoothingQuality: {
      get: function () { return context2dOf(this).draw.smoothingQuality; },
      set: function (value) {
        var q = String(value);
        if (q === 'low' || q === 'medium' || q === 'high') context2dOf(this).draw.smoothingQuality = q;
      }
    },
    lineWidth: {
      get: function () { return context2dOf(this).draw.lineWidth; },
      set: function (value) { var n = Number(value); if (n > 0 && isFinite(n)) context2dOf(this).draw.lineWidth = n; }
    },
    save: function () {
      var state = context2dOf(this);
      state.stack.push(copyDrawState(state.draw));
    },
    restore: function () {
      var state = context2dOf(this);
      if (state.stack.length > 0) state.draw = state.stack.pop();
    },
    reset: function () {
      var state = context2dOf(this);
      state.pixels.fill(0);
      state.draw = freshDrawState();
      state.stack = [];
    },
    setTransform: function (a, b, c, d, e, f) {
      var state = context2dOf(this);
      if (arguments.length === 0) { state.draw.matrix = [1, 0, 0, 1, 0, 0]; return; }
      if (arguments.length === 1 && a !== null && typeof a === 'object') {
        var m = a;
        state.draw.matrix = [m.a === undefined ? 1 : m.a, m.b || 0, m.c || 0, m.d === undefined ? 1 : m.d, m.e || 0, m.f || 0];
        return;
      }
      var next = [Number(a), Number(b), Number(c), Number(d), Number(e), Number(f)];
      if (allFinite.apply(null, next)) state.draw.matrix = next;
    },
    resetTransform: function () { context2dOf(this).draw.matrix = [1, 0, 0, 1, 0, 0]; },
    getTransform: function () {
      var m = context2dOf(this).draw.matrix;
      return { a: m[0], b: m[1], c: m[2], d: m[3], e: m[4], f: m[5], is2D: true,
               isIdentity: m[0] === 1 && m[1] === 0 && m[2] === 0 && m[3] === 1 && m[4] === 0 && m[5] === 0 };
    },
    transform: function (a, b, c, d, e, f) {
      var state = context2dOf(this), m = state.draw.matrix;
      var n = [Number(a), Number(b), Number(c), Number(d), Number(e), Number(f)];
      if (!allFinite.apply(null, n)) return;
      state.draw.matrix = [m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3],
                           m[1] * n[2] + m[3] * n[3], m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]];
    },
    translate: function (x, y) { this.transform(1, 0, 0, 1, x, y); },
    scale: function (x, y) { this.transform(x, 0, 0, y, 0, 0); },
    rotate: function (angle) {
      var cos = Math.cos(angle), sin = Math.sin(angle);
      this.transform(cos, sin, -sin, cos, 0, 0);
    },
    clearRect: function (x, y, w, h) {
      var state = context2dOf(this);
      x = Number(x); y = Number(y); w = Number(w); h = Number(h);
      if (!allFinite(x, y, w, h)) return;
      if (w < 0) { x += w; w = -w; }
      if (h < 0) { y += h; h = -h; }
      var pixels = state.pixels;
      scanRect(state, x, y, w, h, function (index) {
        pixels[index] = 0; pixels[index + 1] = 0; pixels[index + 2] = 0; pixels[index + 3] = 0;
      });
    },
    fillRect: function (x, y, w, h) {
      var state = context2dOf(this);
      x = Number(x); y = Number(y); w = Number(w); h = Number(h);
      if (!allFinite(x, y, w, h)) return;
      var color = textColor(state.draw, false);
      if (color !== null) fillRect2d(state, x, y, w, h, color);
    },
    strokeRect: function (x, y, w, h) {
      var state = context2dOf(this);
      x = Number(x); y = Number(y); w = Number(w); h = Number(h);
      if (!allFinite(x, y, w, h)) return;
      var color = textColor(state.draw, true);
      if (color === null) return;
      var half = state.draw.lineWidth / 2;
      fillRect2d(state, x - half, y - half, w + 2 * half, 2 * half, color);
      fillRect2d(state, x - half, y + h - half, w + 2 * half, 2 * half, color);
      fillRect2d(state, x - half, y + half, 2 * half, h - 2 * half, color);
      fillRect2d(state, x + w - half, y + half, 2 * half, h - 2 * half, color);
    },
    drawImage: function (image) {
      var state = context2dOf(this);
      var n = arguments.length;
      if (n !== 3 && n !== 5 && n !== 9) {
        throw new TypeError("Failed to execute 'drawImage' on 'CanvasRenderingContext2D': Valid arities are: [3, 5, 9], but " + n + ' arguments provided.');
      }
      var pixels = imagePixels(image);
      if (pixels === null) {
        if (image === null || typeof image !== 'object') {
          throw new TypeError("Failed to execute 'drawImage' on 'CanvasRenderingContext2D': The provided value is not of type '(CanvasImageSource)'.");
        }
        return;  // not loaded yet, or nothing readable: a browser draws nothing
      }
      var args = Array.prototype.slice.call(arguments, 1).map(Number);
      if (!allFinite.apply(null, args)) return;
      if (n === 3) drawImage2d(state, pixels, 0, 0, pixels.width, pixels.height, args[0], args[1], pixels.width, pixels.height);
      else if (n === 5) drawImage2d(state, pixels, 0, 0, pixels.width, pixels.height, args[0], args[1], args[2], args[3]);
      else drawImage2d(state, pixels, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
    },
    createImageData: function (a, b) {
      context2dOf(this);
      if (a !== null && typeof a === 'object') return makeImageData(a.width, a.height);
      var w = Math.abs(Math.trunc(Number(a))), h = Math.abs(Math.trunc(Number(b)));
      if (!(w > 0 && h > 0)) {
        throw domError("Failed to execute 'createImageData' on 'CanvasRenderingContext2D': The source " +
                       (w > 0 ? 'height' : 'width') + ' is 0.', 'IndexSizeError');
      }
      return makeImageData(w, h);
    },
    getImageData: function (sx, sy, sw, sh) {
      var state = context2dOf(this);
      sx = Math.trunc(Number(sx)); sy = Math.trunc(Number(sy)); sw = Math.trunc(Number(sw)); sh = Math.trunc(Number(sh));
      if (!(sw !== 0 && sh !== 0) || !allFinite(sx, sy, sw, sh)) {
        throw domError("Failed to execute 'getImageData' on 'CanvasRenderingContext2D': The source " +
                       (sw === 0 ? 'width' : 'height') + ' is 0.', 'IndexSizeError');
      }
      if (sw < 0) { sx += sw; sw = -sw; }
      if (sh < 0) { sy += sh; sh = -sh; }
      var out = new Uint8ClampedArray(sw * sh * 4), src = state.pixels;
      for (var y = Math.max(0, -sy); y < sh && sy + y < state.height; y++) {
        var left = Math.max(0, -sx), right = Math.min(sw, state.width - sx);
        if (right <= left) continue;
        var from = ((sy + y) * state.width + sx + left) * 4;
        out.set(src.subarray(from, from + (right - left) * 4), (y * sw + left) * 4);
      }
      return makeImageData(sw, sh, out);
    },
    putImageData: function (imageData, dx, dy, dirtyX, dirtyY, dirtyWidth, dirtyHeight) {
      var state = context2dOf(this);
      if (imageData === null || typeof imageData !== 'object' || !imageData.data) {
        throw new TypeError("Failed to execute 'putImageData' on 'CanvasRenderingContext2D': parameter 1 is not of type 'ImageData'.");
      }
      dx = Math.trunc(Number(dx)); dy = Math.trunc(Number(dy));
      var x0 = 0, y0 = 0, x1 = imageData.width, y1 = imageData.height;
      if (arguments.length >= 7) {
        var rx = Math.trunc(Number(dirtyX)), ry = Math.trunc(Number(dirtyY));
        var rw = Math.trunc(Number(dirtyWidth)), rh = Math.trunc(Number(dirtyHeight));
        if (rw < 0) { rx += rw; rw = -rw; }
        if (rh < 0) { ry += rh; rh = -rh; }
        x0 = Math.max(0, rx); y0 = Math.max(0, ry);
        x1 = Math.min(imageData.width, rx + rw); y1 = Math.min(imageData.height, ry + rh);
      }
      var src = imageData.data, out = state.pixels;
      for (var y = y0; y < y1; y++) {
        var ty = dy + y;
        if (ty < 0 || ty >= state.height) continue;
        var left = Math.max(x0, -dx), right = Math.min(x1, state.width - dx);
        if (right <= left) continue;
        var from = (y * imageData.width + left) * 4;
        out.set(src.subarray(from, from + (right - left) * 4), (ty * state.width + dx + left) * 4);
      }
    },
    fillText: function (text, x, y, maxWidth) { drawText2d(context2dOf(this), text, x, y, maxWidth, false); },
    strokeText: function (text, x, y, maxWidth) { drawText2d(context2dOf(this), text, x, y, maxWidth, true); },
    measureText: function (text) { return measureText2d(context2dOf(this), text); },
    font: {
      get: function () { return context2dOf(this).draw.font.text; },
      set: function (value) { var font = parseFont(value); if (font !== null) context2dOf(this).draw.font = font; }
    },
    // The unsupported half: present, drawing nothing, said once.
    createLinearGradient: function () { context2dOf(this); return makeGradient(); },
    createRadialGradient: function () { context2dOf(this); return makeGradient(); },
    createConicGradient: function () { context2dOf(this); return makeGradient(); },
    createPattern: function () { context2dOf(this); unsupported2d('createPattern'); return new CanvasPattern(); },
    getLineDash: function () { return context2dOf(this).draw.lineDash || []; },
    setLineDash: function (segments) { context2dOf(this).draw.lineDash = Array.prototype.slice.call(segments || []); },
    isPointInPath: function () { context2dOf(this); unsupported2d('isPointInPath'); return false; },
    isPointInStroke: function () { context2dOf(this); unsupported2d('isPointInStroke'); return false; }
  });
  ['beginPath', 'closePath', 'moveTo', 'lineTo', 'bezierCurveTo', 'quadraticCurveTo', 'arc', 'arcTo', 'ellipse',
   'rect', 'roundRect', 'fill', 'stroke', 'clip', 'drawFocusIfNeeded'].forEach(function (name) {
    CanvasRenderingContext2D.prototype[name] = function () {
      context2dOf(this);
      unsupported2d(name);
    };
  });
  // No letterSpacing or wordSpacing: Pixi and Phaser space letters themselves
  // when the context has no such property, and this one draws a run in one go.
  ['lineCap', 'lineJoin', 'miterLimit', 'lineDashOffset', 'textAlign', 'textBaseline', 'direction',
   'shadowBlur', 'shadowOffsetX', 'shadowOffsetY', 'filter', 'fontKerning', 'textRendering'].forEach(function (name) {
    Object.defineProperty(CanvasRenderingContext2D.prototype, name, {
      get: function () { var d = context2dOf(this).draw; return d[name] === undefined ? '' : d[name]; },
      set: function (value) { context2dOf(this).draw[name] = typeof value === 'number' ? value : String(value); },
      enumerable: true, configurable: true
    });
  });
  Object.defineProperty(CanvasRenderingContext2D.prototype, 'shadowColor', {
    get: function () { return serializeColor(context2dOf(this).draw.shadowColor); },
    set: function (value) { var c = parseCssColor(value); if (c !== null) context2dOf(this).draw.shadowColor = c; },
    enumerable: true, configurable: true
  });

  // Gradients keep their stops; what is drawn with one uses the first colour.
  var gradientStops = new WeakMap();
  function CanvasGradient() {
    throw new TypeError("Failed to construct 'CanvasGradient': Illegal constructor");
  }
  function makeGradient() {
    var gradient = Object.create(CanvasGradient.prototype);
    gradientStops.set(gradient, []);
    return gradient;
  }
  CanvasGradient.prototype.addColorStop = function (offset, color) {
    var stops = gradientStops.get(this);
    if (!stops) throw new TypeError('Illegal invocation');
    var parsed = parseCssColor(color);
    if (parsed === null) {
      throw domError("Failed to execute 'addColorStop' on 'CanvasGradient': The value provided ('" + color +
                     "') could not be parsed as a color.", 'SyntaxError');
    }
    stops.push([Number(offset), parsed]);
  };
  global.CanvasGradient = CanvasGradient;
  function CanvasPattern() {}
  CanvasPattern.prototype.setTransform = function () {};
  global.CanvasPattern = CanvasPattern;



  // -------------------------------------------------------------------------
  // Intl where the engine's is a placeholder
  //
  // Hermes on Linux gets Intl from ICU, but its ICU backend leaves two things
  // unwritten: toLocaleUpperCase/toLocaleLowerCase answer "uppered"/"lowered",
  // and NumberFormat (behind Number#toLocaleString too) prints C's "%f". Where
  // the engine does that, this puts in plain implementations -- the root
  // locale's case mapping, and en-US-style decimal, percent and grouping. Apple
  // and Android Hermes are unaffected: the checks find nothing to replace.
  // -------------------------------------------------------------------------

  function defineBuiltin(target, name, value) {
    Object.defineProperty(target, name, { value: value, writable: true, enumerable: false, configurable: true });
  }

  if ('a'.toLocaleUpperCase() !== 'A' || 'A'.toLocaleLowerCase() !== 'a') {
    defineBuiltin(String.prototype, 'toLocaleUpperCase', function () { return String.prototype.toUpperCase.call(this); });
    defineBuiltin(String.prototype, 'toLocaleLowerCase', function () { return String.prototype.toLowerCase.call(this); });
  }

  if (typeof global.Intl === 'object' && global.Intl !== null && typeof global.Intl.NumberFormat === 'function' &&
      new global.Intl.NumberFormat('en-US').format(1234.5) !== '1,234.5') {
    var PlainNumberFormat = function NumberFormat(locales, options) {
      if (!(this instanceof PlainNumberFormat)) return new PlainNumberFormat(locales, options);
      var o = options || {};
      var style = o.style === 'percent' ? 'percent' : 'decimal';
      var minimum = o.minimumFractionDigits !== undefined ? Math.min(20, Number(o.minimumFractionDigits) >>> 0) : 0;
      var maximum = o.maximumFractionDigits !== undefined ? Math.min(20, Number(o.maximumFractionDigits) >>> 0)
                                                          : Math.max(minimum, style === 'percent' ? 0 : 3);
      var grouping = o.useGrouping !== false;
      var resolved = { locale: 'en-US', numberingSystem: 'latn', style: style, minimumIntegerDigits: 1,
                       minimumFractionDigits: minimum, maximumFractionDigits: Math.max(minimum, maximum),
                       useGrouping: grouping };
      defineBuiltin(this, 'resolvedOptions', function () { return Object.assign({}, resolved); });
      defineBuiltin(this, 'format', function (value) {
        var n = Number(value) * (style === 'percent' ? 100 : 1);
        if (n !== n) return 'NaN';
        if (!isFinite(n)) return (n < 0 ? '-' : '') + '∞';
        var fixed = Math.abs(n).toFixed(resolved.maximumFractionDigits).split('.');
        var fraction = (fixed[1] || '').replace(/0+$/, '');
        while (fraction.length < minimum) fraction += '0';
        var whole = grouping ? fixed[0].replace(/\B(?=(\d{3})+(?!\d))/g, ',') : fixed[0];
        var negative = n < 0 && /[1-9]/.test(fixed.join(''));
        return (negative ? '-' : '') + whole + (fraction ? '.' + fraction : '') + (style === 'percent' ? '%' : '');
      });
    };
    defineBuiltin(PlainNumberFormat.prototype, 'formatToParts', function (value) {
      return [{ type: 'literal', value: this.format(value) }];
    });
    defineBuiltin(PlainNumberFormat, 'supportedLocalesOf', function () { return ['en-US']; });
    defineBuiltin(global.Intl, 'NumberFormat', PlainNumberFormat);
    defineBuiltin(Number.prototype, 'toLocaleString', function (locales, options) {
      return new PlainNumberFormat(locales, options).format(Number.prototype.valueOf.call(this));
    });
  }

  // -------------------------------------------------------------------------
  // Environment: window, self, location, performance
  //
  // Evidence: `window.devicePixelRatio`, `window.removeEventListener("resize")`,
  // `window.dispatchEvent`, `self.location`, `self.postMessage`, `location.hash`,
  // `performance.now` x345 in a six-second trace.
  // -------------------------------------------------------------------------

  // A TV panel has no separate CSS pixel: the drawable is the screen, and
  // Lightning multiplies by this to size its buffers. Claiming anything but 1
  // would double-scale everything.
  var DEVICE_PIXEL_RATIO = 1;

  // The document's address. There is no document to navigate to, so the one
  // navigation that exists is the one that stays in the document: the
  // fragment. Hash routers -- Blits, Lightning's Router -- set `location.hash`
  // and wait for `hashchange` on window, so that has to behave as in a browser:
  // the new hash reads back at once, `hashchange` follows as a task with
  // oldURL / newURL, and setting the hash it already has fires nothing. Every
  // other part of the address is empty rather than invented, and a navigation
  // that would leave the document is ignored with a warning.
  var LOCATION_BASE = 'screenkit:/';
  var fragment = null; // null: no '#' at all; '': a bare '#'

  // The URL fragment percent-encode set: C0 controls, space, ", <, >, `.
  function encodeFragment(value) {
    return String(value).replace(/[\u0000-\u0020"<>`\u007f]/g, function (c) {
      return '%' + (c.charCodeAt(0) < 16 ? '0' : '') + c.charCodeAt(0).toString(16).toUpperCase();
    });
  }
  function currentHref() {
    return fragment === null ? LOCATION_BASE : LOCATION_BASE + '#' + fragment;
  }
  function navigateToFragment(next) {
    if (next === fragment) return;
    var oldURL = currentHref();
    fragment = next;
    var newURL = currentHref();
    global.setTimeout(function () {
      global.dispatchEvent(new global.HashChangeEvent('hashchange', { oldURL: oldURL, newURL: newURL }));
    }, 0);
  }
  // `assign`, `replace` and the `href` setter: a '#fragment' (or this same
  // address with one) is a fragment navigation; anything else would unload the
  // app, which is not a thing a TV runtime does on a script's say-so.
  function navigate(url, how) {
    var value = String(url);
    if (value.charAt(0) === '#') return navigateToFragment(encodeFragment(value.slice(1)));
    if (value.indexOf(LOCATION_BASE + '#') === 0) {
      return navigateToFragment(encodeFragment(value.slice(LOCATION_BASE.length + 1)));
    }
    if (value === LOCATION_BASE) return navigateToFragment(null);
    global.console.warn('ScreenKit: location.' + how + '(' + JSON.stringify(value) + ') was ignored. ' +
        'Only fragment (#) navigation exists; there is no other document to load.');
  }

  var LOCATION = {
    protocol: 'screenkit:', host: '', hostname: '', port: '',
    pathname: '/', search: '', origin: 'screenkit://',
    toString: function () { return currentHref(); },
    assign: function (url) { navigate(url, 'assign'); },
    replace: function (url) { navigate(url, 'replace'); },
    reload: function () {}
  };
  Object.defineProperty(LOCATION, 'href', {
    enumerable: true,
    get: currentHref,
    set: function (url) { navigate(url, 'href ='); }
  });
  Object.defineProperty(LOCATION, 'hash', {
    enumerable: true,
    get: function () { return fragment ? '#' + fragment : ''; },
    set: function (value) {
      var input = String(value);
      if (input.charAt(0) === '#') input = input.slice(1);
      navigateToFragment(encodeFragment(input));
    }
  });

  global.location = LOCATION;

  // performance.now must be monotonic and must not jump when the wall clock
  // does. Date.now is the only clock reachable from plain JS here, so this is
  // millisecond-resolution and anchored at prelude load; rAF callbacks already
  // receive a finer timestamp from the native loop, which is what frame pacing
  // should use.
  var TIME_ORIGIN = Date.now();
  global.performance = {
    timeOrigin: TIME_ORIGIN,
    now: function () { return Date.now() - TIME_ORIGIN; }
  };

  // `window` is the global object, as in a browser. A separate object that
  // copied a few globals looked equivalent and was not: code written against
  // `window.URL`, or a library that takes `this || window` as its root, found
  // nothing there -- shaka-player died on `window.URL.createObjectURL` at import.
  global.window = global;
  global.devicePixelRatio = DEVICE_PIXEL_RATIO;
  // `window` is an EventTarget too, and the end of every event path in the
  // document. It is the global object, so these are own functions bound to it
  // rather than inherited: `addEventListener('resize', f)` called bare works.
  global.addEventListener = function (type, listener, options) { addListener(global, type, listener, options); };
  global.removeEventListener = function (type, listener, options) { removeListener(global, type, listener, options); };
  global.dispatchEvent = function (event) { return dispatchNow(global, event); };
  defineEventHandlers(global, GLOBAL_EVENT_HANDLERS.concat(WINDOW_EVENT_HANDLERS));
  // There is no second browsing context to *open*: the one way to a second one
  // is an <iframe>, which is an Instance (see below). A blocked popup returns
  // null, which is what a browser gives a script that did not ask from a user
  // gesture, so a page that feature-detects windows finds none.
  global.open = function () { return null; };
  // A TV app closes itself the way a web app does. A host that can leave -- on
  // Android the activity finishes (runtime/android/jni/HostMain.cpp) -- defines
  // `__screenkitClose` before the app runs; where there is nowhere to go, as on
  // a Raspberry Pi running one app, this does nothing, which is also what a
  // browser does for a window a script did not open.
  global.close = function () {
    if (typeof global.__screenkitClose === 'function') global.__screenkitClose();
  };

  // The viewport is the drawable, read live: captured once, these went stale on
  // the first resize while `canvas.width` followed the surface. [Replaceable], as
  // in a browser: an assignment replaces the accessor with the assigned value
  // rather than throwing or being ignored.
  function liveViewport(name, read) {
    Object.defineProperty(global, name, {
      get: read,
      set: function (value) {
        Object.defineProperty(this, name,
          { value: value, writable: true, enumerable: true, configurable: true });
      },
      enumerable: true,
      configurable: true
    });
  }
  liveViewport('innerWidth', drawableWidth);
  liveViewport('innerHeight', drawableHeight);

  // screen: the display is the drawable, landscape, 24-bit. The orientation is a
  // real EventTarget -- Phaser's scale manager listens on it -- that never
  // changes, because a TV does not rotate.
  function ScreenOrientation() { throw new TypeError("Failed to construct 'ScreenOrientation': Illegal constructor"); }
  inherit(ScreenOrientation, EventTarget);
  define(ScreenOrientation.prototype, {
    type: { get: function () { return 'landscape-primary'; } },
    angle: { get: function () { return 0; } },
    lock: function () {
      return Promise.reject(domError("screen.orientation.lock() is not available: a TV does not rotate.", 'NotSupportedError'));
    },
    unlock: function () {}
  });
  defineEventHandlers(ScreenOrientation.prototype, ['change']);
  global.ScreenOrientation = ScreenOrientation;
  var screenOrientation = Object.create(ScreenOrientation.prototype);
  function Screen() { throw new TypeError("Failed to construct 'Screen': Illegal constructor"); }
  inherit(Screen, EventTarget);
  define(Screen.prototype, {
    width: { get: drawableWidth },
    height: { get: drawableHeight },
    availWidth: { get: drawableWidth },
    availHeight: { get: drawableHeight },
    availLeft: { get: function () { return 0; } },
    availTop: { get: function () { return 0; } },
    colorDepth: { get: function () { return 24; } },
    pixelDepth: { get: function () { return 24; } },
    orientation: { get: function () { return screenOrientation; } }
  });
  global.Screen = Screen;
  global.screen = Object.create(Screen.prototype);

  // The host's window changed pixel size (ViewportEvents, native). `resize`
  // fires at window, as in a browser, when the size a page can read differs
  // from the one it last heard about -- a drag that ends where it began fires
  // nothing. The dispatch comes back as a stepper, like a key's, so native
  // runs a microtask checkpoint after each listener; null when there is none.
  var viewportAnnounced = drawableWidth() + 'x' + drawableHeight();
  global.__screenkitResize = function () {
    var size = drawableWidth() + 'x' + drawableHeight();
    if (size === viewportAnnounced) return null;
    viewportAnnounced = size;
    var event = new Event('resize');
    event.isTrusted = true;
    return dispatcher(global, event);
  };

  // navigator: what a bundle reads to sniff its platform. Blits' example app
  // matches `navigator.userAgent` against 'Tizen', 'WebOS' and 'WPE Sky' while it
  // launches, and a missing navigator was a TypeError before the first frame.
  // The values say what this is -- a ScreenKit runtime with no touch, one JS
  // thread and no network state to report -- rather than impersonate a browser.
  // `getGamepads` is the Gamepad API (see Gamepads, below).
  global.navigator = {
    userAgent: 'Mozilla/5.0 (ScreenKit)',
    appName: 'ScreenKit',
    appVersion: '5.0 (ScreenKit)',
    product: 'Gecko',
    platform: '',
    vendor: '',
    language: 'en-US',
    languages: ['en-US'],
    onLine: true,
    cookieEnabled: false,
    maxTouchPoints: 0,
    hardwareConcurrency: 1,
    getGamepads: getGamepads
  };

  // `self` is the global in a browser, and bundles use it to feature-detect a
  // worker. Aliasing the real global is both simpler and more truthful than a
  // second object that can drift.
  global.self = global;

  // -------------------------------------------------------------------------
  // Events and observers
  //
  // Evidence: `new Event` x1, `new MutationObserver` x2, ResizeObserver
  // referenced. MutationObserver lives with the element tree above. The
  // ResizeObserver registers and never fires: no element here has a layout box
  // whose size could change.
  // -------------------------------------------------------------------------

  function Event(type, init) {
    this.type = String(type);
    this.bubbles = !!(init && init.bubbles);
    this.cancelable = !!(init && init.cancelable);
    this.composed = !!(init && init.composed);
    this.defaultPrevented = false;
    this.target = null;
    this.currentTarget = null;
    this.eventPhase = NONE;
    this.isTrusted = false;
    this.timeStamp = global.performance.now();
  }
  [['NONE', NONE], ['CAPTURING_PHASE', CAPTURING_PHASE], ['AT_TARGET', AT_TARGET],
   ['BUBBLING_PHASE', BUBBLING_PHASE]].forEach(function (constant) {
    Event[constant[0]] = constant[1];
    Event.prototype[constant[0]] = constant[1];
  });
  Event.prototype.preventDefault = function () {
    if (this.cancelable) this.defaultPrevented = true;
  };
  Event.prototype.stopPropagation = function () { this._stopPropagation = true; };
  Event.prototype.stopImmediatePropagation = function () {
    this._stopPropagation = true;
    this._stopImmediate = true;
  };
  // The objects the event is travelling through, target first; empty outside
  // a dispatch.
  Event.prototype.composedPath = function () { return this._dispatching ? this._path.slice() : []; };
  Object.defineProperty(Event.prototype, 'cancelBubble', {
    get: function () { return !!this._stopPropagation; },
    set: function (value) { if (value) this._stopPropagation = true; },
    enumerable: true, configurable: true
  });
  Object.defineProperty(Event.prototype, 'returnValue', {
    get: function () { return !this.defaultPrevented; },
    set: function (value) { if (!value) this.preventDefault(); },
    enumerable: true, configurable: true
  });
  global.Event = Event;

  function CustomEvent(type, init) {
    Event.call(this, type, init);
    this.detail = init && 'detail' in init ? init.detail : null;
  }
  CustomEvent.prototype = Object.create(Event.prototype);
  CustomEvent.prototype.constructor = CustomEvent;
  global.CustomEvent = CustomEvent;

  // What location.hash changes arrive as, on window.
  function HashChangeEvent(type, init) {
    Event.call(this, type, init);
    this.oldURL = init && init.oldURL !== undefined ? String(init.oldURL) : '';
    this.newURL = init && init.newURL !== undefined ? String(init.newURL) : '';
  }
  HashChangeEvent.prototype = Object.create(Event.prototype);
  HashChangeEvent.prototype.constructor = HashChangeEvent;
  global.HashChangeEvent = HashChangeEvent;

  // KeyboardEvent, what remote, keyboard and gamepad input arrive as (see
  // __screenkitKey below). `keyCode` / `which` are taken from the init dict:
  // a browser ignores them there, but TV frameworks read keyCode, and Blits
  // builds its own synthetic events by passing keyCode in init.
  function KeyboardEvent(type, init) {
    Event.call(this, type, init);
    init = init || {};
    this.key = init.key !== undefined ? String(init.key) : '';
    this.code = init.code !== undefined ? String(init.code) : '';
    this.keyCode = init.keyCode | 0;
    this.which = init.which !== undefined ? init.which | 0 : this.keyCode;
    this.charCode = init.charCode | 0;
    this.location = init.location | 0;
    this.repeat = !!init.repeat;
    this.isComposing = !!init.isComposing;
    this.shiftKey = !!init.shiftKey;
    this.ctrlKey = !!init.ctrlKey;
    this.altKey = !!init.altKey;
    this.metaKey = !!init.metaKey;
  }
  KeyboardEvent.prototype = Object.create(Event.prototype);
  KeyboardEvent.prototype.constructor = KeyboardEvent;
  KeyboardEvent.prototype.getModifierState = function (name) {
    return ({ Shift: this.shiftKey, Control: this.ctrlKey, Alt: this.altKey, Meta: this.metaKey })[name] === true;
  };
  KeyboardEvent.DOM_KEY_LOCATION_STANDARD = 0;
  KeyboardEvent.DOM_KEY_LOCATION_LEFT = 1;
  KeyboardEvent.DOM_KEY_LOCATION_RIGHT = 2;
  KeyboardEvent.DOM_KEY_LOCATION_NUMPAD = 3;
  global.KeyboardEvent = KeyboardEvent;

  // What XMLHttpRequest, WebSocket and EventSource dispatch
  // (spec-runtime-networking.md): progress with its byte counts, a message with
  // its data, a WebSocket close with its code and reason.
  function ProgressEvent(type, init) {
    Event.call(this, type, init);
    this.lengthComputable = !!(init && init.lengthComputable);
    this.loaded = init && init.loaded !== undefined ? Number(init.loaded) : 0;
    this.total = init && init.total !== undefined ? Number(init.total) : 0;
  }
  ProgressEvent.prototype = Object.create(Event.prototype);
  ProgressEvent.prototype.constructor = ProgressEvent;
  global.ProgressEvent = ProgressEvent;

  function MessageEvent(type, init) {
    Event.call(this, type, init);
    init = init || {};
    this.data = init.data !== undefined ? init.data : null;
    this.origin = init.origin !== undefined ? String(init.origin) : '';
    this.lastEventId = init.lastEventId !== undefined ? String(init.lastEventId) : '';
    this.source = init.source !== undefined ? init.source : null;
    this.ports = init.ports !== undefined ? init.ports : [];
  }
  MessageEvent.prototype = Object.create(Event.prototype);
  MessageEvent.prototype.constructor = MessageEvent;
  global.MessageEvent = MessageEvent;

  function CloseEvent(type, init) {
    Event.call(this, type, init);
    init = init || {};
    this.wasClean = !!init.wasClean;
    this.code = init.code !== undefined ? Number(init.code) & 0xffff : 0;
    this.reason = init.reason !== undefined ? String(init.reason) : '';
  }
  CloseEvent.prototype = Object.create(Event.prototype);
  CloseEvent.prototype.constructor = CloseEvent;
  global.CloseEvent = CloseEvent;

  // Both observers share a shape: register, never fire, disconnect cleanly.
  // takeRecords returning [] is load-bearing -- callers treat a missing method
  // as a broken observer.
  function makeObserver(name) {
    function Observer(callback) { this._callback = callback; }
    Observer.prototype.observe = function () {};
    Observer.prototype.unobserve = function () {};
    Observer.prototype.disconnect = function () {};
    Observer.prototype.takeRecords = function () { return []; };
    Observer.displayName = name;
    return Observer;
  }
  global.ResizeObserver = makeObserver('ResizeObserver');
  global.IntersectionObserver = makeObserver('IntersectionObserver');



  // -------------------------------------------------------------------------
  // DOMException
  //
  // Not in the Lightning trace, but load-bearing all the same: core-js's
  // `web.dom-exception` polyfill, which @vitejs/plugin-legacy injects, does
  // `NativeDOMException.prototype` unconditionally and throws on an engine that
  // has none -- which is Hermes. A legacy build dies before the first line of app
  // code without this. The legacy codes are kept because callers still branch on
  // them (`e.code === DOMException.ABORT_ERR`).
  // -------------------------------------------------------------------------

  var DOM_EXCEPTION_CODES = {
    IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4,
    InvalidCharacterError: 5, NoModificationAllowedError: 7, NotFoundError: 8,
    NotSupportedError: 9, InvalidStateError: 11, SyntaxError: 12,
    InvalidModificationError: 13, NamespaceError: 14, InvalidAccessError: 15,
    TypeMismatchError: 17, SecurityError: 18, NetworkError: 19, AbortError: 20,
    URLMismatchError: 21, QuotaExceededError: 22, TimeoutError: 23,
    InvalidNodeTypeError: 24, DataCloneError: 25
  };

  if (typeof global.DOMException !== 'function') {
    var DOMException = function DOMException(message, name) {
      var err = new Error(message === undefined ? '' : String(message));
      Object.setPrototypeOf(err, DOMException.prototype);
      Object.defineProperty(err, 'name', { value: name === undefined ? 'Error' : String(name),
                                           writable: true, configurable: true });
      Object.defineProperty(err, 'code', { value: DOM_EXCEPTION_CODES[err.name] || 0,
                                           writable: true, configurable: true });
      return err;
    };
    DOMException.prototype = Object.create(Error.prototype);
    DOMException.prototype.constructor = DOMException;
    var legacy = {
      INDEX_SIZE_ERR: 1, HIERARCHY_REQUEST_ERR: 3, WRONG_DOCUMENT_ERR: 4, INVALID_CHARACTER_ERR: 5,
      NO_MODIFICATION_ALLOWED_ERR: 7, NOT_FOUND_ERR: 8, NOT_SUPPORTED_ERR: 9, INVALID_STATE_ERR: 11,
      SYNTAX_ERR: 12, INVALID_MODIFICATION_ERR: 13, NAMESPACE_ERR: 14, INVALID_ACCESS_ERR: 15,
      TYPE_MISMATCH_ERR: 17, SECURITY_ERR: 18, NETWORK_ERR: 19, ABORT_ERR: 20, URL_MISMATCH_ERR: 21,
      QUOTA_EXCEEDED_ERR: 22, TIMEOUT_ERR: 23, INVALID_NODE_TYPE_ERR: 24, DATA_CLONE_ERR: 25
    };
    for (var code in legacy) {
      DOMException[code] = legacy[code];
      DOMException.prototype[code] = legacy[code];
    }
    global.DOMException = DOMException;
  }

  // -------------------------------------------------------------------------
  // Encoding and binary: Blob, URL, URLSearchParams, atob/btoa, ImageData
  //
  // Evidence: `new Blob` x5, `URL.createObjectURL`/`revokeObjectURL` (proven at
  // runtime), `new URL` x5, `new URLSearchParams` x1, `atob` x1, `new ImageData` x3.
  // All of these are data structures; none needs native support.
  // -------------------------------------------------------------------------

  var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

  // Hermes ships atob, btoa, TextEncoder and TextDecoder natively. Defining them
  // unconditionally would replace a spec-correct native implementation with this
  // slower one, so these are fallbacks only -- installed when the engine lacks them.
  if (typeof global.atob !== 'function') global.atob = function (input) {
    var str = String(input).replace(/[=\s]/g, '');
    var out = '';
    var bits = 0;
    var acc = 0;
    for (var i = 0; i < str.length; i++) {
      var v = B64.indexOf(str.charAt(i));
      if (v < 0) throw new Error('atob: not base64');
      acc = (acc << 6) | v;
      bits += 6;
      if (bits >= 8) { bits -= 8; out += String.fromCharCode((acc >> bits) & 0xff); }
    }
    return out;
  };

  if (typeof global.btoa !== 'function') global.btoa = function (input) {
    var str = String(input);
    var out = '';
    for (var i = 0; i < str.length; i += 3) {
      var c0 = str.charCodeAt(i);
      var c1 = str.charCodeAt(i + 1);
      var c2 = str.charCodeAt(i + 2);
      if (c0 > 255 || c1 > 255 || c2 > 255) throw new Error('btoa: byte out of range');
      var n = (c0 << 16) | ((isNaN(c1) ? 0 : c1) << 8) | (isNaN(c2) ? 0 : c2);
      out += B64.charAt((n >> 18) & 63) + B64.charAt((n >> 12) & 63)
           + (isNaN(c1) ? '=' : B64.charAt((n >> 6) & 63))
           + (isNaN(c2) ? '=' : B64.charAt(n & 63));
    }
    return out;
  };

  // UTF-8, which the File API requires for a string part and which Blob.text()
  // decodes. Hermes has TextEncoder; the fallback is for an engine without one.
  var utf8Encoder = typeof global.TextEncoder === 'function' ? new global.TextEncoder() : null;
  function utf8Bytes(text) {
    var s = String(text);
    if (utf8Encoder) return utf8Encoder.encode(s);
    var out = [];
    for (var i = 0; i < s.length; i++) {
      var c = s.charCodeAt(i);
      if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length && s.charCodeAt(i + 1) >= 0xdc00 && s.charCodeAt(i + 1) <= 0xdfff) {
        c = 0x10000 + ((c - 0xd800) << 10) + (s.charCodeAt(++i) - 0xdc00);
      } else if (c >= 0xd800 && c <= 0xdfff) {
        c = 0xfffd;
      }
      if (c < 0x80) out.push(c);
      else if (c < 0x800) out.push(0xc0 | c >> 6, 0x80 | c & 63);
      else if (c < 0x10000) out.push(0xe0 | c >> 12, 0x80 | c >> 6 & 63, 0x80 | c & 63);
      else out.push(0xf0 | c >> 18, 0x80 | c >> 12 & 63, 0x80 | c >> 6 & 63, 0x80 | c & 63);
    }
    return new Uint8Array(out);
  }

  // Blob keeps its bytes. That matters: the runtime tracer found that Web IDL
  // validates internal slots, and anything that merely *looks* like a Blob is
  // rejected by the APIs that take one. Here the shim owns both sides, so the
  // contract is simply "the bytes are really here".
  function flattenParts(parts) {
    var chunks = [], total = 0;
    for (var i = 0; parts && i < parts.length; i++) {
      var part = parts[i], bytes;
      if (typeof part === 'string') {
        bytes = utf8Bytes(part);
      } else if (part && part._bytes) {
        bytes = part._bytes;
      } else if (part && typeof part.byteLength === 'number') {
        bytes = part.buffer ? new Uint8Array(part.buffer, part.byteOffset || 0, part.byteLength)
                            : new Uint8Array(part);
      } else {
        bytes = utf8Bytes(String(part));
      }
      chunks.push(bytes);
      total += bytes.length;
    }
    var out = new Uint8Array(total), offset = 0;
    for (var j = 0; j < chunks.length; j++) {
      out.set(chunks[j], offset);
      offset += chunks[j].length;
    }
    return out;
  }

  function Blob(parts, options) {
    this._bytes = flattenParts(parts);
    this.size = this._bytes.length;
    var type = options && options.type ? String(options.type) : '';
    // The File API: a type with anything outside U+0020-U+007E is no type. It
    // ends up in multipart part headers, where a CR/LF would forge a header.
    this.type = /[^\u0020-\u007e]/.test(type) ? '' : type;
  }
  Blob.prototype.slice = function (start, end, type) {
    var sub = this._bytes.slice(start || 0, end === undefined ? this.size : end);
    var out = new Blob([], { type: type || this.type });
    out._bytes = sub;
    out.size = sub.length;
    return out;
  };
  Blob.prototype.arrayBuffer = function () { return Promise.resolve(this._bytes.slice().buffer); };
  Blob.prototype.bytes = function () { return Promise.resolve(this._bytes.slice()); };
  Blob.prototype.text = function () { return Promise.resolve(bytesToText(this._bytes)); };
  global.Blob = Blob;

  // Object URLs are a handle to a Blob this runtime already holds. Nothing
  // fetches them over the network, so the registry *is* the implementation.
  var objectUrls = Object.create(null);
  var nextObjectUrl = 1;

  var SCHEME = /^[a-zA-Z][a-zA-Z0-9+.-]*:/;

  // RFC 3986 5.2.4: `.` and `..` segments out of a resolved path.
  function removeDotSegments(path) {
    var segments = path.split('/'), out = [];
    for (var i = 0; i < segments.length; i++) {
      var segment = segments[i], last = i === segments.length - 1;
      if (segment === '.' || segment === '..') {
        if (segment === '..' && out.length > 1) out.pop();
        if (last) out.push('');
        continue;
      }
      out.push(segment);
    }
    return out.join('/');
  }

  // A relative reference against an absolute base, RFC 3986 5.2.2. The base's
  // query and fragment never take part: Lightning resolves every texture URL
  // with `new URL(src, location.href)`, and once a hash router has put
  // `#/intro` in the href, cutting the base at its last '/' landed inside the
  // fragment and every image loaded after the first navigation 404'd.
  function resolveReference(ref, base) {
    var b = parseUrl(base);
    var withoutFragment = b.href.replace(/#.*$/, '');
    if (ref === '') return withoutFragment;
    if (ref.charAt(0) === '#') return withoutFragment + ref;
    if (ref.slice(0, 2) === '//') return b.protocol + ref;
    var authority = b.href.slice(b.protocol.length, b.protocol.length + 2) === '//' ? '//' + b.host : '';
    var m = /^([^?#]*)(.*)$/.exec(ref);
    var path = m[1], tail = m[2];
    if (path === '') return b.protocol + authority + b.pathname + tail;  // '?query'
    if (path.charAt(0) !== '/') {
      path = (authority && b.pathname === '' ? '/' : b.pathname.replace(/[^/]*$/, '')) + path;
    }
    return b.protocol + authority + removeDotSegments(path) + tail;
  }

  function parseUrl(input, base) {
    var href = String(input);
    if (base !== undefined && !SCHEME.test(href)) href = resolveReference(href, String(base));
    var m = /^([a-zA-Z][a-zA-Z0-9+.-]*:)\/\/([^/?#]*)?([^?#]*)(\?[^#]*)?(#.*)?$/.exec(href);
    if (!m) {
      // No authority -- `screenkit:/assets/x.png`, this runtime's own href. The
      // query and fragment still split off the path.
      var noAuthority = /^([a-zA-Z][a-zA-Z0-9+.-]*:)([^?#]*)(\?[^#]*)?(#.*)?$/.exec(href);
      m = noAuthority ? [href, noAuthority[1], '', noAuthority[2], noAuthority[3], noAuthority[4]] : null;
    }
    if (!m) throw new TypeError('Invalid URL: ' + href);
    var host = m[2] || '';
    return {
      href: href, protocol: m[1], host: host,
      hostname: host.split(':')[0], port: host.split(':')[1] || '',
      pathname: m[3] || '', search: m[4] || '', hash: m[5] || '',
      origin: host ? m[1] + '//' + host : 'null'
    };
  }

  function URLShim(input, base) {
    var parts = parseUrl(input, base);
    for (var k in parts) if (Object.prototype.hasOwnProperty.call(parts, k)) this[k] = parts[k];
    this.searchParams = new URLSearchParams(this.search);
  }
  URLShim.prototype.toString = function () { return this.href; };
  URLShim.prototype.toJSON = function () { return this.href; };
  URLShim.createObjectURL = function (blob) {
    var url = 'blob:screenkit/' + (nextObjectUrl++);
    objectUrls[url] = blob;
    return url;
  };
  URLShim.revokeObjectURL = function (url) { delete objectUrls[String(url)]; };
  // Not web API, but the image and fetch shims need to resolve a blob: URL back
  // to its bytes without reaching into this closure.
  URLShim._resolveObjectURL = function (url) { return objectUrls[String(url)] || null; };
  global.URL = URLShim;
  global.webkitURL = URLShim;

  function URLSearchParams(init) {
    this._pairs = [];
    if (typeof init === 'string') {
      var q = init.charAt(0) === '?' ? init.slice(1) : init;
      if (q) {
        var parts = q.split('&');
        for (var i = 0; i < parts.length; i++) {
          if (!parts[i]) continue;
          var eq = parts[i].indexOf('=');
          var k = eq < 0 ? parts[i] : parts[i].slice(0, eq);
          var v = eq < 0 ? '' : parts[i].slice(eq + 1);
          this._pairs.push([decodeURIComponent(k.replace(/\+/g, ' ')),
                            decodeURIComponent(v.replace(/\+/g, ' '))]);
        }
      }
    } else if (init && typeof init === 'object') {
      for (var key in init) {
        if (Object.prototype.hasOwnProperty.call(init, key)) this._pairs.push([key, String(init[key])]);
      }
    }
  }
  URLSearchParams.prototype.get = function (name) {
    for (var i = 0; i < this._pairs.length; i++) if (this._pairs[i][0] === name) return this._pairs[i][1];
    return null;
  };
  URLSearchParams.prototype.getAll = function (name) {
    var out = [];
    for (var i = 0; i < this._pairs.length; i++) if (this._pairs[i][0] === name) out.push(this._pairs[i][1]);
    return out;
  };
  URLSearchParams.prototype.has = function (name) { return this.get(name) !== null; };
  URLSearchParams.prototype.set = function (name, value) {
    for (var i = 0; i < this._pairs.length; i++) {
      if (this._pairs[i][0] === name) { this._pairs[i][1] = String(value); return; }
    }
    this._pairs.push([String(name), String(value)]);
  };
  URLSearchParams.prototype.append = function (name, value) {
    this._pairs.push([String(name), String(value)]);
  };
  URLSearchParams.prototype['delete'] = function (name) {
    this._pairs = this._pairs.filter(function (p) { return p[0] !== name; });
  };
  URLSearchParams.prototype.forEach = function (fn, thisArg) {
    for (var i = 0; i < this._pairs.length; i++) fn.call(thisArg, this._pairs[i][1], this._pairs[i][0], this);
  };
  URLSearchParams.prototype.toString = function () {
    return this._pairs.map(function (p) {
      return encodeURIComponent(p[0]) + '=' + encodeURIComponent(p[1]);
    }).join('&');
  };
  // Iteration. The Blits router turns query parameters into an object with
  // `[...params.entries()]`; without these that spread threw inside a promise and
  // the app mounted nothing, with no error anywhere.
  function pairIterator(pairs, pick) {
    var snapshot = pairs.map(pick);
    var i = 0;
    var it = { next: function () {
      return i < snapshot.length ? { value: snapshot[i++], done: false } : { value: undefined, done: true };
    } };
    it[Symbol.iterator] = function () { return this; };
    return it;
  }
  URLSearchParams.prototype.entries = function () { return pairIterator(this._pairs, function (p) { return [p[0], p[1]]; }); };
  URLSearchParams.prototype.keys = function () { return pairIterator(this._pairs, function (p) { return p[0]; }); };
  URLSearchParams.prototype.values = function () { return pairIterator(this._pairs, function (p) { return p[1]; }); };
  URLSearchParams.prototype[Symbol.iterator] = URLSearchParams.prototype.entries;
  Object.defineProperty(URLSearchParams.prototype, 'size', {
    get: function () { return this._pairs.length; }, configurable: true
  });
  global.URLSearchParams = URLSearchParams;

  // ImageData is a struct: a byte buffer and its dimensions. texImage2D uploads
  // its `data` (the seam in runtime/core/src/gfx/VendoredWebGL.cpp; upstream
  // expo-gl only decoded files and uploaded nothing for it).
  function ImageData(a, b, c) {
    if (typeof a === 'number') {
      this.width = a; this.height = b;
      this.data = new Uint8ClampedArray(a * b * 4);
    } else {
      this.data = a; this.width = b;
      this.height = c === undefined ? (a.length / 4) / b : c;
    }
    this.colorSpace = 'srgb';
  }
  global.ImageData = ImageData;


  // -------------------------------------------------------------------------
  // Loading: fetch, XMLHttpRequest, WebSocket, EventSource, images
  //
  // Evidence: Lightning's texture loader is
  //   new XMLHttpRequest -> open("GET", url) -> responseType = "blob" -> onload
  //   -> createImageBitmap(blob, {premultiplyAlpha, ...}) -> texImage2D
  // and XMLHttpRequest's proven surface is open, send, responseType, response,
  // status, statusText, readyState, onload, onerror, onreadystatechange. The
  // rest -- streams, Headers/Request/Response, AbortController, FormData, the
  // whole of XMLHttpRequest, WebSocket, EventSource -- is the networking spec's
  // matrix (spec-runtime-networking.md), each row a `net-*` ctest.
  //
  // Two sources of bytes, chosen by URL:
  //   http:/https:/ws:/wss: -> __screenkit.net, the native handle API over the OS
  //     network stack (runtime/core/src/net). Every request and socket is one
  //     id; its events arrive as event-loop tasks.
  //   anything else -> the package, through __screenkit.readFile, confined to the
  //     asset root in C++, exactly as before networking existed.
  //
  // The browser semantics are all here, in JS: the native side moves bytes and
  // reports head/data/end/error, and this file turns that into promises,
  // streams, readyState machines and events in the orders their specs define.
  // -------------------------------------------------------------------------

  var io = global.__screenkit;
  var net = io && io.net ? io.net : null;

  function isNetworkUrl(href) { return /^https?:\/\//i.test(String(href)); }

  // A URL a bundle hands us, as something a loader can open.
  // Returns { blob } for an object URL, { network } for http(s), { path } for an
  // asset, or throws.
  // A data: URL's bytes and type (RFC 2397): base64, or percent-encoded text.
  function dataUrlBlob(href) {
    var comma = href.indexOf(',');
    if (comma < 0) throw new TypeError('malformed data: URL');
    var meta = href.slice(5, comma), body = href.slice(comma + 1);
    var base64 = /;base64$/i.test(meta);
    var type = (base64 ? meta.slice(0, -7) : meta).split(';')[0] || 'text/plain';
    var bytes;
    if (base64) {
      var binary = global.atob(decodeURIComponent(body).replace(/\s+/g, ''));
      bytes = new Uint8Array(binary.length);
      for (var i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
    } else {
      bytes = new TextEncoder().encode(decodeURIComponent(body));
    }
    return blobFromBytes(bytes, type.toLowerCase());
  }

  function resolveResource(url) {
    var href = String(url);
    if (/^data:/i.test(href)) return { blob: dataUrlBlob(href) };
    if (href.indexOf('blob:') === 0) {
      var blob = global.URL._resolveObjectURL(href);
      if (!blob) throw new Error('unknown or revoked object URL: ' + href);
      return { blob: blob };
    }
    if (isNetworkUrl(href)) return { network: href };
    if (href.indexOf('file://') === 0) return { path: decodeURI(href.slice(7)).split(/[?#]/)[0] };
    // This runtime's own origin. `location.origin` is "screenkit://", so a bundle
    // that builds absolute URLs the web way -- `new URL(path, location.href)` --
    // hands back `screenkit://fonts/x.png`. Blits does exactly that for the MSDF
    // atlas PNG; unrecognised, it reached the reader verbatim, 404'd, failed the
    // font, and stalled rendering with a black screen and no error but the font's.
    if (/^screenkit:/i.test(href)) href = href.replace(/^screenkit:\/*/i, '/');
    var bare = href.split(/[?#]/)[0];
    // A leading slash is *site-root*-relative on the web, and the asset root is
    // the site root here. Passing `/fonts/x` through unchanged made it a
    // filesystem-absolute path, which the confined reader correctly refused -- so
    // Blits' `/fonts/Lato-Regular.ttf` 404'd and every piece of text silently
    // failed to load while the app reported no error at all. `//host/x` is
    // protocol-relative, i.e. network, and stays refused.
    if (bare.charAt(0) === '/' && bare.charAt(1) !== '/') bare = bare.replace(/^\/+/, '');
    return { path: bare };
  }

  // A Blob that remembers which asset it came from. That path is what lets
  // createImageBitmap hand texImage2D something it can decode from a file; a
  // Blob of downloaded bytes is decoded from memory instead.
  function blobFromAsset(path, type) {
    var buffer = io.readFile(path);
    var blob = blobFromBytes(new Uint8Array(buffer), type || mimeFor(path));
    blob._sourcePath = path;
    return blob;
  }

  function blobFromBytes(bytes, type) {
    var blob = new global.Blob([], { type: type || '' });
    blob._bytes = bytes;
    blob.size = bytes.length;
    return blob;
  }

  function mimeFor(path) {
    var ext = String(path).split('.').pop().toLowerCase();
    return ({ png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif',
              webp: 'image/webp', json: 'application/json', js: 'text/javascript',
              txt: 'text/plain', ttf: 'font/ttf', otf: 'font/otf', woff: 'font/woff' })[ext]
           || 'application/octet-stream';
  }

  // UTF-8, through the engine's own TextDecoder, which also drops a leading BOM
  // as the Encoding spec's "UTF-8 decode" does. The byte-string fallback is for
  // an engine without one.
  var utf8 = typeof global.TextDecoder === 'function' ? new global.TextDecoder('utf-8') : null;
  function bytesToText(bytes) {
    if (utf8) return utf8.decode(bytes);
    var s = '';
    for (var i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
    return s;
  }

  function concatBytes(chunks, total) {
    if (chunks.length === 1 && chunks[0].length === total) return chunks[0];
    var out = new Uint8Array(total), offset = 0;
    for (var i = 0; i < chunks.length; i++) {
      out.set(chunks[i], offset);
      offset += chunks[i].length;
    }
    return out;
  }

  // A copy of the bytes behind an ArrayBuffer or any view of one.
  function copyBytes(value) {
    if (value instanceof ArrayBuffer) return new Uint8Array(value.slice(0));
    return new Uint8Array(value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength));
  }
  function isBufferSource(value) {
    return value instanceof ArrayBuffer || (value !== null && typeof value === 'object' &&
      value.buffer instanceof ArrayBuffer && typeof value.byteLength === 'number');
  }

  // Callbacks fire on a later task, never synchronously inside send(). A bundle
  // that assigns onload *after* send() -- common and legal -- would otherwise
  // miss it.
  function later(fn) { global.setTimeout(fn, 0); }

  function networkError(message) {
    return new TypeError(message);
  }
  function noNetwork() {
    sandboxRefusal('allow-network', 'fetch, XMLHttpRequest, WebSocket and EventSource');
    return networkError('no network in this runtime: __screenkit.net is missing');
  }

  // --- ReadableStream ---------------------------------------------------------
  //
  // The default (non-byte) stream: a controller, a default reader with read /
  // cancel / releaseLock / closed, tee, and async iteration. Byte streams, BYOB
  // readers, pipeTo/pipeThrough and the Writable/Transform streams they pipe to
  // are absent (runtime/js/README.md), so a feature check for them fails honestly.

  var streamStates = new WeakMap();
  var readerStates = new WeakMap();
  var controllerStates = new WeakMap();
  function noop() {}

  function streamState(stream, method) {
    var state = stream !== null && typeof stream === 'object' ? streamStates.get(stream) : undefined;
    if (!state) throw new TypeError("Failed to execute '" + method + "' on 'ReadableStream': Illegal invocation");
    return state;
  }

  function ReadableStream(source, strategy) {
    if (!(this instanceof ReadableStream)) {
      throw new TypeError("Failed to construct 'ReadableStream': Please use the 'new' operator.");
    }
    if (source === undefined || source === null) source = {};
    if (typeof source !== 'object' && typeof source !== 'function') {
      throw new TypeError("Failed to construct 'ReadableStream': the underlying source must be an object");
    }
    if (source.type !== undefined) {
      if (String(source.type) === 'bytes') {
        throw new RangeError("Failed to construct 'ReadableStream': byte streams (type: 'bytes') are not supported in ScreenKit");
      }
      throw new TypeError("Failed to construct 'ReadableStream': invalid type " + JSON.stringify(String(source.type)));
    }
    strategy = strategy || {};
    var hwm = strategy.highWaterMark === undefined ? 1 : Number(strategy.highWaterMark);
    if (isNaN(hwm) || hwm < 0) throw new RangeError("Failed to construct 'ReadableStream': invalid highWaterMark");
    var state = {
      stream: this, source: source, state: 'readable', error: undefined, disturbed: false,
      queue: [], queueTotal: 0, hwm: hwm, size: typeof strategy.size === 'function' ? strategy.size : null,
      reader: null, started: false, pulling: false, pullAgain: false, closeRequested: false, controller: null
    };
    streamStates.set(this, state);
    var controller = Object.create(ReadableStreamDefaultController.prototype);
    controllerStates.set(controller, state);
    state.controller = controller;
    var started = typeof source.start === 'function' ? source.start.call(source, controller) : undefined;
    Promise.resolve(started).then(function () {
      state.started = true;
      pullIfNeeded(state);
    }, function (e) { errorStream(state, e); });
  }

  function desiredSize(state) {
    if (state.state === 'errored') return null;
    if (state.state === 'closed') return 0;
    return state.hwm - state.queueTotal;
  }

  function pullIfNeeded(state) {
    if (state.state !== 'readable' || state.closeRequested || !state.started) return;
    var waiting = state.reader !== null && state.reader.requests.length > 0;
    if (!waiting && desiredSize(state) <= 0) return;
    if (typeof state.source.pull !== 'function') return;
    if (state.pulling) { state.pullAgain = true; return; }
    state.pulling = true;
    var pulled;
    try {
      pulled = state.source.pull.call(state.source, state.controller);
    } catch (e) {
      pulled = Promise.reject(e);
    }
    Promise.resolve(pulled).then(function () {
      state.pulling = false;
      if (state.pullAgain) {
        state.pullAgain = false;
        pullIfNeeded(state);
      }
    }, function (e) { errorStream(state, e); });
  }

  function finishClose(state) {
    state.state = 'closed';
    var reader = state.reader;
    if (reader !== null) {
      var requests = reader.requests;
      reader.requests = [];
      for (var i = 0; i < requests.length; i++) requests[i].resolve({ value: undefined, done: true });
      reader.resolveClosed(undefined);
    }
  }

  function errorStream(state, error) {
    if (state.state !== 'readable') return;
    state.state = 'errored';
    state.error = error;
    state.queue = [];
    state.queueTotal = 0;
    var reader = state.reader;
    if (reader !== null) {
      var requests = reader.requests;
      reader.requests = [];
      for (var i = 0; i < requests.length; i++) requests[i].reject(error);
      reader.rejectClosed(error);
    }
  }

  function cancelStream(state, reason) {
    state.disturbed = true;
    if (state.state === 'closed') return Promise.resolve(undefined);
    if (state.state === 'errored') return Promise.reject(state.error);
    state.queue = [];
    state.queueTotal = 0;
    finishClose(state);
    var cancelled;
    try {
      cancelled = typeof state.source.cancel === 'function' ? state.source.cancel.call(state.source, reason) : undefined;
    } catch (e) {
      return Promise.reject(e);
    }
    return Promise.resolve(cancelled).then(noop);
  }

  function ReadableStreamDefaultController() {
    throw new TypeError("Failed to construct 'ReadableStreamDefaultController': Illegal constructor");
  }
  define(ReadableStreamDefaultController.prototype, {
    desiredSize: { get: function () { return desiredSize(controllerStates.get(this)); } },
    enqueue: function (chunk) {
      var state = controllerStates.get(this);
      if (state.closeRequested || state.state !== 'readable') {
        throw new TypeError("Failed to execute 'enqueue' on 'ReadableStreamDefaultController': the stream is closed or errored");
      }
      var reader = state.reader;
      if (reader !== null && reader.requests.length > 0) {
        reader.requests.shift().resolve({ value: chunk, done: false });
      } else {
        var size = 1;
        if (state.size !== null) {
          try {
            size = Number(state.size(chunk));
          } catch (e) {
            errorStream(state, e);
            throw e;
          }
        }
        state.queue.push({ chunk: chunk, size: size });
        state.queueTotal += size;
      }
      pullIfNeeded(state);
    },
    close: function () {
      var state = controllerStates.get(this);
      if (state.closeRequested || state.state !== 'readable') {
        throw new TypeError("Failed to execute 'close' on 'ReadableStreamDefaultController': the stream is closed or errored");
      }
      state.closeRequested = true;
      if (state.queue.length === 0) finishClose(state);
    },
    error: function (e) { errorStream(controllerStates.get(this), e); }
  });
  global.ReadableStreamDefaultController = ReadableStreamDefaultController;

  function ReadableStreamDefaultReader(stream) {
    var state = streamState(stream, 'getReader');
    if (state.reader !== null) {
      throw new TypeError("Failed to construct 'ReadableStreamDefaultReader': ReadableStream is locked");
    }
    var reader = { stream: stream, state: state, requests: [] };
    function settleable() {
      reader.closed = new Promise(function (resolve, reject) {
        reader.resolveClosed = resolve;
        reader.rejectClosed = reject;
      });
      reader.closed.catch(noop);
    }
    settleable();
    reader.renew = settleable;
    if (state.state === 'closed') reader.resolveClosed(undefined);
    if (state.state === 'errored') reader.rejectClosed(state.error);
    state.reader = reader;
    readerStates.set(this, reader);
  }
  function readerOf(value, method) {
    var reader = value !== null && typeof value === 'object' ? readerStates.get(value) : undefined;
    if (!reader) throw new TypeError("Failed to execute '" + method + "' on 'ReadableStreamDefaultReader': Illegal invocation");
    return reader;
  }
  define(ReadableStreamDefaultReader.prototype, {
    closed: { get: function () { return readerOf(this, 'closed').closed; } },
    read: function () {
      var reader;
      try { reader = readerOf(this, 'read'); } catch (e) { return Promise.reject(e); }
      if (reader.stream === null) return Promise.reject(new TypeError('This reader has been released'));
      var state = reader.state;
      state.disturbed = true;
      if (state.state === 'closed') return Promise.resolve({ value: undefined, done: true });
      if (state.state === 'errored') return Promise.reject(state.error);
      if (state.queue.length > 0) {
        var item = state.queue.shift();
        state.queueTotal -= item.size;
        if (state.closeRequested && state.queue.length === 0) finishClose(state);
        else pullIfNeeded(state);
        return Promise.resolve({ value: item.chunk, done: false });
      }
      return new Promise(function (resolve, reject) {
        reader.requests.push({ resolve: resolve, reject: reject });
        pullIfNeeded(state);
      });
    },
    cancel: function (reason) {
      var reader;
      try { reader = readerOf(this, 'cancel'); } catch (e) { return Promise.reject(e); }
      if (reader.stream === null) return Promise.reject(new TypeError('This reader has been released'));
      return cancelStream(reader.state, reason);
    },
    releaseLock: function () {
      var reader = readerOf(this, 'releaseLock');
      if (reader.stream === null) return;
      var released = new TypeError('This reader has been released');
      var requests = reader.requests;
      reader.requests = [];
      for (var i = 0; i < requests.length; i++) requests[i].reject(released);
      if (reader.state.state === 'readable') reader.rejectClosed(released);
      else { reader.renew(); reader.rejectClosed(released); }
      reader.state.reader = null;
      reader.stream = null;
    }
  });
  global.ReadableStreamDefaultReader = ReadableStreamDefaultReader;

  define(ReadableStream.prototype, {
    locked: { get: function () { return streamState(this, 'locked').reader !== null; } },
    getReader: function (options) {
      streamState(this, 'getReader');
      if (options && options.mode !== undefined) {
        throw new TypeError("Failed to execute 'getReader' on 'ReadableStream': BYOB readers are not supported in ScreenKit");
      }
      return new ReadableStreamDefaultReader(this);
    },
    cancel: function (reason) {
      var state;
      try { state = streamState(this, 'cancel'); } catch (e) { return Promise.reject(e); }
      if (state.reader !== null) return Promise.reject(new TypeError("Failed to execute 'cancel' on 'ReadableStream': the stream is locked"));
      return cancelStream(state, reason);
    },
    tee: function () {
      streamState(this, 'tee');
      var reader = this.getReader();
      var reading = false, readAgain = false, done = false;
      var canceled = [false, false], reasons = [undefined, undefined];
      var controllers = [null, null];
      var resolveCancel;
      var cancelled = new Promise(function (resolve) { resolveCancel = resolve; });
      function pull() {
        if (reading) { readAgain = true; return Promise.resolve(); }
        reading = true;
        reader.read().then(function (result) {
          reading = false;
          if (result.done) {
            done = true;
            for (var i = 0; i < 2; i++) if (!canceled[i]) { try { controllers[i].close(); } catch (e) { /* already closed */ } }
            resolveCancel(undefined);
            return;
          }
          // One chunk object for both branches, as the spec's default tee does.
          for (var j = 0; j < 2; j++) if (!canceled[j]) { try { controllers[j].enqueue(result.value); } catch (e) { /* branch closed */ } }
          if (readAgain) { readAgain = false; pull(); }
        }, function (e) {
          reading = false;
          for (var i = 0; i < 2; i++) { try { controllers[i].error(e); } catch (x) { /* ignore */ } }
          resolveCancel(undefined);
        });
        return Promise.resolve();
      }
      function branch(index) {
        return new ReadableStream({
          start: function (c) { controllers[index] = c; },
          pull: pull,
          cancel: function (reason) {
            canceled[index] = true;
            reasons[index] = reason;
            if (canceled[0] && canceled[1] && !done) {
              reader.cancel([reasons[0], reasons[1]]).then(resolveCancel, resolveCancel);
            }
            return cancelled;
          }
        }, { highWaterMark: 0 });
      }
      return [branch(0), branch(1)];
    },
    values: function (options) {
      var reader = this.getReader();
      var preventCancel = !!(options && options.preventCancel);
      var finished = false;
      var iterator = {
        next: function () {
          if (finished) return Promise.resolve({ value: undefined, done: true });
          return reader.read().then(function (result) {
            if (result.done) {
              finished = true;
              reader.releaseLock();
            }
            return result;
          }, function (e) {
            finished = true;
            reader.releaseLock();
            throw e;
          });
        },
        'return': function (value) {
          if (finished) return Promise.resolve({ value: value, done: true });
          finished = true;
          if (preventCancel) {
            reader.releaseLock();
            return Promise.resolve({ value: value, done: true });
          }
          var cancelled = reader.cancel(value);
          reader.releaseLock();
          return cancelled.then(function () { return { value: value, done: true }; });
        }
      };
      iterator[Symbol.asyncIterator] = function () { return this; };
      return iterator;
    }
  });
  ReadableStream.prototype[Symbol.asyncIterator] = ReadableStream.prototype.values;
  global.ReadableStream = ReadableStream;

  function streamFromBytes(bytes) {
    return new ReadableStream({
      start: function (c) {
        if (bytes.length > 0) c.enqueue(bytes);
        c.close();
      }
    });
  }

  Blob.prototype.stream = function () { return streamFromBytes(this._bytes); };

  // --- AbortController / AbortSignal ------------------------------------------

  var signalStates = new WeakMap();

  function AbortSignal() {
    throw new TypeError("Failed to construct 'AbortSignal': Illegal constructor");
  }
  inherit(AbortSignal, EventTarget);
  defineEventHandlers(AbortSignal.prototype, ['abort']);
  function makeSignal() {
    var signal = Object.create(AbortSignal.prototype);
    signalStates.set(signal, { aborted: false, reason: undefined, dependents: [] });
    return signal;
  }
  function signalOf(value, member) {
    var state = value !== null && typeof value === 'object' ? signalStates.get(value) : undefined;
    if (!state) throw new TypeError("Failed to read '" + member + "' from 'AbortSignal': Illegal invocation");
    return state;
  }
  function abortSignal(signal, reason) {
    var state = signalStates.get(signal);
    if (state.aborted) return;
    state.aborted = true;
    state.reason = reason === undefined ? domError('signal is aborted without reason', 'AbortError') : reason;
    var dependents = state.dependents;
    state.dependents = [];
    var event = new Event('abort');
    event.isTrusted = true;
    dispatchNow(signal, event);
    for (var i = 0; i < dependents.length; i++) abortSignal(dependents[i], state.reason);
  }
  define(AbortSignal.prototype, {
    aborted: { get: function () { return signalOf(this, 'aborted').aborted; } },
    reason: { get: function () { return signalOf(this, 'reason').reason; } },
    throwIfAborted: function () {
      var state = signalOf(this, 'throwIfAborted');
      if (state.aborted) throw state.reason;
    }
  });
  AbortSignal.abort = function (reason) {
    var signal = makeSignal();
    abortSignal(signal, reason);
    return signal;
  };
  AbortSignal.timeout = function (ms) {
    var signal = makeSignal();
    global.setTimeout(function () {
      abortSignal(signal, domError('signal timed out', 'TimeoutError'));
    }, Math.max(0, Number(ms) || 0));
    return signal;
  };
  AbortSignal.any = function (signals) {
    var result = makeSignal();
    var list = Array.prototype.slice.call(signals || []);
    for (var i = 0; i < list.length; i++) {
      var state = signalOf(list[i], 'any');
      if (state.aborted) {
        abortSignal(result, state.reason);
        return result;
      }
    }
    for (var j = 0; j < list.length; j++) signalStates.get(list[j]).dependents.push(result);
    return result;
  };
  global.AbortSignal = AbortSignal;

  function AbortController() {
    if (!(this instanceof AbortController)) {
      throw new TypeError("Failed to construct 'AbortController': Please use the 'new' operator.");
    }
    var signal = makeSignal();
    Object.defineProperty(this, 'signal', { value: signal, enumerable: true, configurable: true });
  }
  AbortController.prototype.abort = function (reason) { abortSignal(this.signal, reason); };
  global.AbortController = AbortController;

  // --- Headers ----------------------------------------------------------------

  var headerStates = new WeakMap();
  var HEADER_NAME = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
  var FORBIDDEN_REQUEST_HEADERS = table({
    'accept-charset': 1, 'accept-encoding': 1, 'access-control-request-headers': 1,
    'access-control-request-method': 1, connection: 1, 'content-length': 1, cookie: 1, cookie2: 1,
    date: 1, dnt: 1, expect: 1, host: 1, 'keep-alive': 1, origin: 1, referer: 1, 'set-cookie': 1,
    te: 1, trailer: 1, 'transfer-encoding': 1, upgrade: 1, via: 1
  });
  function forbiddenRequestHeader(lower) {
    return FORBIDDEN_REQUEST_HEADERS[lower] === 1 || lower.indexOf('proxy-') === 0 || lower.indexOf('sec-') === 0;
  }
  function forbiddenResponseHeader(lower) { return lower === 'set-cookie' || lower === 'set-cookie2'; }
  function headerValue(value) {
    var v = String(value).replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, '');
    if (/[\0\r\n]/.test(v)) throw new TypeError('Invalid header value: ' + JSON.stringify(v));
    return v;
  }
  function headerName(name) {
    var n = String(name);
    if (!HEADER_NAME.test(n)) throw new TypeError('Invalid header name: ' + JSON.stringify(n));
    return n.toLowerCase();
  }
  function headersOf(value, method) {
    var state = value !== null && typeof value === 'object' ? headerStates.get(value) : undefined;
    if (!state) throw new TypeError("Failed to execute '" + method + "' on 'Headers': Illegal invocation");
    return state;
  }

  function Headers(init) {
    if (!(this instanceof Headers)) throw new TypeError("Failed to construct 'Headers': Please use the 'new' operator.");
    headerStates.set(this, { list: [], guard: 'none' });
    fillHeaders(this, init);
  }
  function fillHeaders(headers, init) {
    if (init === undefined || init === null) return;
    if (typeof init !== 'object') throw new TypeError("Failed to construct 'Headers': The provided value is not of type '(record<ByteString, ByteString> or sequence<sequence<ByteString>>)'.");
    if (headerStates.has(init)) {
      var list = headerStates.get(init).list.slice();
      for (var h = 0; h < list.length; h++) headers.append(list[h][0], list[h][1]);
      return;
    }
    if (typeof init[Symbol.iterator] === 'function') {
      var pairs = Array.from(init);
      for (var i = 0; i < pairs.length; i++) {
        var pair = Array.from(pairs[i]);
        if (pair.length !== 2) throw new TypeError("Failed to construct 'Headers': Invalid value: each header needs a name and a value");
        headers.append(pair[0], pair[1]);
      }
      return;
    }
    Object.keys(init).forEach(function (key) { headers.append(key, init[key]); });
  }
  function mutableHeaders(state, name) {
    if (state.guard === 'immutable') throw new TypeError("Failed to execute '" + name + "' on 'Headers': Headers are immutable");
  }
  function sortedHeaders(state) {
    var names = [], combined = Object.create(null), out = [];
    for (var i = 0; i < state.list.length; i++) {
      var name = state.list[i][0];
      if (name === 'set-cookie') continue;
      if (!(name in combined)) { names.push(name); combined[name] = state.list[i][1]; }
      else combined[name] += ', ' + state.list[i][1];
    }
    var cookies = state.list.filter(function (p) { return p[0] === 'set-cookie'; });
    if (cookies.length) names.push('set-cookie');
    names.sort();
    for (var j = 0; j < names.length; j++) {
      if (names[j] === 'set-cookie') cookies.forEach(function (p) { out.push(['set-cookie', p[1]]); });
      else out.push([names[j], combined[names[j]]]);
    }
    return out;
  }
  define(Headers.prototype, {
    append: function (name, value) {
      var state = headersOf(this, 'append');
      var lower = headerName(name);
      var v = headerValue(value);
      mutableHeaders(state, 'append');
      if (state.guard === 'request' && forbiddenRequestHeader(lower)) return;
      if (state.guard === 'response' && forbiddenResponseHeader(lower)) return;
      state.list.push([lower, v]);
    },
    'delete': function (name) {
      var state = headersOf(this, 'delete');
      var lower = headerName(name);
      mutableHeaders(state, 'delete');
      if (state.guard === 'request' && forbiddenRequestHeader(lower)) return;
      state.list = state.list.filter(function (p) { return p[0] !== lower; });
    },
    get: function (name) {
      var state = headersOf(this, 'get');
      var lower = headerName(name);
      var values = [];
      for (var i = 0; i < state.list.length; i++) if (state.list[i][0] === lower) values.push(state.list[i][1]);
      return values.length ? values.join(', ') : null;
    },
    getSetCookie: function () {
      return headersOf(this, 'getSetCookie').list.filter(function (p) { return p[0] === 'set-cookie'; })
        .map(function (p) { return p[1]; });
    },
    has: function (name) {
      var state = headersOf(this, 'has');
      var lower = headerName(name);
      return state.list.some(function (p) { return p[0] === lower; });
    },
    set: function (name, value) {
      var state = headersOf(this, 'set');
      var lower = headerName(name);
      var v = headerValue(value);
      mutableHeaders(state, 'set');
      if (state.guard === 'request' && forbiddenRequestHeader(lower)) return;
      if (state.guard === 'response' && forbiddenResponseHeader(lower)) return;
      var found = false;
      state.list = state.list.filter(function (p) {
        if (p[0] !== lower) return true;
        if (found) return false;
        found = true;
        p[1] = v;
        return true;
      });
      if (!found) state.list.push([lower, v]);
    },
    forEach: function (callback, thisArg) {
      var sorted = sortedHeaders(headersOf(this, 'forEach'));
      for (var i = 0; i < sorted.length; i++) callback.call(thisArg, sorted[i][1], sorted[i][0], this);
    },
    entries: function () { return pairIterator(sortedHeaders(headersOf(this, 'entries')), function (p) { return [p[0], p[1]]; }); },
    keys: function () { return pairIterator(sortedHeaders(headersOf(this, 'keys')), function (p) { return p[0]; }); },
    values: function () { return pairIterator(sortedHeaders(headersOf(this, 'values')), function (p) { return p[1]; }); }
  });
  Headers.prototype[Symbol.iterator] = Headers.prototype.entries;
  global.Headers = Headers;

  function guardedHeaders(guard, init) {
    var headers = new Headers();
    headerStates.get(headers).guard = guard;
    fillHeaders(headers, init);
    return headers;
  }
  function headerPairs(headers) { return headerStates.get(headers).list.map(function (p) { return [p[0], p[1]]; }); }

  // --- FormData and File ---------------------------------------------------------

  function File(bits, name, options) {
    if (arguments.length < 2) {
      throw new TypeError("Failed to construct 'File': 2 arguments required, but only " + arguments.length + ' present.');
    }
    Blob.call(this, bits, options);
    this.name = String(name);
    this.lastModified = options && options.lastModified !== undefined ? Number(options.lastModified) : Date.now();
  }
  File.prototype = Object.create(Blob.prototype);
  File.prototype.constructor = File;
  global.File = File;

  function formEntry(name, value, filename, argc) {
    if (value instanceof Blob) {
      if (!(value instanceof File) || filename !== undefined) {
        var file = new File([], filename !== undefined ? String(filename) : (value instanceof File ? value.name : 'blob'),
                            { type: value.type, lastModified: value.lastModified });
        file._bytes = value._bytes;
        file.size = value.size;
        value = file;
      }
      return [String(name), value];
    }
    if (argc > 2) throw new TypeError("Failed to execute 'append' on 'FormData': parameter 2 is not of type 'Blob'.");
    return [String(name), String(value)];
  }

  function FormData(form) {
    if (!(this instanceof FormData)) throw new TypeError("Failed to construct 'FormData': Please use the 'new' operator.");
    if (form !== undefined) {
      throw new TypeError("Failed to construct 'FormData': a form element is not supported -- there is no HTML form here");
    }
    this._entries = [];
  }
  define(FormData.prototype, {
    append: function (name, value, filename) { this._entries.push(formEntry(name, value, filename, arguments.length)); },
    set: function (name, value, filename) {
      var entry = formEntry(name, value, filename, arguments.length), found = false;
      this._entries = this._entries.filter(function (e) {
        if (e[0] !== entry[0]) return true;
        if (found) return false;
        found = true;
        e[1] = entry[1];
        return true;
      });
      if (!found) this._entries.push(entry);
    },
    get: function (name) {
      var key = String(name);
      for (var i = 0; i < this._entries.length; i++) if (this._entries[i][0] === key) return this._entries[i][1];
      return null;
    },
    getAll: function (name) {
      var key = String(name);
      return this._entries.filter(function (e) { return e[0] === key; }).map(function (e) { return e[1]; });
    },
    has: function (name) {
      var key = String(name);
      return this._entries.some(function (e) { return e[0] === key; });
    },
    'delete': function (name) {
      var key = String(name);
      this._entries = this._entries.filter(function (e) { return e[0] !== key; });
    },
    forEach: function (callback, thisArg) {
      var entries = this._entries.slice();
      for (var i = 0; i < entries.length; i++) callback.call(thisArg, entries[i][1], entries[i][0], this);
    },
    entries: function () { return pairIterator(this._entries, function (e) { return [e[0], e[1]]; }); },
    keys: function () { return pairIterator(this._entries, function (e) { return e[0]; }); },
    values: function () { return pairIterator(this._entries, function (e) { return e[1]; }); }
  });
  FormData.prototype[Symbol.iterator] = FormData.prototype.entries;
  global.FormData = FormData;

  function escapeFormName(value) {
    return String(value).replace(/\r\n|\r|\n/g, '\r\n').replace(/\n/g, '%0A').replace(/\r/g, '%0D').replace(/"/g, '%22');
  }
  function multipartBody(form) {
    var boundary = '----ScreenKitFormBoundary';
    for (var r = 0; r < 16; r++) boundary += 'abcdefghijklmnopqrstuvwxyz0123456789'.charAt(Math.floor(Math.random() * 36));
    var chunks = [], total = 0;
    function push(bytes) { chunks.push(bytes); total += bytes.length; }
    form._entries.forEach(function (entry) {
      var head = '--' + boundary + '\r\nContent-Disposition: form-data; name="' + escapeFormName(entry[0]) + '"';
      if (entry[1] instanceof Blob) {
        head += '; filename="' + escapeFormName(entry[1].name) + '"\r\nContent-Type: ' +
                (entry[1].type || 'application/octet-stream') + '\r\n\r\n';
        push(utf8Bytes(head));
        push(entry[1]._bytes);
        push(utf8Bytes('\r\n'));
      } else {
        push(utf8Bytes(head + '\r\n\r\n' + entry[1].replace(/\r\n|\r|\n/g, '\r\n') + '\r\n'));
      }
    });
    push(utf8Bytes('--' + boundary + '--\r\n'));
    return { bytes: concatBytes(chunks, total), type: 'multipart/form-data; boundary=' + boundary };
  }

  function indexOfBytes(haystack, needle, from) {
    outer: for (var i = from; i <= haystack.length - needle.length; i++) {
      for (var j = 0; j < needle.length; j++) if (haystack[i + j] !== needle[j]) continue outer;
      return i;
    }
    return -1;
  }
  function parseFormData(bytes, contentType) {
    var form = new FormData();
    var type = String(contentType || '').toLowerCase();
    if (type.indexOf('application/x-www-form-urlencoded') === 0) {
      new URLSearchParams(bytesToText(bytes)).forEach(function (value, name) { form.append(name, value); });
      return form;
    }
    var match = /boundary=(?:"([^"]+)"|([^;]+))/i.exec(String(contentType || ''));
    if (type.indexOf('multipart/form-data') !== 0 || !match) {
      throw new TypeError('Could not parse content as FormData.');
    }
    var delimiter = utf8Bytes('--' + (match[1] || match[2]).trim());
    var position = indexOfBytes(bytes, delimiter, 0);
    if (position < 0) throw new TypeError('Could not parse content as FormData.');
    for (;;) {
      position += delimiter.length;
      if (bytes[position] === 45 && bytes[position + 1] === 45) break;  // "--": the last one
      position += 2;  // CRLF
      var headEnd = indexOfBytes(bytes, utf8Bytes('\r\n\r\n'), position);
      if (headEnd < 0) throw new TypeError('Could not parse content as FormData.');
      var head = bytesToText(bytes.subarray(position, headEnd));
      var next = indexOfBytes(bytes, delimiter, headEnd + 4);
      if (next < 0) throw new TypeError('Could not parse content as FormData.');
      var body = bytes.slice(headEnd + 4, next - 2);
      var name = /(?:^|;)\s*name="([^"]*)"/im.exec(head);
      var filename = /filename="([^"]*)"/i.exec(head);
      var partType = /content-type:\s*([^\r\n]*)/i.exec(head);
      if (!name) throw new TypeError('Could not parse content as FormData.');
      if (filename) {
        var file = new File([], filename[1], { type: partType ? partType[1] : '' });
        file._bytes = body;
        file.size = body.length;
        form.append(name[1], file);
      } else {
        form.append(name[1], bytesToText(body));
      }
      position = next;
    }
    return form;
  }

  // --- Body: what Request and Response share --------------------------------------

  var bodyStates = new WeakMap();

  // A body from anything fetch accepts: its bytes (or its stream) and the
  // Content-Type it implies.
  function extractBody(value) {
    if (value instanceof ReadableStream) {
      var state = streamStates.get(value);
      if (state.reader !== null || state.disturbed) throw new TypeError('the body stream is locked or already read');
      return { stream: value, bytes: null, type: null };
    }
    if (typeof value === 'string') return { stream: null, bytes: utf8Bytes(value), type: 'text/plain;charset=UTF-8' };
    if (value instanceof URLSearchParams) {
      return { stream: null, bytes: utf8Bytes(value.toString()), type: 'application/x-www-form-urlencoded;charset=UTF-8' };
    }
    if (value instanceof FormData) {
      var multipart = multipartBody(value);
      return { stream: null, bytes: multipart.bytes, type: multipart.type };
    }
    if (value instanceof Blob) return { stream: null, bytes: value._bytes.slice(), type: value.type || null };
    if (isBufferSource(value)) return { stream: null, bytes: copyBytes(value), type: null };
    return { stream: null, bytes: utf8Bytes(String(value)), type: 'text/plain;charset=UTF-8' };
  }

  function setBody(owner, extracted) {
    bodyStates.set(owner, { stream: extracted ? extracted.stream : null, bytes: extracted ? extracted.bytes : null,
                            used: false, present: !!extracted });
  }
  function bodyUsed(owner) {
    var body = bodyStates.get(owner);
    return body.used || (body.stream !== null && streamStates.get(body.stream).disturbed);
  }
  function bodyStream(owner) {
    var body = bodyStates.get(owner);
    if (!body.present) return null;
    if (body.stream === null) {
      body.stream = streamFromBytes(body.bytes);
      body.bytes = null;
    }
    return body.stream;
  }
  // Read the whole body, once: bytes as a Uint8Array.
  function consumeBody(owner, method) {
    var body = bodyStates.get(owner);
    if (!body) return Promise.reject(new TypeError("Failed to execute '" + method + "': Illegal invocation"));
    if (bodyUsed(owner)) return Promise.reject(new TypeError("Failed to execute '" + method + "': body stream already read"));
    if (body.stream !== null && streamStates.get(body.stream).reader !== null) {
      return Promise.reject(new TypeError("Failed to execute '" + method + "': body stream is locked"));
    }
    body.used = true;
    // Always a fresh copy: the bytes may be shared with a clone, a registered
    // Blob or the other branch of a tee, and a reader may mutate what it gets.
    if (body.stream === null) return Promise.resolve(body.bytes ? body.bytes.slice() : new Uint8Array(0));
    var reader = body.stream.getReader(), chunks = [], total = 0;
    return new Promise(function (resolve, reject) {
      function next() {
        reader.read().then(function (result) {
          if (result.done) return resolve(chunks.length === 1 ? chunks[0].slice() : concatBytes(chunks, total));
          var chunk = result.value;
          if (!(chunk instanceof Uint8Array)) return reject(new TypeError('body stream chunks must be Uint8Array'));
          chunks.push(chunk);
          total += chunk.length;
          next();
        }, reject);
      }
      next();
    });
  }
  function bodyMixin(proto, contentType) {
    define(proto, {
      body: { get: function () { return bodyStream(this); } },
      bodyUsed: { get: function () { return bodyUsed(this); } },
      arrayBuffer: function () {
        return consumeBody(this, 'arrayBuffer').then(function (bytes) { return bytes.buffer; });
      },
      bytes: function () { return consumeBody(this, 'bytes'); },
      blob: function () {
        var type = contentType(this);
        return consumeBody(this, 'blob').then(function (bytes) { return blobFromBytes(bytes, type); });
      },
      text: function () { return consumeBody(this, 'text').then(bytesToText); },
      json: function () { return consumeBody(this, 'json').then(function (bytes) { return JSON.parse(bytesToText(bytes)); }); },
      formData: function () {
        var type = contentType(this);
        return consumeBody(this, 'formData').then(function (bytes) { return parseFormData(bytes, type); });
      }
    });
  }
  function headerContentType(owner) {
    var type = owner.headers.get('content-type');
    return type === null ? '' : type;
  }

  // --- Request ------------------------------------------------------------------

  var requestStates = new WeakMap();
  var REDIRECT_MODES = table({ follow: 1, manual: 1, error: 1 });
  var CREDENTIALS_MODES = table({ omit: 1, 'same-origin': 1, include: 1 });

  // A request URL: absolute, resolved against the document's address. A package
  // asset keeps its screenkit: address and is still read from the package.
  function requestUrl(input, iface) {
    var href = String(input);
    try {
      return SCHEME.test(href) ? parseUrl(href).href : new URLShim(href, currentHref()).href;
    } catch (e) {
      throw new TypeError("Failed to construct '" + iface + "': Invalid URL " + JSON.stringify(href));
    }
  }
  function normalizeMethod(method) {
    var m = String(method);
    if (!HEADER_NAME.test(m)) throw new TypeError("'" + m + "' is not a valid HTTP method.");
    var upper = m.toUpperCase();
    if (upper === 'CONNECT' || upper === 'TRACE' || upper === 'TRACK') {
      throw new TypeError("'" + m + "' HTTP method is unsupported.");
    }
    return /^(DELETE|GET|HEAD|OPTIONS|POST|PUT|PATCH)$/.test(upper) ? upper : m;
  }

  function Request(input, init) {
    if (!(this instanceof Request)) throw new TypeError("Failed to construct 'Request': Please use the 'new' operator.");
    init = init === undefined || init === null ? {} : init;
    var base = input instanceof Request ? requestStates.get(input) : null;
    var state = {
      url: base ? base.url : requestUrl(input, 'Request'),
      method: base ? base.method : 'GET',
      redirect: base ? base.redirect : 'follow',
      credentials: base ? base.credentials : 'same-origin',
      mode: base ? base.mode : 'cors',
      cache: base ? base.cache : 'default',
      referrer: base ? base.referrer : 'about:client',
      referrerPolicy: base ? base.referrerPolicy : '',
      integrity: base ? base.integrity : '',
      keepalive: base ? base.keepalive : false,
      duplex: base ? base.duplex : 'half',
      signal: makeSignal()
    };
    if (init.method !== undefined) state.method = normalizeMethod(init.method);
    if (init.redirect !== undefined) {
      if (REDIRECT_MODES[init.redirect] !== 1) throw new TypeError("Failed to construct 'Request': invalid redirect mode " + JSON.stringify(String(init.redirect)));
      state.redirect = String(init.redirect);
    }
    if (init.credentials !== undefined) {
      if (CREDENTIALS_MODES[init.credentials] !== 1) throw new TypeError("Failed to construct 'Request': invalid credentials mode " + JSON.stringify(String(init.credentials)));
      state.credentials = String(init.credentials);
    }
    ['mode', 'cache', 'referrer', 'referrerPolicy', 'integrity'].forEach(function (key) {
      if (init[key] !== undefined) state[key] = String(init[key]);
    });
    if (init.keepalive !== undefined) state.keepalive = !!init.keepalive;
    var parentSignal = init.signal !== undefined ? init.signal : (base ? base.signal : null);
    if (parentSignal !== null && parentSignal !== undefined) {
      var parent = signalOf(parentSignal, 'signal');
      if (parent.aborted) abortSignal(state.signal, parent.reason);
      else parent.dependents.push(state.signal);
      state.parentSignal = parentSignal;
    }
    state.headers = guardedHeaders('request', init.headers !== undefined ? init.headers : (base ? input.headers : undefined));

    var extracted = null;
    if (init.body !== undefined && init.body !== null) {
      if (state.method === 'GET' || state.method === 'HEAD') {
        throw new TypeError("Failed to construct 'Request': Request with GET/HEAD method cannot have body.");
      }
      extracted = extractBody(init.body);
      if (extracted.stream !== null && init.duplex !== 'half') {
        throw new TypeError("Failed to construct 'Request': The duplex member must be specified for a request with a streaming body");
      }
      if (extracted.type !== null && !state.headers.has('content-type')) state.headers.append('content-type', extracted.type);
    } else if (base && bodyStates.get(input).present) {
      if (bodyUsed(input)) throw new TypeError("Failed to construct 'Request': Cannot construct a Request with a Request object that has already been used.");
      var inherited = bodyStates.get(input);
      extracted = { stream: inherited.stream, bytes: inherited.bytes, type: null };
      inherited.used = true;
    }
    if (init.duplex !== undefined) state.duplex = String(init.duplex);
    requestStates.set(this, state);
    setBody(this, extracted);
  }
  function requestOf(value, member) {
    var state = value !== null && typeof value === 'object' ? requestStates.get(value) : undefined;
    if (!state) throw new TypeError("Failed to read '" + member + "' from 'Request': Illegal invocation");
    return state;
  }
  ['url', 'method', 'headers', 'redirect', 'credentials', 'mode', 'cache', 'referrer', 'referrerPolicy',
   'integrity', 'keepalive', 'signal', 'duplex'].forEach(function (key) {
    Object.defineProperty(Request.prototype, key, {
      get: function () { return requestOf(this, key)[key]; }, enumerable: true, configurable: true
    });
  });
  Object.defineProperty(Request.prototype, 'destination', { get: function () { return ''; }, enumerable: true, configurable: true });
  Request.prototype.clone = function () {
    var state = requestOf(this, 'clone');
    if (bodyUsed(this)) throw new TypeError("Failed to execute 'clone' on 'Request': Request body is already used");
    var copy = Object.create(Request.prototype);
    var copied = {};
    for (var key in state) if (Object.prototype.hasOwnProperty.call(state, key)) copied[key] = state[key];
    copied.headers = guardedHeaders('request', state.headers);
    copied.signal = makeSignal();
    copied.parentSignal = null;
    if (state.signal.aborted) {
      abortSignal(copied.signal, state.signal.reason);
    } else {
      signalStates.get(state.signal).dependents.push(copied.signal);
      copied.parentSignal = state.signal;
    }
    requestStates.set(copy, copied);
    setBody(copy, teeBody(this));
    return copy;
  };
  bodyMixin(Request.prototype, headerContentType);
  global.Request = Request;

  // Split a body in two: this owner keeps one branch, the returned one is the
  // other's. Bytes are shared, a stream is teed.
  function teeBody(owner) {
    var body = bodyStates.get(owner);
    if (!body.present) return null;
    if (body.stream === null) return { stream: null, bytes: body.bytes, type: null };
    var branches = body.stream.tee();
    body.stream = branches[0];
    return { stream: branches[1], bytes: null, type: null };
  }

  // --- Response -----------------------------------------------------------------

  var responseStates = new WeakMap();
  var NULL_BODY_STATUS = table({ 101: 1, 103: 1, 204: 1, 205: 1, 304: 1 });
  var REDIRECT_STATUS = table({ 301: 1, 302: 1, 303: 1, 307: 1, 308: 1 });
  // How far a response body is read ahead of the page. A body nobody reads
  // stops being downloaded once this much of it is queued, and resumes as the
  // page reads -- rather than the whole response landing in memory.
  var FETCH_FLOW_WINDOW = 1024 * 1024;

  function Response(body, init) {
    if (!(this instanceof Response)) throw new TypeError("Failed to construct 'Response': Please use the 'new' operator.");
    init = init === undefined || init === null ? {} : init;
    var status = init.status === undefined ? 200 : Number(init.status);
    if (!(status >= 200 && status <= 599) || Math.floor(status) !== status) {
      throw new RangeError("Failed to construct 'Response': The status provided (" + init.status + ') is outside the range [200, 599].');
    }
    var statusText = init.statusText === undefined ? '' : String(init.statusText);
    if (/[\r\n]/.test(statusText)) throw new TypeError("Failed to construct 'Response': Invalid statusText");
    var state = { type: 'default', url: '', redirected: false, status: status, statusText: statusText,
                  headers: guardedHeaders('response', init.headers) };
    var extracted = null;
    if (body !== undefined && body !== null) {
      if (NULL_BODY_STATUS[status] === 1) throw new TypeError("Failed to construct 'Response': Response with null body status cannot have body");
      extracted = extractBody(body);
      if (extracted.type !== null && !state.headers.has('content-type')) state.headers.append('content-type', extracted.type);
    }
    responseStates.set(this, state);
    setBody(this, extracted);
  }
  function responseOf(value, member) {
    var state = value !== null && typeof value === 'object' ? responseStates.get(value) : undefined;
    if (!state) throw new TypeError("Failed to read '" + member + "' from 'Response': Illegal invocation");
    return state;
  }
  ['type', 'url', 'redirected', 'status', 'statusText', 'headers'].forEach(function (key) {
    Object.defineProperty(Response.prototype, key, {
      get: function () { return responseOf(this, key)[key]; }, enumerable: true, configurable: true
    });
  });
  Object.defineProperty(Response.prototype, 'ok', {
    get: function () { var s = responseOf(this, 'ok').status; return s >= 200 && s < 300; },
    enumerable: true, configurable: true
  });
  Response.prototype.clone = function () {
    var state = responseOf(this, 'clone');
    if (bodyUsed(this)) throw new TypeError("Failed to execute 'clone' on 'Response': Response body is already used");
    var copy = makeResponse({ type: state.type, url: state.url, redirected: state.redirected, status: state.status,
                              statusText: state.statusText, headers: headerPairs(state.headers), guard: headerStates.get(state.headers).guard },
                            teeBody(this));
    return copy;
  };
  bodyMixin(Response.prototype, headerContentType);
  Response.error = function () {
    return makeResponse({ type: 'error', url: '', redirected: false, status: 0, statusText: '', headers: [], guard: 'immutable' }, null);
  };
  Response.redirect = function (url, status) {
    var code = status === undefined ? 302 : Number(status);
    if (!(code === 301 || code === 302 || code === 303 || code === 307 || code === 308)) {
      throw new RangeError("Failed to execute 'redirect' on 'Response': Invalid status code");
    }
    return makeResponse({ type: 'default', url: '', redirected: false, status: code, statusText: '',
                          headers: [['location', requestUrl(url, 'Response')]], guard: 'immutable' }, null);
  };
  Response.json = function (data, init) {
    var text = JSON.stringify(data);
    if (text === undefined) throw new TypeError("Failed to execute 'json' on 'Response': The data is not JSON serializable");
    var response = new Response(text, init);
    responseStates.get(response).headers.set('content-type', 'application/json');
    return response;
  };
  global.Response = Response;

  // A Response built here rather than by the constructor: from the network, from
  // a package asset, or a clone.
  function makeResponse(fields, body) {
    var response = Object.create(Response.prototype);
    var headers = new Headers();
    var headerState = headerStates.get(headers);
    headerState.list = fields.headers.map(function (p) { return [String(p[0]).toLowerCase(), String(p[1])]; });
    headerState.guard = fields.guard || 'immutable';
    responseStates.set(response, { type: fields.type, url: fields.url, redirected: fields.redirected,
                                   status: fields.status, statusText: fields.statusText, headers: headers });
    setBody(response, body);
    return response;
  }

  // --- fetch --------------------------------------------------------------------

  global.fetch = function (input, init) {
    var request;
    try {
      request = new Request(input, init);
    } catch (e) {
      return Promise.reject(e);
    }
    var state = requestStates.get(request);
    var body = bodyStates.get(request);
    body.used = body.present;
    return new Promise(function (resolve, reject) {
      if (state.integrity !== '') {
        detachSignal(state);
        if (body.stream !== null) body.stream.cancel().catch(noop);
        reject(new TypeError('Failed to fetch ' + state.url + ': subresource integrity (the integrity option) is not ' +
                             'supported in ScreenKit, and a check that is not made must not look as if it passed'));
        return;
      }
      if (state.signal.aborted) {
        detachSignal(state);
        if (body.stream !== null) body.stream.cancel(state.signal.reason).catch(noop);
        reject(state.signal.reason);
        return;
      }
      if (isNetworkUrl(state.url)) networkFetch(state, body, resolve, reject);
      else assetFetch(state, resolve, reject);
    });
  };

  // A request's signal follows its parent's (init.signal, or the Request it was
  // made from) only while the fetch is outstanding. Left registered, one
  // controller reused across many fetches would collect every one of them.
  function detachSignal(state) {
    if (!state.parentSignal) return;
    var dependents = signalStates.get(state.parentSignal).dependents;
    var index = dependents.indexOf(state.signal);
    if (index >= 0) dependents.splice(index, 1);
    state.parentSignal = null;
  }

  function assetFetch(state, resolve, reject) {
    var aborted = false;
    function onAbort() {
      aborted = true;
      detachSignal(state);
      reject(state.signal.reason);
    }
    state.signal.addEventListener('abort', onAbort);
    later(function () {
      state.signal.removeEventListener('abort', onAbort);
      if (aborted) return;
      detachSignal(state);
      try {
        var res = resolveResource(state.url);
        var blob = res.blob || blobFromAsset(res.path);
        resolve(makeResponse({ type: 'basic', url: state.url, redirected: false, status: 200, statusText: 'OK',
                               headers: blob.type ? [['content-type', blob.type]] : [] },
                             { stream: null, bytes: blob._bytes, type: null }));
      } catch (err) {
        // A missing asset is a response, like a 404 over HTTP; anything else
        // (a bad URL, an escape the reader refused) is a rejection, as fetch does.
        if (/no such asset/.test(String(err && err.message))) {
          resolve(makeResponse({ type: 'basic', url: state.url, redirected: false, status: 404, statusText: 'Not Found',
                                 headers: [] }, { stream: null, bytes: new Uint8Array(0), type: null }));
        } else {
          reject(new TypeError(String(err && err.message)));
        }
      }
    });
  }

  function failedFetch(url, payload) {
    var error = networkError('Failed to fetch ' + url + ': ' + payload.message);
    error.cause = payload.kind;
    return error;
  }

  function networkFetch(state, body, resolve, reject) {
    if (net === null) {
      reject(noNetwork());
      return;
    }
    var id = 0, settled = false, finished = false, controller = null, uploadReader = null;
    var delivered = 0, acknowledged = 0;
    function cleanup() {
      finished = true;
      state.signal.removeEventListener('abort', onAbort);
      detachSignal(state);
    }
    // The request body's source hears the cancel through the reader the upload
    // pump holds: the stream itself is locked to it and would refuse.
    function cancelUpload(reason) {
      if (uploadReader !== null) uploadReader.cancel(reason).catch(noop);
      else if (body.stream !== null) body.stream.cancel(reason).catch(noop);
    }
    function onAbort() {
      if (finished) return;
      cleanup();
      net.abort(id);
      var reason = state.signal.reason;
      if (!settled) {
        settled = true;
        reject(reason);
      } else if (controller !== null) {
        try { controller.error(reason); } catch (e) { /* already closed */ }
      }
      cancelUpload(reason);
    }
    function onEvent(type, payload) {
      if (finished) return;
      if (type === 'head') {
        // `redirect: 'manual'` gets what a browser gives a page: an opaque
        // redirect -- status 0, no headers, no body, the URL that answered --
        // never the 3xx itself or its Location. The rest of that response is
        // not wanted, so the request ends here.
        if (state.redirect === 'manual' && REDIRECT_STATUS[payload.status] === 1) {
          cleanup();
          net.abort(id);
          settled = true;
          resolve(makeResponse({ type: 'opaqueredirect', url: payload.url, redirected: false, status: 0,
                                 statusText: '', headers: [] }, null));
          cancelUpload(new TypeError('the request was answered with a redirect'));
          return;
        }
        // HEAD and the null-body statuses have no body at all: `body` is null.
        var nullBody = state.method === 'HEAD' || NULL_BODY_STATUS[payload.status] === 1;
        var response = makeResponse({
          type: 'basic', url: payload.url, redirected: payload.redirected, status: payload.status,
          statusText: payload.statusText,
          headers: payload.headers.filter(function (p) { return !forbiddenResponseHeader(p[0]); })
        }, nullBody ? null : {
          stream: new ReadableStream({
            start: function (c) { controller = c; },
            // The queue is below its high-water mark: what was delivered and is
            // no longer queued has been read, so native may read that much more.
            pull: function (c) {
              if (finished) return;
              var read = delivered - (FETCH_FLOW_WINDOW - c.desiredSize);
              if (read > acknowledged) {
                net.acknowledge(id, read - acknowledged);
                acknowledged = read;
              }
            },
            cancel: function () {
              if (finished) return;
              cleanup();
              net.abort(id);
            }
          }, { highWaterMark: FETCH_FLOW_WINDOW, size: function (chunk) { return chunk.byteLength; } }),
          bytes: null, type: null
        });
        settled = true;
        resolve(response);
      } else if (type === 'data') {
        delivered += payload.byteLength;
        if (controller !== null) {
          try { controller.enqueue(new Uint8Array(payload)); } catch (e) { /* cancelled */ }
        }
      } else if (type === 'end') {
        cleanup();
        if (controller !== null) {
          try { controller.close(); } catch (e) { /* cancelled */ }
        }
      } else if (type === 'error') {
        cleanup();
        var error = failedFetch(state.url, payload);
        if (!settled) {
          settled = true;
          reject(error);
        } else if (controller !== null) {
          try { controller.error(error); } catch (e) { /* cancelled */ }
        }
        cancelUpload(error);
      }
    }
    var options = {
      method: state.method, url: state.url, headers: headerPairs(state.headers), redirect: state.redirect,
      cookies: state.credentials !== 'omit', flowWindow: FETCH_FLOW_WINDOW
    };
    if (body.stream !== null) options.stream = true;
    else if (body.bytes !== null) options.body = body.bytes;
    try {
      id = net.request(options, onEvent);
    } catch (e) {
      detachSignal(state);
      reject(networkError('Failed to fetch ' + state.url + ': ' + (e && e.message)));
      return;
    }
    state.signal.addEventListener('abort', onAbort);
    if (body.stream !== null) {
      var reader = body.stream.getReader();
      uploadReader = reader;
      var pump = function () {
        reader.read().then(function (result) {
          if (finished) return;
          if (result.done) {
            net.finish(id);
            return;
          }
          if (!(result.value instanceof Uint8Array)) {
            streamFailed(new TypeError('a request body stream must yield Uint8Array chunks'));
            return;
          }
          if (result.value.byteLength > 0) net.write(id, result.value);
          pump();
        }, streamFailed);
      };
      var streamFailed = function (error) {
        if (finished) return;
        cleanup();
        net.abort(id);
        reader.cancel(error).catch(noop);
        var failure = networkError('Failed to fetch ' + state.url + ': the request body stream errored');
        failure.cause = error;
        if (!settled) {
          settled = true;
          reject(failure);
        } else if (controller !== null) {
          try { controller.error(failure); } catch (e) { /* closed */ }
        }
      };
      pump();
    }
  }

  // --- XMLHttpRequest --------------------------------------------------------------

  var UNSENT = 0, OPENED = 1, HEADERS_RECEIVED = 2, LOADING = 3, DONE = 4;
  var xhrStates = new WeakMap();

  function XMLHttpRequestEventTarget() {
    throw new TypeError("Failed to construct 'XMLHttpRequestEventTarget': Illegal constructor");
  }
  inherit(XMLHttpRequestEventTarget, EventTarget);
  global.XMLHttpRequestEventTarget = XMLHttpRequestEventTarget;

  function XMLHttpRequestUpload() {
    throw new TypeError("Failed to construct 'XMLHttpRequestUpload': Illegal constructor");
  }
  inherit(XMLHttpRequestUpload, XMLHttpRequestEventTarget);
  global.XMLHttpRequestUpload = XMLHttpRequestUpload;

  var XHR_EVENTS = ['loadstart', 'progress', 'abort', 'error', 'load', 'timeout', 'loadend'];
  defineEventHandlers(XMLHttpRequestEventTarget.prototype, XHR_EVENTS);

  function XMLHttpRequest() {
    if (!(this instanceof XMLHttpRequest)) {
      throw new TypeError("Failed to construct 'XMLHttpRequest': Please use the 'new' operator.");
    }
    var upload = Object.create(XMLHttpRequestUpload.prototype);
    Object.defineProperty(this, 'upload', { value: upload, enumerable: true, configurable: true });
    xhrStates.set(this, {
      state: UNSENT, send: false, method: 'GET', url: '', async: true, headers: [], responseType: '',
      timeout: 0, withCredentials: false, generation: 0, id: 0, timer: null, uploadComplete: false,
      uploadListener: false, status: 0, statusText: '', responseURL: '', responseHeaders: [],
      chunks: [], received: 0, total: 0, lengthComputable: false, lastProgress: 0, overrideMime: null,
      failed: false, cache: null
    });
  }
  inherit(XMLHttpRequest, XMLHttpRequestEventTarget);
  defineEventHandlers(XMLHttpRequest.prototype, ['readystatechange']);
  [['UNSENT', UNSENT], ['OPENED', OPENED], ['HEADERS_RECEIVED', HEADERS_RECEIVED], ['LOADING', LOADING],
   ['DONE', DONE]].forEach(function (constant) {
    XMLHttpRequest[constant[0]] = constant[1];
    XMLHttpRequest.prototype[constant[0]] = constant[1];
  });

  function xhrOf(value, member) {
    var x = value !== null && typeof value === 'object' ? xhrStates.get(value) : undefined;
    if (!x) throw new TypeError("Failed to execute '" + member + "' on 'XMLHttpRequest': Illegal invocation");
    return x;
  }
  function xhrEvent(target, type, loaded, total, computable) {
    var event = new ProgressEvent(type, { loaded: loaded, total: total, lengthComputable: computable });
    event.isTrusted = true;
    dispatchNow(target, event);
  }
  function readyStateChange(xhr) {
    var event = new Event('readystatechange');
    event.isTrusted = true;
    dispatchNow(xhr, event);
  }
  function hasListeners(target) {
    var byType = listenerTable.get(target);
    for (var i = 0; i < XHR_EVENTS.length; i++) {
      if (typeof target['on' + XHR_EVENTS[i]] === 'function') return true;
      if (byType && byType[XHR_EVENTS[i]] && byType[XHR_EVENTS[i]].length) return true;
    }
    return false;
  }
  // Stop whatever fetch is running for this object. Its later events are dropped
  // by generation, and the connection is closed.
  function terminateXhr(x) {
    x.generation++;
    if (x.id) {
      if (net !== null) net.abort(x.id);
      x.id = 0;
    }
    if (x.timer !== null) {
      global.clearTimeout(x.timer);
      x.timer = null;
    }
  }
  // The request error steps: a network error, an abort or a timeout.
  function xhrRequestError(xhr, x, type) {
    x.state = DONE;
    x.send = false;
    x.status = 0;
    x.statusText = '';
    x.responseURL = '';
    x.responseHeaders = [];
    x.chunks = [];
    x.received = 0;
    x.cache = null;
    x.failed = true;
    readyStateChange(xhr);
    if (!x.uploadComplete) {
      x.uploadComplete = true;
      if (x.uploadListener) {
        xhrEvent(xhr.upload, type, 0, 0, false);
        xhrEvent(xhr.upload, 'loadend', 0, 0, false);
      }
    }
    xhrEvent(xhr, type, 0, 0, false);
    xhrEvent(xhr, 'loadend', 0, 0, false);
  }
  function xhrBytes(x) {
    if (x.chunks.length !== 1) {
      x.chunks = [concatBytes(x.chunks, x.received)];
    }
    return x.chunks[0];
  }
  function xhrMime(x) {
    if (x.overrideMime !== null) return x.overrideMime;
    for (var i = 0; i < x.responseHeaders.length; i++) {
      if (x.responseHeaders[i][0] === 'content-type') return x.responseHeaders[i][1];
    }
    return '';
  }

  define(XMLHttpRequest.prototype, {
    readyState: { get: function () { return xhrOf(this, 'readyState').state; } },
    status: { get: function () { return xhrOf(this, 'status').status; } },
    statusText: { get: function () { return xhrOf(this, 'statusText').statusText; } },
    responseURL: { get: function () { return xhrOf(this, 'responseURL').responseURL; } },
    responseXML: { get: function () { xhrOf(this, 'responseXML'); return null; } },
    timeout: {
      get: function () { return xhrOf(this, 'timeout').timeout; },
      set: function (value) {
        var x = xhrOf(this, 'timeout');
        if (x.state !== UNSENT && !x.async) {
          throw domError("Failed to set the 'timeout' property on 'XMLHttpRequest': Timeouts cannot be set for synchronous requests", 'InvalidAccessError');
        }
        x.timeout = Math.max(0, Number(value) >>> 0);
      }
    },
    withCredentials: {
      get: function () { return xhrOf(this, 'withCredentials').withCredentials; },
      set: function (value) {
        var x = xhrOf(this, 'withCredentials');
        if ((x.state !== UNSENT && x.state !== OPENED) || x.send) {
          throw domError("Failed to set the 'withCredentials' property on 'XMLHttpRequest': The value may only be set if the object's state is UNSENT or OPENED.", 'InvalidStateError');
        }
        // Accepted, and it gates nothing: the jar applies to every request
        // (runtime/js/README.md, "Cookies").
        x.withCredentials = !!value;
      }
    },
    responseType: {
      get: function () { return xhrOf(this, 'responseType').responseType; },
      set: function (value) {
        var x = xhrOf(this, 'responseType');
        if (x.state === LOADING || x.state === DONE) {
          throw domError("Failed to set the 'responseType' property on 'XMLHttpRequest': The response type cannot be set if the object's state is LOADING or DONE.", 'InvalidStateError');
        }
        var type = String(value);
        if (['', 'arraybuffer', 'blob', 'document', 'json', 'text'].indexOf(type) < 0) return;
        if (x.state !== UNSENT && !x.async) {
          throw domError("Failed to set the 'responseType' property on 'XMLHttpRequest': The response type cannot be changed for synchronous requests", 'InvalidAccessError');
        }
        if (type === 'document') {
          announce('xhr:document', "ScreenKit: XMLHttpRequest responseType 'document' is not supported -- there is no HTML or XML parser; response will be null.");
        }
        x.responseType = type;
      }
    },
    responseText: {
      get: function () {
        var x = xhrOf(this, 'responseText');
        if (x.responseType !== '' && x.responseType !== 'text') {
          throw domError("Failed to read the 'responseText' property from 'XMLHttpRequest': The value is only accessible if the object's 'responseType' is '' or 'text' (was '" + x.responseType + "').", 'InvalidStateError');
        }
        if ((x.state !== LOADING && x.state !== DONE) || x.failed) return '';
        return bytesToText(xhrBytes(x));
      }
    },
    response: {
      get: function () {
        var x = xhrOf(this, 'response');
        if (x.responseType === '' || x.responseType === 'text') {
          if ((x.state !== LOADING && x.state !== DONE) || x.failed) return '';
          return bytesToText(xhrBytes(x));
        }
        if (x.state !== DONE || x.failed) return null;
        if (x.cache !== null) return x.cache.value;
        var value = null;
        if (x.responseType === 'arraybuffer') {
          value = xhrBytes(x).slice().buffer;
        } else if (x.responseType === 'blob') {
          value = x.assetBlob || blobFromBytes(xhrBytes(x), xhrMime(x));
        } else if (x.responseType === 'json') {
          try {
            value = JSON.parse(bytesToText(xhrBytes(x)));
          } catch (e) {
            value = null;
          }
        }
        x.cache = { value: value };
        return value;
      }
    },
    open: function (method, url, async) {
      var x = xhrOf(this, 'open');
      if (arguments.length < 2) {
        throw new TypeError("Failed to execute 'open' on 'XMLHttpRequest': 2 arguments required, but only " + arguments.length + ' present.');
      }
      var m;
      try {
        m = normalizeMethod(method);
      } catch (e) {
        throw domError("Failed to execute 'open' on 'XMLHttpRequest': " + e.message, /unsupported/.test(e.message) ? 'SecurityError' : 'SyntaxError');
      }
      var href;
      try {
        href = requestUrl(url, 'XMLHttpRequest');
      } catch (e) {
        throw domError("Failed to execute 'open' on 'XMLHttpRequest': Invalid URL", 'SyntaxError');
      }
      var isAsync = arguments.length < 3 ? true : !!async;
      if (!isAsync && (x.timeout !== 0 || x.responseType !== '')) {
        throw domError("Failed to execute 'open' on 'XMLHttpRequest': Synchronous requests cannot have a timeout or a responseType", 'InvalidAccessError');
      }
      terminateXhr(x);
      x.send = false;
      x.uploadListener = false;
      x.method = m;
      x.url = href;
      x.async = isAsync;
      x.headers = [];
      x.status = 0;
      x.statusText = '';
      x.responseURL = '';
      x.responseHeaders = [];
      x.chunks = [];
      x.received = 0;
      x.cache = null;
      x.failed = false;
      x.assetBlob = null;
      if (x.state !== OPENED) {
        x.state = OPENED;
        readyStateChange(this);
      }
    },
    setRequestHeader: function (name, value) {
      var x = xhrOf(this, 'setRequestHeader');
      if (x.state !== OPENED || x.send) {
        throw domError("Failed to execute 'setRequestHeader' on 'XMLHttpRequest': The object's state must be OPENED.", 'InvalidStateError');
      }
      var lower, v;
      try {
        lower = headerName(name);
        v = headerValue(value);
      } catch (e) {
        throw domError("Failed to execute 'setRequestHeader' on 'XMLHttpRequest': " + e.message, 'SyntaxError');
      }
      if (forbiddenRequestHeader(lower)) return;
      for (var i = 0; i < x.headers.length; i++) {
        if (x.headers[i][0].toLowerCase() === lower) {
          x.headers[i][1] += ', ' + v;
          return;
        }
      }
      x.headers.push([String(name), v]);
    },
    getResponseHeader: function (name) {
      var x = xhrOf(this, 'getResponseHeader');
      if (x.state < HEADERS_RECEIVED) return null;
      var lower = String(name).toLowerCase(), values = [];
      for (var i = 0; i < x.responseHeaders.length; i++) {
        if (x.responseHeaders[i][0] === lower) values.push(x.responseHeaders[i][1]);
      }
      return values.length ? values.join(', ') : null;
    },
    getAllResponseHeaders: function () {
      var x = xhrOf(this, 'getAllResponseHeaders');
      if (x.state < HEADERS_RECEIVED) return '';
      var names = [], combined = Object.create(null);
      x.responseHeaders.forEach(function (p) {
        if (!(p[0] in combined)) { names.push(p[0]); combined[p[0]] = p[1]; }
        else combined[p[0]] += ', ' + p[1];
      });
      names.sort();
      return names.map(function (n) { return n + ': ' + combined[n] + '\r\n'; }).join('');
    },
    overrideMimeType: function (mime) {
      var x = xhrOf(this, 'overrideMimeType');
      if (x.state === LOADING || x.state === DONE) {
        throw domError("Failed to execute 'overrideMimeType' on 'XMLHttpRequest': MimeType cannot be overridden when the state is LOADING or DONE.", 'InvalidStateError');
      }
      x.overrideMime = String(mime);
    },
    abort: function () {
      var x = xhrOf(this, 'abort');
      terminateXhr(x);
      if ((x.state === OPENED && x.send) || x.state === HEADERS_RECEIVED || x.state === LOADING) {
        xhrRequestError(this, x, 'abort');
      }
      if (x.state === DONE) {
        x.state = UNSENT;
        x.status = 0;
        x.statusText = '';
        x.chunks = [];
        x.received = 0;
        x.cache = null;
        x.failed = true;
      }
    },
    send: function (data) {
      var x = xhrOf(this, 'send');
      if (x.state !== OPENED || x.send) {
        throw domError("Failed to execute 'send' on 'XMLHttpRequest': The object's state must be OPENED.", 'InvalidStateError');
      }
      var extracted = null;
      if (x.method !== 'GET' && x.method !== 'HEAD' && data !== undefined && data !== null) {
        if (data instanceof ReadableStream) {
          throw new TypeError("Failed to execute 'send' on 'XMLHttpRequest': a ReadableStream body is not supported; use fetch");
        }
        extracted = extractBody(data);
        var hasType = x.headers.some(function (p) { return p[0].toLowerCase() === 'content-type'; });
        if (extracted.type !== null && !hasType) x.headers.push(['Content-Type', extracted.type]);
      }
      x.uploadComplete = extracted === null || extracted.bytes.length === 0;
      x.failed = false;
      var xhr = this;
      if (!x.async) return sendSync(xhr, x);

      x.uploadListener = hasListeners(xhr.upload);
      x.send = true;
      xhrEvent(xhr, 'loadstart', 0, 0, false);
      if (!x.uploadComplete && x.uploadListener) xhrEvent(xhr.upload, 'loadstart', 0, extracted.bytes.length, true);
      if (x.state !== OPENED || !x.send) return;

      var generation = ++x.generation;
      if (x.timeout > 0) {
        x.timer = global.setTimeout(function () {
          x.timer = null;
          if (generation !== x.generation || !x.send) return;
          terminateXhr(x);
          xhrRequestError(xhr, x, 'timeout');
        }, x.timeout);
      }
      if (isNetworkUrl(x.url)) sendNetwork(xhr, x, extracted, generation);
      else sendAsset(xhr, x, generation);
    }
  });
  global.XMLHttpRequest = XMLHttpRequest;

  function xhrReceive(xhr, x, bytes, final) {
    if (bytes !== null && bytes.length > 0) {
      x.chunks.push(bytes);
      x.received += bytes.length;
      x.cache = null;
    }
    if (final) return;
    var now = Date.now();
    if (x.lastProgress !== 0 && now - x.lastProgress < 50) return;
    x.lastProgress = now;
    if (x.state === HEADERS_RECEIVED) x.state = LOADING;
    readyStateChange(xhr);
    xhrEvent(xhr, 'progress', x.received, x.total, x.lengthComputable);
  }
  function xhrHeaders(xhr, x, status, statusText, url, headers) {
    x.status = status;
    x.statusText = statusText;
    x.responseURL = url;
    x.responseHeaders = headers;
    var length = null, encoded = false;
    headers.forEach(function (p) {
      if (p[0] === 'content-length') length = Number(p[1]);
      if (p[0] === 'content-encoding') encoded = true;
    });
    x.lengthComputable = length !== null && isFinite(length) && !encoded;
    x.total = x.lengthComputable ? length : 0;
    x.lastProgress = 0;
    x.state = HEADERS_RECEIVED;
    readyStateChange(xhr);
  }
  function xhrEnd(xhr, x) {
    if (x.timer !== null) {
      global.clearTimeout(x.timer);
      x.timer = null;
    }
    x.id = 0;
    xhrEvent(xhr, 'progress', x.received, x.total, x.lengthComputable);
    x.state = DONE;
    x.send = false;
    readyStateChange(xhr);
    xhrEvent(xhr, 'load', x.received, x.total, x.lengthComputable);
    xhrEvent(xhr, 'loadend', x.received, x.total, x.lengthComputable);
  }
  function xhrUploadDone(xhr, x, loaded, total) {
    if (x.uploadComplete) return;
    x.uploadComplete = true;
    if (!x.uploadListener) return;
    xhrEvent(xhr.upload, 'progress', loaded, total, total >= 0);
    xhrEvent(xhr.upload, 'load', loaded, total, total >= 0);
    xhrEvent(xhr.upload, 'loadend', loaded, total, total >= 0);
  }

  function sendNetwork(xhr, x, extracted, generation) {
    if (net === null) {
      later(function () {
        if (generation !== x.generation) return;
        terminateXhr(x);
        xhrRequestError(xhr, x, 'error');
      });
      return;
    }
    var options = { method: x.method, url: x.url, headers: x.headers.slice(), redirect: 'follow', cookies: true,
                    upload: x.uploadListener };
    if (extracted !== null) options.body = extracted.bytes;
    var total = extracted === null ? 0 : extracted.bytes.length;
    try {
      x.id = net.request(options, function (type, payload) {
        if (generation !== x.generation) return;
        if (type === 'upload') {
          if (payload.complete) {
            xhrUploadDone(xhr, x, payload.loaded, payload.total);
          } else if (x.uploadListener && !x.uploadComplete) {
            xhrEvent(xhr.upload, 'progress', payload.loaded, payload.total, payload.total >= 0);
          }
        } else if (type === 'head') {
          xhrUploadDone(xhr, x, total, total);
          if (generation !== x.generation) return;
          xhrHeaders(xhr, x, payload.status, payload.statusText, payload.url,
                     payload.headers.filter(function (p) { return !forbiddenResponseHeader(p[0]); }));
        } else if (type === 'data') {
          xhrReceive(xhr, x, new Uint8Array(payload), false);
        } else if (type === 'end') {
          xhrEnd(xhr, x);
        } else if (type === 'error') {
          x.id = 0;
          terminateXhr(x);
          xhrRequestError(xhr, x, 'error');
        }
      });
    } catch (e) {
      later(function () {
        if (generation !== x.generation) return;
        terminateXhr(x);
        xhrRequestError(xhr, x, 'error');
      });
    }
  }

  function readAsset(x) {
    if (x.method !== 'GET' && x.method !== 'HEAD') throw new Error(x.method + ' is not supported; assets are read-only');
    var res = resolveResource(x.url);
    return res.blob || blobFromAsset(res.path);
  }

  // A package asset through XMLHttpRequest: the same event sequence a network
  // response produces, one task later. A missing asset keeps the behaviour
  // Lightning was measured against: DONE, status 404 and an error event.
  function sendAsset(xhr, x, generation) {
    later(function () {
      if (generation !== x.generation || !x.send) return;
      var blob;
      try {
        blob = readAsset(x);
      } catch (err) {
        terminateXhr(x);
        x.state = DONE;
        x.send = false;
        x.status = /no such asset/.test(String(err && err.message)) ? 404 : 0;
        x.statusText = String(err && err.message);
        x.failed = true;
        readyStateChange(xhr);
        xhrEvent(xhr, 'error', 0, 0, false);
        xhrEvent(xhr, 'loadend', 0, 0, false);
        return;
      }
      xhrUploadDone(xhr, x, 0, 0);
      // HEAD: the headers, not the body.
      var head = x.method === 'HEAD';
      x.assetBlob = head ? null : blob;
      xhrHeaders(xhr, x, 200, 'OK', x.url,
                 (blob.type ? [['content-type', blob.type]] : []).concat([['content-length', String(blob.size)]]));
      if (generation !== x.generation) return;
      xhrReceive(xhr, x, head ? null : blob._bytes, true);
      xhrEnd(xhr, x);
    });
  }

  function sendSync(xhr, x) {
    if (isNetworkUrl(x.url)) {
      throw domError("Failed to execute 'send' on 'XMLHttpRequest': synchronous requests over the network are not supported in ScreenKit -- network I/O never blocks the JS thread", 'NetworkError');
    }
    x.send = true;
    var blob;
    try {
      blob = readAsset(x);
    } catch (err) {
      x.state = DONE;
      x.send = false;
      x.failed = true;
      x.status = /no such asset/.test(String(err && err.message)) ? 404 : 0;
      readyStateChange(xhr);
      throw domError("Failed to execute 'send' on 'XMLHttpRequest': " + String(err && err.message), 'NetworkError');
    }
    var head = x.method === 'HEAD';
    x.assetBlob = head ? null : blob;
    x.status = 200;
    x.statusText = 'OK';
    x.responseURL = x.url;
    x.responseHeaders = blob.type ? [['content-type', blob.type]] : [];
    x.chunks = head ? [] : [blob._bytes];
    x.received = head ? 0 : blob.size;
    x.state = DONE;
    x.send = false;
    readyStateChange(xhr);
    xhrEvent(xhr, 'load', x.received, x.received, true);
    xhrEvent(xhr, 'loadend', x.received, x.received, true);
  }

  // --- WebSocket --------------------------------------------------------------------

  var socketStates = new WeakMap();
  var WS_CONNECTING = 0, WS_OPEN = 1, WS_CLOSING = 2, WS_CLOSED = 3;

  function WebSocket(url, protocols) {
    if (!(this instanceof WebSocket)) throw new TypeError("Failed to construct 'WebSocket': Please use the 'new' operator.");
    if (arguments.length < 1) throw new TypeError("Failed to construct 'WebSocket': 1 argument required, but only 0 present.");
    var href;
    try {
      href = requestUrl(url, 'WebSocket');
    } catch (e) {
      throw domError("Failed to construct 'WebSocket': The URL '" + url + "' is invalid.", 'SyntaxError');
    }
    href = href.replace(/^http(s?):/i, 'ws$1:');
    if (!/^wss?:\/\//i.test(href)) {
      throw domError("Failed to construct 'WebSocket': The URL's scheme must be either 'http', 'https', 'ws', or 'wss'. '" + href + "' is not allowed.", 'SyntaxError');
    }
    if (href.indexOf('#') >= 0) {
      throw domError("Failed to construct 'WebSocket': The URL contains a fragment identifier ('" + href.slice(href.indexOf('#')) + "'). Fragment identifiers are not allowed in WebSocket URLs.", 'SyntaxError');
    }
    var list = protocols === undefined ? [] : (typeof protocols === 'string' ? [protocols] : Array.prototype.slice.call(protocols));
    list = list.map(String);
    for (var i = 0; i < list.length; i++) {
      if (!HEADER_NAME.test(list[i]) || list.indexOf(list[i]) !== i) {
        throw domError("Failed to construct 'WebSocket': The subprotocol '" + list[i] + "' is invalid.", 'SyntaxError');
      }
    }
    var self = this;
    var s = { url: href, readyState: WS_CONNECTING, protocol: '', extensions: '', binaryType: 'blob', bufferedAmount: 0, id: 0 };
    socketStates.set(this, s);
    if (net === null) {
      s.readyState = WS_CLOSED;
      later(function () {
        dispatchNow(self, trustedEvent(new Event('error')));
        dispatchNow(self, trustedEvent(new CloseEvent('close', { wasClean: false, code: 1006, reason: '' })));
      });
      return;
    }
    var origin = parseUrl(href).origin;
    try {
      s.id = net.openSocket(href, list, function (type, payload) {
        if (type === 'open') {
          // close() during CONNECTING already decided this socket fails; an
          // open that raced it is not announced.
          if (s.readyState !== WS_CONNECTING) return;
          s.readyState = WS_OPEN;
          s.protocol = payload.protocol;
          s.extensions = payload.extensions;
          dispatchNow(self, trustedEvent(new Event('open')));
        } else if (type === 'message') {
          if (s.readyState !== WS_OPEN) return;
          var data = payload;
          if (typeof payload !== 'string') {
            data = s.binaryType === 'arraybuffer' ? payload : blobFromBytes(new Uint8Array(payload), '');
          }
          dispatchNow(self, trustedEvent(new MessageEvent('message', { data: data, origin: origin })));
        } else if (type === 'sent') {
          s.bufferedAmount = Math.max(0, s.bufferedAmount - payload);
        } else if (type === 'error') {
          // A failed connection is CLOSED by the time its error is dispatched.
          s.readyState = WS_CLOSED;
          s.errored = true;
          dispatchNow(self, trustedEvent(new Event('error')));
        } else if (type === 'close') {
          s.readyState = WS_CLOSED;
          if (s.failed) {
            // close() during CONNECTING fails the connection (WebSockets
            // standard), whatever the handshake had got to natively: error,
            // then close 1006, not clean.
            if (!s.errored) {
              s.errored = true;
              dispatchNow(self, trustedEvent(new Event('error')));
            }
            dispatchNow(self, trustedEvent(new CloseEvent('close', { wasClean: false, code: 1006, reason: '' })));
            return;
          }
          dispatchNow(self, trustedEvent(new CloseEvent('close', { wasClean: payload.wasClean, code: payload.code, reason: payload.reason })));
        }
      });
    } catch (e) {
      throw domError("Failed to construct 'WebSocket': " + (e && e.message), 'SyntaxError');
    }
  }
  inherit(WebSocket, EventTarget);
  defineEventHandlers(WebSocket.prototype, ['open', 'message', 'error', 'close']);
  [['CONNECTING', WS_CONNECTING], ['OPEN', WS_OPEN], ['CLOSING', WS_CLOSING], ['CLOSED', WS_CLOSED]].forEach(function (c) {
    WebSocket[c[0]] = c[1];
    WebSocket.prototype[c[0]] = c[1];
  });
  function trustedEvent(event) {
    event.isTrusted = true;
    return event;
  }
  function socketOf(value, member) {
    var s = value !== null && typeof value === 'object' ? socketStates.get(value) : undefined;
    if (!s) throw new TypeError("Failed to execute '" + member + "' on 'WebSocket': Illegal invocation");
    return s;
  }
  define(WebSocket.prototype, {
    url: { get: function () { return socketOf(this, 'url').url; } },
    readyState: { get: function () { return socketOf(this, 'readyState').readyState; } },
    bufferedAmount: { get: function () { return socketOf(this, 'bufferedAmount').bufferedAmount; } },
    extensions: { get: function () { return socketOf(this, 'extensions').extensions; } },
    protocol: { get: function () { return socketOf(this, 'protocol').protocol; } },
    binaryType: {
      get: function () { return socketOf(this, 'binaryType').binaryType; },
      set: function (value) {
        var s = socketOf(this, 'binaryType');
        if (value === 'blob' || value === 'arraybuffer') s.binaryType = value;
      }
    },
    send: function (data) {
      var s = socketOf(this, 'send');
      if (arguments.length < 1) throw new TypeError("Failed to execute 'send' on 'WebSocket': 1 argument required, but only 0 present.");
      if (s.readyState === WS_CONNECTING) {
        throw domError("Failed to execute 'send' on 'WebSocket': Still in CONNECTING state.", 'InvalidStateError');
      }
      var payload, length;
      if (data instanceof Blob) {
        payload = data._bytes;
        length = data.size;
      } else if (isBufferSource(data)) {
        payload = data;
        length = data.byteLength;
      } else {
        payload = String(data);
        length = utf8Bytes(payload).length;
      }
      // Once closing, data is counted and dropped, as the spec says.
      if (s.readyState === WS_OPEN) net.send(s.id, payload);
      s.bufferedAmount += length;
    },
    close: function (code, reason) {
      var s = socketOf(this, 'close');
      if (code !== undefined) {
        code = Number(code) & 0xffff;
        if (code !== 1000 && !(code >= 3000 && code <= 4999)) {
          throw domError("Failed to execute 'close' on 'WebSocket': The close code must be either 1000, or between 3000 and 4999. " + code + ' is neither.', 'InvalidAccessError');
        }
      }
      var reasonText = reason === undefined ? '' : String(reason);
      if (utf8Bytes(reasonText).length > 123) {
        throw domError("Failed to execute 'close' on 'WebSocket': The close reason must not be greater than 123 UTF-8 bytes.", 'SyntaxError');
      }
      if (s.readyState === WS_CLOSING || s.readyState === WS_CLOSED) return;
      if (s.readyState === WS_CONNECTING) s.failed = true;
      s.readyState = WS_CLOSING;
      // A reason without a code is sent with 1000, as the standard says; an
      // empty close frame could not carry the reason at all.
      if (code === undefined && reasonText !== '') code = 1000;
      if (net !== null) net.close(s.id, code === undefined ? 0 : code, reasonText);
    }
  });
  global.WebSocket = WebSocket;

  // --- EventSource ------------------------------------------------------------------

  var eventSourceStates = new WeakMap();
  var ES_CONNECTING = 0, ES_OPEN = 1, ES_CLOSED = 2;

  function EventSource(url, init) {
    if (!(this instanceof EventSource)) throw new TypeError("Failed to construct 'EventSource': Please use the 'new' operator.");
    if (arguments.length < 1) throw new TypeError("Failed to construct 'EventSource': 1 argument required, but only 0 present.");
    var href;
    try {
      href = requestUrl(url, 'EventSource');
    } catch (e) {
      throw domError("Failed to construct 'EventSource': Cannot open an EventSource to '" + url + "'. The URL is invalid.", 'SyntaxError');
    }
    if (!isNetworkUrl(href)) {
      throw domError("Failed to construct 'EventSource': Cannot open an EventSource to '" + href + "': only http: and https: URLs are supported.", 'SyntaxError');
    }
    var self = this;
    var es = {
      url: href, withCredentials: !!(init && init.withCredentials), readyState: ES_CONNECTING,
      origin: parseUrl(href).origin, lastEventId: '', reconnectMs: 3000, id: 0, generation: 0, timer: null
    };
    eventSourceStates.set(this, es);
    connectEventSource(this, es);
  }
  inherit(EventSource, EventTarget);
  defineEventHandlers(EventSource.prototype, ['open', 'message', 'error']);
  [['CONNECTING', ES_CONNECTING], ['OPEN', ES_OPEN], ['CLOSED', ES_CLOSED]].forEach(function (c) {
    EventSource[c[0]] = c[1];
    EventSource.prototype[c[0]] = c[1];
  });

  function connectEventSource(source, es) {
    var generation = ++es.generation;
    if (net === null) {
      later(function () { failEventSource(source, es); });
      return;
    }
    var headers = [['Accept', 'text/event-stream'], ['Cache-Control', 'no-cache']];
    if (es.lastEventId !== '') headers.push(['Last-Event-ID', es.lastEventId]);
    var decoder = typeof global.TextDecoder === 'function' ? new global.TextDecoder('utf-8') : null;
    var parser = { buffer: '', lastWasCR: false, data: '', type: '', id: es.lastEventId };
    try {
      es.id = net.request({ method: 'GET', url: es.url, headers: headers, redirect: 'follow', cookies: true },
        function (type, payload) {
          if (generation !== es.generation || es.readyState === ES_CLOSED) return;
          if (type === 'head') {
            var contentType = '';
            payload.headers.forEach(function (p) { if (p[0] === 'content-type') contentType = p[1]; });
            if (payload.status !== 200 || contentType.split(';')[0].trim().toLowerCase() !== 'text/event-stream') {
              if (es.id && net !== null) net.abort(es.id);
              es.id = 0;
              failEventSource(source, es);
              return;
            }
            es.readyState = ES_OPEN;
            dispatchNow(source, trustedEvent(new Event('open')));
          } else if (type === 'data') {
            var bytes = new Uint8Array(payload);
            var text = decoder ? decoder.decode(bytes, { stream: true }) : bytesToText(bytes);
            parseEventStream(source, es, parser, text);
          } else if (type === 'end' || type === 'error') {
            es.id = 0;
            // Retrying cannot help a certificate the OS refused, a platform with
            // no network backend or a URL the client cannot reach: those fail
            // for good, or the source would reconnect forever and never idle.
            if (type === 'error' && (payload.kind === 'tls' || payload.kind === 'unsupported' || payload.kind === 'url')) {
              failEventSource(source, es);
            } else {
              reestablishEventSource(source, es);
            }
          }
        });
    } catch (e) {
      later(function () { failEventSource(source, es); });
    }
  }

  function failEventSource(source, es) {
    if (es.readyState === ES_CLOSED) return;
    es.readyState = ES_CLOSED;
    dispatchNow(source, trustedEvent(new Event('error')));
  }

  function reestablishEventSource(source, es) {
    if (es.readyState === ES_CLOSED) return;
    es.readyState = ES_CONNECTING;
    dispatchNow(source, trustedEvent(new Event('error')));
    if (es.readyState !== ES_CONNECTING) return;
    es.timer = global.setTimeout(function () {
      es.timer = null;
      if (es.readyState !== ES_CONNECTING) return;
      connectEventSource(source, es);
    }, es.reconnectMs);
  }

  // The text/event-stream interpretation (HTML 9.2.6), fed as text arrives. A
  // line ends at CRLF, LF or CR, including a CR at the end of one chunk whose LF
  // starts the next.
  function parseEventStream(source, es, parser, text) {
    var i = 0;
    if (parser.lastWasCR && text.charAt(0) === '\n') i = 1;
    parser.lastWasCR = false;
    for (; i < text.length; i++) {
      var c = text.charAt(i);
      if (c !== '\r' && c !== '\n') {
        parser.buffer += c;
        continue;
      }
      if (c === '\r') {
        if (i + 1 < text.length) {
          if (text.charAt(i + 1) === '\n') i++;
        } else {
          parser.lastWasCR = true;
        }
      }
      var line = parser.buffer;
      parser.buffer = '';
      eventStreamLine(source, es, parser, line);
      if (es.readyState === ES_CLOSED) return;
    }
  }

  function eventStreamLine(source, es, parser, line) {
    if (line === '') {
      es.lastEventId = parser.id;
      if (parser.data === '') {
        parser.type = '';
        return;
      }
      var data = parser.data.charAt(parser.data.length - 1) === '\n' ? parser.data.slice(0, -1) : parser.data;
      var event = trustedEvent(new MessageEvent(parser.type || 'message', { data: data, origin: es.origin, lastEventId: es.lastEventId }));
      parser.data = '';
      parser.type = '';
      if (es.readyState !== ES_CLOSED) dispatchNow(source, event);
      return;
    }
    if (line.charAt(0) === ':') return;
    var colon = line.indexOf(':');
    var field = colon < 0 ? line : line.slice(0, colon);
    var value = colon < 0 ? '' : line.slice(colon + 1);
    if (value.charAt(0) === ' ') value = value.slice(1);
    if (field === 'event') parser.type = value;
    else if (field === 'data') parser.data += value + '\n';
    else if (field === 'id') { if (value.indexOf('\u0000') < 0) parser.id = value; }
    else if (field === 'retry') { if (/^[0-9]+$/.test(value)) es.reconnectMs = Number(value); }
  }

  function eventSourceOf(value, member) {
    var es = value !== null && typeof value === 'object' ? eventSourceStates.get(value) : undefined;
    if (!es) throw new TypeError("Failed to execute '" + member + "' on 'EventSource': Illegal invocation");
    return es;
  }
  define(EventSource.prototype, {
    url: { get: function () { return eventSourceOf(this, 'url').url; } },
    withCredentials: { get: function () { return eventSourceOf(this, 'withCredentials').withCredentials; } },
    readyState: { get: function () { return eventSourceOf(this, 'readyState').readyState; } },
    close: function () {
      var es = eventSourceOf(this, 'close');
      es.readyState = ES_CLOSED;
      es.generation++;
      if (es.timer !== null) {
        global.clearTimeout(es.timer);
        es.timer = null;
      }
      if (es.id && net !== null) net.abort(es.id);
      es.id = 0;
    }
  });
  global.EventSource = EventSource;

  // --- Images -----------------------------------------------------------------------

  // ImageBitmap carries a localUri when its pixels are a file the vendored
  // texImage2D decodes at upload, or `data` (RGBA) when they were decoded from
  // memory -- a downloaded image. Either way decoding happens once.
  //
  // `_premultiplied` is the bitmap's alpha state, and the texture upload reads
  // it: a bitmap is uploaded premultiplied unless it was created with
  // `premultiplyAlpha: 'none'`, whatever UNPACK_PREMULTIPLY_ALPHA_WEBGL says --
  // Chromium's behaviour, measured. Lightning asks for 'premultiply' on every PNG
  // and turns the unpack flag off, so uploading straight alpha drew transparent
  // pixels as white.
  function ImageBitmap(width, height, localUri, premultiplied) {
    this.width = width;
    this.height = height;
    this.localUri = localUri;
    this._premultiplied = premultiplied !== false;
    this._closed = false;
  }
  ImageBitmap.prototype.close = function () { this._closed = true; };
  global.ImageBitmap = ImageBitmap;

  function bitmapFromPath(path, premultiplied) {
    var info = io.imageInfo(path);
    var bitmap = new ImageBitmap(info.width, info.height, 'file://' + encodeURI(info.path), premultiplied);
    bitmap._assetPath = path;
    return bitmap;
  }

  // Bytes in memory -> {width, height, data}, or an InvalidStateError DOMException
  // saying why, as a browser rejects an undecodable image.
  function decodePixels(bytes) {
    try {
      var decoded = io.decodeImage(bytes);
      return { width: decoded.width, height: decoded.height, data: new Uint8Array(decoded.data) };
    } catch (err) {
      throw domError('The source image could not be decoded: ' + (err && err.message), 'InvalidStateError');
    }
  }
  // The same, decoded off the JS thread (`__screenkit.net.decodeImage`), so a
  // large image does not cost a frame. Without the binding, here and now.
  function decodePixelsLater(bytes) {
    if (net === null) {
      try {
        return Promise.resolve(decodePixels(bytes));
      } catch (err) {
        return Promise.reject(err);
      }
    }
    return new Promise(function (resolve, reject) {
      net.decodeImage(bytes, function (type, payload) {
        if (type === 'image') {
          resolve({ width: payload.width, height: payload.height, data: new Uint8Array(payload.data) });
        } else {
          reject(domError('The source image could not be decoded: ' + payload.message, 'InvalidStateError'));
        }
      });
    });
  }
  function bitmapFromPixels(pixels, premultiplied) {
    var bitmap = new ImageBitmap(pixels.width, pixels.height, null, premultiplied);
    bitmap.data = pixels.data;
    return bitmap;
  }

  // Any source createImageBitmap accepts, decoded: what a crop copies from. An
  // asset is otherwise handed to texImage2D as a file and never decoded here.
  // A promise, because the decode runs off the JS thread.
  function sourcePixels(source) {
    if (source instanceof ImageBitmap || source instanceof global.ImageData) {
      if (source.data) return Promise.resolve({ width: source.width, height: source.height, data: source.data });
      if (source._assetPath) return decodePixelsLater(io.readFile(source._assetPath));
    } else if (source && source._sourcePath) {
      return decodePixelsLater(io.readFile(source._sourcePath));
    } else if (source && source._assetPath) {
      return decodePixelsLater(io.readFile(source._assetPath));
    } else if (source && source._pixels) {
      return Promise.resolve(source._pixels);
    } else if (source instanceof HTMLCanvasElement) {
      var canvasPixels = imagePixels(source);
      if (canvasPixels === null) return Promise.reject(domError('The canvas has no 2D pixels to read.', 'InvalidStateError'));
      return Promise.resolve({ width: canvasPixels.width, height: canvasPixels.height, data: canvasPixels.data.slice() });
    } else if (source instanceof global.Blob) {
      return decodePixelsLater(source._bytes);
    }
    return Promise.reject(new TypeError('createImageBitmap: unsupported source'));
  }

  // The crop rectangle as Chromium normalises it: `long` arguments, and a
  // negative width or height extends the rectangle left or up from its origin.
  function cropRect(sx, sy, sw, sh) {
    var rect = { x: sx | 0, y: sy | 0, width: sw | 0, height: sh | 0 };
    if (rect.width < 0) { rect.x += rect.width; rect.width = -rect.width; }
    if (rect.height < 0) { rect.y += rect.height; rect.height = -rect.height; }
    return rect;
  }

  // The part of the source under `rect`. What falls outside the source is
  // transparent black, as in a browser.
  function cropPixels(pixels, rect) {
    var out = new Uint8Array(rect.width * rect.height * 4);
    var left = Math.max(rect.x, 0), right = Math.min(rect.x + rect.width, pixels.width);
    var top = Math.max(rect.y, 0), bottom = Math.min(rect.y + rect.height, pixels.height);
    for (var y = top; right > left && y < bottom; y++) {
      var from = (y * pixels.width + left) * 4;
      out.set(pixels.data.subarray(from, from + (right - left) * 4),
              ((y - rect.y) * rect.width + (left - rect.x)) * 4);
    }
    return { width: rect.width, height: rect.height, data: out };
  }

  // createImageBitmap(source[, options]) or (source, sx, sy, sw, sh[, options]).
  global.createImageBitmap = function (source) {
    var crop = arguments.length >= 5 ? cropRect(arguments[1], arguments[2], arguments[3], arguments[4]) : null;
    var options = crop !== null ? arguments[5] : arguments[1];
    var premultiplied = !(options && options.premultiplyAlpha === 'none');
    return new Promise(function (resolve, reject) {
      try {
        if (crop !== null && (crop.width === 0 || crop.height === 0)) {
          throw new RangeError("Failed to execute 'createImageBitmap' on 'Window': The crop rect " +
                               (crop.width === 0 ? 'width' : 'height') + ' is 0.');
        }
        if (crop !== null) {
          sourcePixels(source).then(function (pixels) {
            resolve(bitmapFromPixels(cropPixels(pixels, crop), premultiplied));
          }).catch(reject);
        } else if (source instanceof HTMLCanvasElement) {
          sourcePixels(source).then(function (pixels) { resolve(bitmapFromPixels(pixels, premultiplied)); }).catch(reject);
        } else if (source instanceof ImageBitmap) {
          var copy = new ImageBitmap(source.width, source.height, source.localUri, premultiplied);
          if (source.data) copy.data = source.data;
          if (source._assetPath) copy._assetPath = source._assetPath;
          resolve(copy);
        } else if (source && source._sourcePath) {
          resolve(bitmapFromPath(source._sourcePath, premultiplied));    // Blob from an asset
        } else if (source instanceof global.ImageData) {
          var bm = new ImageBitmap(source.width, source.height, null, premultiplied);
          bm.data = source.data;
          resolve(bm);
        } else if (source && source._assetPath) {
          resolve(bitmapFromPath(source._assetPath, premultiplied));     // an <img> of an asset
        } else if (source && source._pixels) {
          resolve(bitmapFromPixels(source._pixels, premultiplied));      // an <img> of a download
        } else if (source instanceof global.Blob) {
          decodePixelsLater(source._bytes).then(function (pixels) {    // downloaded bytes
            resolve(bitmapFromPixels(pixels, premultiplied));
          }, reject);
        } else {
          reject(new TypeError('createImageBitmap: unsupported source'));
        }
      } catch (err) {
        reject(err);
      }
    });
  };

  // An <img>'s bytes from the network: a GET through __screenkit.net, gathered
  // whole. `done(error, bytes)`.
  function downloadBytes(url, done) {
    if (net === null) {
      later(function () { done(noNetwork()); });
      return 0;
    }
    var chunks = [], total = 0, status = 0;
    try {
      return net.request({ method: 'GET', url: url, headers: [['Accept', 'image/*,*/*;q=0.8']], redirect: 'follow', cookies: true },
        function (type, payload) {
          if (type === 'head') status = payload.status;
          else if (type === 'data') { chunks.push(new Uint8Array(payload)); total += payload.byteLength; }
          else if (type === 'end') {
            if (status >= 200 && status < 300) done(null, concatBytes(chunks, total));
            else done(new Error('image request failed with status ' + status + ': ' + url));
          } else if (type === 'error') done(networkError('Failed to load ' + url + ': ' + payload.message));
        });
    } catch (e) {
      later(function () { done(networkError('Failed to load ' + url + ': ' + (e && e.message))); });
      return 0;
    }
  }

  function makeImage() {
    var img = newElement('img', HTMLImageElement.prototype);
    img.width = 0; img.height = 0; img.naturalWidth = 0; img.naturalHeight = 0;
    img.complete = false;
    img.crossOrigin = null;
    img.decoding = 'auto';
    var src = '';
    var loads = 0;
    var download = 0;
    // A downloaded image is pixels, not a file: texImage2D takes them from `data`
    // and the natural size (VendoredWebGL.cpp readImageSource).
    Object.defineProperty(img, 'data', {
      enumerable: false, configurable: true,
      get: function () { return img._pixels ? img._pixels.data : undefined; }
    });
    function loaded(width, height) {
      img.width = img.naturalWidth = width;
      img.height = img.naturalHeight = height;
      img.complete = true;
      fire('load');
    }
    function failed(err) {
      // Nothing of an earlier load survives a failed one: decode() rejects and
      // texImage2D has no stale pixels to upload.
      img._pixels = null;
      img._assetPath = null;
      delete img.localUri;
      img.complete = true;
      fire('error', err);
    }
    Object.defineProperty(img, 'src', {
      enumerable: true,
      get: function () { return src; },
      set: function (value) {
        src = String(value);
        img.complete = false;
        var load = ++loads;
        // A download for the previous src is abandoned, and its connection closed.
        if (download !== 0 && net !== null) net.abort(download);
        download = 0;
        later(function () {
          if (load !== loads) return;
          var res;
          try {
            res = resolveResource(src);
          } catch (err) {
            return failed(err);
          }
          if (res.network) {
            download = downloadBytes(res.network, function (error, bytes) {
              if (load !== loads) return;
              download = 0;
              if (error) return failed(error);
              decodePixelsLater(bytes).then(function (pixels) {
                if (load !== loads) return;
                img._assetPath = null;
                delete img.localUri;
                img._pixels = pixels;
                loaded(pixels.width, pixels.height);
              }, function (err) {
                if (load === loads) failed(err);
              });
            });
            return;
          }
          try {
            var path = res.blob ? res.blob._sourcePath : res.path;
            if (!path && res.blob) {
              decodePixelsLater(res.blob._bytes).then(function (decoded) {
                if (load !== loads) return;
                img._assetPath = null;
                delete img.localUri;
                img._pixels = decoded;
                loaded(decoded.width, decoded.height);
              }, function (err) {
                if (load === loads) failed(err);
              });
              return;
            }
            if (!path) throw new Error('image source has no asset behind it: ' + src);
            var info = io.imageInfo(path);
            img._pixels = null;
            img._assetPath = path;
            img.localUri = 'file://' + encodeURI(info.path);
            loaded(info.width, info.height);
          } catch (err) {
            failed(err);
          }
        });
      }
    });
    // `load` and `error` are events at the element: `onload`/`onerror` run
    // among its listeners, all given the same Event.
    function fire(type, error) {
      var event = new Event(type);
      if (error !== undefined) event.error = error;
      dispatchNow(img, event);
    }
    img.decode = function () {
      return new Promise(function (resolve, reject) {
        if (img.complete && (img._assetPath || img._pixels)) return resolve();
        var prevLoad = img.onload, prevError = img.onerror;
        img.onload = function (e) { if (prevLoad) prevLoad(e); resolve(); };
        img.onerror = function (e) { if (prevError) prevError(e); reject(e.error); };
      });
    };
    return img;
  }
  // -------------------------------------------------------------------------
  // <video>: HTMLMediaElement over the platform's player
  //
  // Each <video> is backed by the platform's own player -- AVPlayer on Apple,
  // Media3 ExoPlayer on Android, libvlc on Linux -- through
  // `__screenkit.media` (core/src/bindings/Media.cpp), which owns manifests,
  // buffering, ABR, decoding and presentation. What is here is HTMLMediaElement:
  // the load algorithm, readyState / networkState, the event order, play()
  // promises, seeking, TextTracks, and the plane -- the element's CSS rect,
  // which the platform composites the video into *beneath* the app's canvas.
  // The app clears transparent where video shows.
  //
  // Three things are not the web's, and say so (runtime/js/README.md, "Video"):
  // video is always beneath the canvas whatever its z-index; there is no
  // MediaSource, so `srcObject`, `blob:` and MSE players are out (@screenkit/shaka
  // is the Shaka-shaped player that runs instead); and <audio> plays nothing.
  //
  // `video[Symbol.for('screenkit.media')]` is the controller @screenkit/shaka
  // drives: a load with DRM and ABR configuration, the platform's tracks and
  // stats, and licence requests.
  // -------------------------------------------------------------------------

  var NETWORK_EMPTY = 0, NETWORK_IDLE = 1, NETWORK_LOADING = 2, NETWORK_NO_SOURCE = 3;
  var HAVE_NOTHING = 0, HAVE_METADATA = 1, HAVE_CURRENT_DATA = 2, HAVE_FUTURE_DATA = 3, HAVE_ENOUGH_DATA = 4;
  var MEDIA_ERR_ABORTED = 1, MEDIA_ERR_NETWORK = 2, MEDIA_ERR_DECODE = 3, MEDIA_ERR_SRC_NOT_SUPPORTED = 4;
  var MEDIA_CONTROLLER = Symbol.for('screenkit.media');
  // The load algorithm with no source at all -- an unload -- as opposed to one
  // that selects the `src` attribute.
  var NO_SOURCE = {};

  function mediaApi() {
    var io = global.__screenkit;
    return io && io.media && typeof io.media.create === 'function' ? io.media : null;
  }

  // Asked each time rather than kept: on Linux whether there is a Wayland window
  // to show video on depends on the window the host has made by then.
  function mediaCaps() {
    var api = mediaApi();
    if (api === null) {
      sandboxRefusal('allow-media', '<video> and the platform player');
      return { available: false, platform: 'none', hls: false, dash: false, progressive: false, keySystems: [],
               containers: [], codecs: [], videoOutput: false,
               videoOutputProblem: 'this runtime has no media layer (__screenkit.media is missing)' };
    }
    return api.capabilities();
  }

  // --- MediaError, TimeRanges, VideoPlaybackQuality -------------------------------

  function MediaError() { throw new TypeError("Failed to construct 'MediaError': Illegal constructor"); }
  [['MEDIA_ERR_ABORTED', MEDIA_ERR_ABORTED], ['MEDIA_ERR_NETWORK', MEDIA_ERR_NETWORK],
   ['MEDIA_ERR_DECODE', MEDIA_ERR_DECODE], ['MEDIA_ERR_SRC_NOT_SUPPORTED', MEDIA_ERR_SRC_NOT_SUPPORTED]]
    .forEach(function (constant) {
      MediaError[constant[0]] = constant[1];
      MediaError.prototype[constant[0]] = constant[1];
    });
  function makeMediaError(code, message) {
    var error = Object.create(MediaError.prototype);
    Object.defineProperty(error, 'code', { value: code, enumerable: true });
    Object.defineProperty(error, 'message', { value: message || '', enumerable: true });
    return error;
  }
  global.MediaError = MediaError;

  var timeRangeLists = new WeakMap();
  function TimeRanges() { throw new TypeError("Failed to construct 'TimeRanges': Illegal constructor"); }
  function rangeList(ranges) {
    var list = timeRangeLists.get(ranges);
    if (!list) throw new TypeError('Illegal invocation');
    return list;
  }
  function rangeIndex(ranges, index, method) {
    var list = rangeList(ranges), i = Number(index) >>> 0;
    if (i >= list.length || Number(index) !== i) {
      throw domError("Failed to execute '" + method + "' on 'TimeRanges': The index provided (" + index +
                     ') is greater than or equal to the maximum bound (' + list.length + ').', 'IndexSizeError');
    }
    return list[i];
  }
  define(TimeRanges.prototype, {
    length: { get: function () { return rangeList(this).length; } },
    start: function (index) { return rangeIndex(this, index, 'start')[0]; },
    end: function (index) { return rangeIndex(this, index, 'end')[1]; }
  });
  // Normalised as a browser reports them: sorted, finite, overlapping ranges merged.
  function makeTimeRanges(ranges) {
    var sorted = (ranges || []).filter(function (r) { return isFinite(r[0]) && isFinite(r[1]) && r[1] >= r[0]; })
      .map(function (r) { return [r[0], r[1]]; })
      .sort(function (a, b) { return a[0] - b[0]; });
    var merged = [];
    sorted.forEach(function (r) {
      var last = merged[merged.length - 1];
      if (last && r[0] <= last[1]) last[1] = Math.max(last[1], r[1]);
      else merged.push(r);
    });
    var out = Object.create(TimeRanges.prototype);
    timeRangeLists.set(out, merged);
    return out;
  }
  global.TimeRanges = TimeRanges;

  function VideoPlaybackQuality() {
    throw new TypeError("Failed to construct 'VideoPlaybackQuality': Illegal constructor");
  }
  global.VideoPlaybackQuality = VideoPlaybackQuality;

  // --- Text tracks: TextTrackCue, VTTCue, TextTrack, TextTrackList -------------------
  //
  // The platform player hands over the cues of the selected text track that are
  // active now; they become VTTCues on that track and `cuechange` fires. Nothing
  // is rendered natively: the app draws `track.activeCues` of a 'showing' track.

  var cueTracks = new WeakMap();
  function TextTrackCue() { throw new TypeError("Failed to construct 'TextTrackCue': Illegal constructor"); }
  inherit(TextTrackCue, EventTarget);
  defineEventHandlers(TextTrackCue.prototype, ['enter', 'exit']);
  define(TextTrackCue.prototype, {
    track: { get: function () { return cueTracks.get(this) || null; } }
  });
  global.TextTrackCue = TextTrackCue;

  function VTTCue(startTime, endTime, text) {
    if (!(this instanceof VTTCue)) {
      throw new TypeError("Failed to construct 'VTTCue': Please use the 'new' operator, this DOM object constructor " +
                          'cannot be called as a function.');
    }
    if (arguments.length < 3) {
      throw new TypeError("Failed to construct 'VTTCue': 3 arguments required, but only " + arguments.length +
                          ' present.');
    }
    this.id = '';
    this.startTime = Number(startTime);
    this.endTime = Number(endTime);
    this.pauseOnExit = false;
    this.text = String(text);
    this.vertical = '';
    this.snapToLines = true;
    this.line = 'auto';
    this.lineAlign = 'start';
    this.position = 'auto';
    this.positionAlign = 'auto';
    this.size = 100;
    this.align = 'center';
    this.region = null;
  }
  inherit(VTTCue, TextTrackCue);
  // There are no document fragments here: the cue's text as one text node.
  VTTCue.prototype.getCueAsHTML = function () { return makeText(String(this.text), null); };
  global.VTTCue = VTTCue;

  var cueListStates = new WeakMap();
  function TextTrackCueList() { throw new TypeError("Failed to construct 'TextTrackCueList': Illegal constructor"); }
  define(TextTrackCueList.prototype, {
    getCueById: function (id) {
      var cues = cueListStates.get(this);
      if (!cues) throw new TypeError('Illegal invocation');
      var wanted = String(id);
      for (var i = 0; wanted !== '' && i < cues.length; i++) if (cues[i].id === wanted) return cues[i];
      return null;
    }
  });
  TextTrackCueList.prototype[Symbol.iterator] = Array.prototype.values;
  global.TextTrackCueList = TextTrackCueList;
  function makeCueList() {
    var list = Object.create(TextTrackCueList.prototype);
    cueListStates.set(list, []);
    fill(list, []);
    return list;
  }
  function setCueList(list, cues) {
    cueListStates.set(list, cues.slice());
    fill(list, cues);
  }

  var textTrackStates = new WeakMap();
  function TextTrack() { throw new TypeError("Failed to construct 'TextTrack': Illegal constructor"); }
  inherit(TextTrack, EventTarget);
  defineEventHandlers(TextTrack.prototype, ['cuechange']);
  function trackState(track) {
    var state = textTrackStates.get(track);
    if (!state) throw new TypeError('Illegal invocation');
    return state;
  }
  var TEXT_TRACK_MODES = table({ disabled: 1, hidden: 1, showing: 1 });
  define(TextTrack.prototype, {
    kind: { get: function () { return trackState(this).kind; } },
    label: { get: function () { return trackState(this).label; } },
    language: { get: function () { return trackState(this).language; } },
    id: { get: function () { return trackState(this).id; } },
    inBandMetadataTrackDispatchType: { get: function () { trackState(this); return ''; } },
    mode: {
      get: function () { return trackState(this).mode; },
      // An unknown mode is ignored, as the IDL enum's setter ignores it.
      set: function (value) {
        var state = trackState(this), mode = String(value);
        if (TEXT_TRACK_MODES[mode] !== 1 || mode === state.mode) return;
        state.mode = mode;
        if (mode === 'disabled') {
          state.cues = [];
          state.active = [];
          setCueList(state.cueList, []);
          setCueList(state.activeList, []);
        }
        if (state.media !== null) textModeChanged(state.media);
      }
    },
    cues: { get: function () { var s = trackState(this); return s.mode === 'disabled' ? null : s.cueList; } },
    activeCues: { get: function () { var s = trackState(this); return s.mode === 'disabled' ? null : s.activeList; } },
    addCue: function (cue) {
      var state = trackState(this);
      requireArgs(arguments.length, 1, 'addCue', 'TextTrack');
      if (!(cue instanceof TextTrackCue)) {
        throw new TypeError("Failed to execute 'addCue' on 'TextTrack': parameter 1 is not of type 'TextTrackCue'.");
      }
      var previous = cueTracks.get(cue);
      if (previous && previous !== this) previous.removeCue(cue);
      if (state.cues.indexOf(cue) >= 0) return;
      cueTracks.set(cue, this);
      state.cues.push(cue);
      state.cues.sort(function (a, b) { return a.startTime - b.startTime || b.endTime - a.endTime; });
      setCueList(state.cueList, state.cues);
    },
    removeCue: function (cue) {
      var state = trackState(this);
      var at = state.cues.indexOf(cue);
      if (at < 0) {
        throw domError("Failed to execute 'removeCue' on 'TextTrack': The specified cue is not listed in the " +
                       "TextTrack's list of cues.", 'NotFoundError');
      }
      state.cues.splice(at, 1);
      cueTracks.delete(cue);
      setCueList(state.cueList, state.cues);
      var active = state.active.indexOf(cue);
      if (active >= 0) {
        state.active.splice(active, 1);
        setCueList(state.activeList, state.active);
      }
    }
  });
  global.TextTrack = TextTrack;

  // `nativeId` is the platform player's id for an in-band track, null for one
  // the app made with addTextTrack.
  function makeTextTrack(kind, label, language, id, mode, nativeId) {
    var track = Object.create(TextTrack.prototype);
    textTrackStates.set(track, {
      kind: kind, label: label, language: language, id: id, mode: mode, nativeId: nativeId,
      cues: [], active: [], cueList: makeCueList(), activeList: makeCueList(), media: null
    });
    return track;
  }

  var trackListStates = new WeakMap();
  function TextTrackList() { throw new TypeError("Failed to construct 'TextTrackList': Illegal constructor"); }
  inherit(TextTrackList, EventTarget);
  defineEventHandlers(TextTrackList.prototype, ['change', 'addtrack', 'removetrack']);
  define(TextTrackList.prototype, {
    getTrackById: function (id) {
      var tracks = trackListStates.get(this);
      if (!tracks) throw new TypeError('Illegal invocation');
      var wanted = String(id);
      for (var i = 0; i < tracks.length; i++) if (trackState(tracks[i]).id === wanted) return tracks[i];
      return null;
    }
  });
  TextTrackList.prototype[Symbol.iterator] = Array.prototype.values;
  global.TextTrackList = TextTrackList;

  function TrackEvent(type, init) {
    Event.call(this, type, init);
    this.track = init && init.track !== undefined ? init.track : null;
  }
  TrackEvent.prototype = Object.create(Event.prototype);
  TrackEvent.prototype.constructor = TrackEvent;
  global.TrackEvent = TrackEvent;

  // --- the element ------------------------------------------------------------------

  var mediaStates = new WeakMap();
  // How many media elements were ever made: tree changes only look for them once
  // there is one.
  var mediaElementCount = 0;

  function mediaState(element) {
    var state = mediaStates.get(element);
    if (!state) throw new TypeError('Illegal invocation');
    return state;
  }

  function makeMedia(tag, proto) {
    var element = newElement(tag, proto);
    var node = nodes.get(element);
    var textTracks = Object.create(TextTrackList.prototype);
    trackListStates.set(textTracks, []);
    fill(textTracks, []);
    var state = {
      element: element, node: node, audio: tag === 'audio',
      // The platform player: its id, the object its events go to, the serial of
      // the latest load. Made on the first load, released on an unload.
      id: null, target: null, serial: -1,
      networkState: NETWORK_EMPTY, readyState: HAVE_NOTHING,
      paused: true, ended: false, seeking: false, error: null, autoplaying: true,
      currentTime: 0, defaultStart: NaN, duration: NaN, live: false, seekStart: 0, seekEnd: 0,
      defaultPlaybackRate: 1, playbackRate: 1, preservesPitch: true, volume: 1, muted: false,
      currentSrc: '', buffered: [], played: [], videoWidth: 0, videoHeight: 0,
      nativeState: 'idle', manifest: '', tracks: null, variant: -1, stats: null,
      pendingPlay: [], tasks: [], flushScheduled: false,
      textTracks: textTracks, nativeText: [], selectedText: -1,
      controller: null, controllerLoad: null, listeners: [],
      plane: null, connected: false, source: null
    };
    mediaStates.set(element, state);
    // Its style parses the CSS subset: the rect is where the plane goes.
    if (tag === 'video') node.backed = true;
    mediaElementCount++;
    return element;
  }

  // --- tasks and events ---------------------------------------------------------------
  //
  // What script starts (play(), a src change, a seek) fires its events from a
  // queued task, as HTML queues them; what the player reports arrives as a task
  // already. One FIFO per element keeps the two in order: a player event first
  // runs whatever the element still had queued.

  function queueMediaTask(state, task) {
    state.tasks.push(task);
    if (state.flushScheduled) return;
    state.flushScheduled = true;
    global.setTimeout(function () {
      state.flushScheduled = false;
      flushMediaTasks(state);
    }, 0);
  }
  function queueMediaEvent(state, type) {
    queueMediaTask(state, function () { fireMedia(state, type); });
  }
  function flushMediaTasks(state) {
    while (state.tasks.length > 0) {
      var task = state.tasks.shift();
      try {
        task();
      } catch (err) {
        global.console.error('Uncaught in a media task: ' + (err && err.stack || err));
      }
    }
  }
  function fireMedia(state, type) {
    var event = new Event(type);
    event.isTrusted = true;
    dispatchNow(state.element, event);
  }
  function fireAt(target, event) {
    event.isTrusted = true;
    dispatchNow(target, event);
  }

  function abortError(message) { return domError(message, 'AbortError'); }

  // HTML's "take pending play promises": the ones outstanding now, settled from
  // a task.
  function takePlayPromises(state) {
    var taken = state.pendingPlay;
    state.pendingPlay = [];
    return taken;
  }
  function rejectPlayPromises(state, error) {
    var taken = takePlayPromises(state);
    if (taken.length === 0) return;
    queueMediaTask(state, function () { taken.forEach(function (p) { p.reject(error); }); });
  }
  // `playing`, and the pending play() promises resolved. From a player event it
  // happens now, in that task; from script, in a task of its own.
  function notifyPlaying(state, now) {
    var taken = takePlayPromises(state);
    function run() {
      fireMedia(state, 'playing');
      taken.forEach(function (p) { p.resolve(); });
    }
    if (now) run();
    else queueMediaTask(state, run);
  }

  // --- the load algorithm ---------------------------------------------------------------

  function settleControllerLoad(state, error) {
    var pending = state.controllerLoad;
    state.controllerLoad = null;
    if (pending === null) return;
    if (error) pending.reject(error);
    else pending.resolve();
  }

  function releasePlayer(state) {
    var api = mediaApi();
    if (state.id !== null && api !== null) api.destroy(state.id);
    state.id = null;
    state.target = null;
    state.plane = null;
  }

  // HTML's media element load algorithm. `source` is NO_SOURCE (an unload),
  // null (select the src attribute) or what the controller asked to load.
  function mediaLoad(state, source) {
    settleControllerLoad(state, { kind: 'interrupted', message: 'another load (or an unload) started' });
    if (state.networkState === NETWORK_LOADING || state.networkState === NETWORK_IDLE) {
      queueMediaEvent(state, 'abort');
    }
    if (state.networkState !== NETWORK_EMPTY) {
      queueMediaEvent(state, 'emptied');
      state.networkState = NETWORK_EMPTY;
      state.readyState = HAVE_NOTHING;
      if (!state.paused) {
        state.paused = true;
        rejectPlayPromises(state, abortError('The play() request was interrupted by a new load request.'));
      }
      state.seeking = false;
      if (state.currentTime !== 0) {
        state.currentTime = 0;
        queueMediaEvent(state, 'timeupdate');
      }
      if (!isNaN(state.duration)) {
        state.duration = NaN;
        queueMediaEvent(state, 'durationchange');
      }
      state.live = false;
      state.seekStart = state.seekEnd = 0;
      state.buffered = [];
      state.played = [];
      state.videoWidth = state.videoHeight = 0;
      state.nativeState = 'idle';
      state.manifest = '';
      state.tracks = null;
      state.variant = -1;
      state.stats = null;
      clearNativeText(state);
    }
    state.playbackRate = state.defaultPlaybackRate;
    state.error = null;
    state.ended = false;
    state.autoplaying = true;
    state.defaultStart = NaN;

    var chosen = source === NO_SOURCE ? null : source !== null ? source : srcAttributeSource(state);
    if (chosen === null) {
      // Nothing to play: the platform player goes, so an emptied element holds
      // nothing and keeps nothing alive.
      releasePlayer(state);
      state.source = null;
      return;
    }
    state.networkState = NETWORK_LOADING;
    state.currentSrc = chosen.url;
    state.source = chosen;
    queueMediaEvent(state, 'loadstart');
    startPlayerLoad(state, chosen);
  }

  // What a `src` names, resolved against the document: the network, or a package
  // asset read from its confined file. A blob: or data: URL has nothing a
  // platform player can open.
  function mediaSource(src) {
    var url;
    try {
      url = new global.URL(String(src), String(global.location.href)).href;
    } catch (e) {
      return { url: String(src), unsupported: 'not a URL: ' + src };
    }
    if (isNetworkUrl(url)) return { url: url };
    if (/^screenkit:/i.test(url)) {
      var path = url.replace(/^screenkit:\/*/i, '/').split(/[?#]/)[0];
      return { url: url, asset: decodeURI(path) };
    }
    return { url: url, unsupported: url.split(':')[0] + ': URLs do not play in this runtime -- there is no ' +
                                     'MediaSource (use http(s) or a package asset)' };
  }
  function srcAttributeSource(state) {
    var src = getAttr(state.node, 'src');
    return src === null ? null : mediaSource(src);
  }

  function guessMimeType(url) {
    var path = String(url).split(/[?#]/)[0].toLowerCase();
    if (/\.m3u8?$/.test(path)) return 'application/x-mpegurl';
    if (/\.mpd$/.test(path)) return 'application/dash+xml';
    if (/\.(mp4|m4v|m4a|mov)$/.test(path)) return 'video/mp4';
    if (/\.webm$/.test(path)) return 'video/webm';
    return '';
  }
  var HLS_TYPES = table({ 'application/x-mpegurl': 1, 'application/vnd.apple.mpegurl': 1, 'audio/mpegurl': 1,
                          'audio/x-mpegurl': 1 });
  var DASH_TYPES = table({ 'application/dash+xml': 1 });

  function startPlayerLoad(state, source) {
    var api = mediaApi();
    var caps = mediaCaps();
    var mimeType = String(source.mimeType || guessMimeType(source.url)).toLowerCase();
    var refusal = null;
    if (state.audio) {
      announce('media-audio', 'ScreenKit: <audio> plays nothing in this runtime -- audio plays through a <video> ' +
                              'element or Web Audio is absent (runtime/js/README.md, "Video").');
      refusal = { kind: 'unavailable', message: '<audio> does not play in this runtime' };
    } else if (source.unsupported) {
      refusal = { kind: 'media', message: source.unsupported };
    } else if (api === null || !caps.available) {
      refusal = { kind: 'unavailable', message: caps.videoOutputProblem || 'this platform has no media player' };
    } else if (!caps.videoOutput) {
      refusal = { kind: 'video-output', message: caps.videoOutputProblem };
    } else if (DASH_TYPES[mimeType] === 1 && !caps.dash) {
      refusal = { kind: 'manifest', message: 'DASH does not play on this platform (' + caps.platform + '): its ' +
                                            'player has no DASH support' };
    } else if (HLS_TYPES[mimeType] === 1 && !caps.hls) {
      refusal = { kind: 'manifest', message: 'HLS does not play on this platform (' + caps.platform + ')' };
    } else if (source.drm && source.drm.keySystem && caps.keySystems.indexOf(source.drm.keySystem) < 0) {
      refusal = { kind: 'key-system', message: 'the key system ' + source.drm.keySystem + ' is not available on ' +
                                              caps.platform + ' (it has ' + (caps.keySystems.join(', ') || 'none') + ')' };
    }
    if (refusal !== null) {
      // Refused here, before a player is made -- but reported like the player's
      // own failure, from a task, after loadstart.
      releasePlayer(state);
      var serial = state.serial = -2 - Math.floor(Math.random() * 1e9);
      queueMediaTask(state, function () {
        if (state.serial === serial) mediaFailure(state, refusal);
      });
      return;
    }
    if (state.id === null) {
      state.target = { media: state };
      state.id = api.create(state.target);
    }
    var options = { url: source.url, mimeType: mimeType };
    if (source.asset) options.asset = source.asset;
    if (source.startTime !== undefined && source.startTime !== null && isFinite(source.startTime)) {
      options.startTime = Number(source.startTime);
      // Where playback will start, which is what currentTime reads until the
      // player reports a position of its own.
      state.currentTime = options.startTime;
    }
    if (source.drm) options.drm = source.drm;
    if (source.abr) options.abr = source.abr;
    if (source.audioLanguage) options.audioLanguage = String(source.audioLanguage);
    if (source.textLanguage) options.textLanguage = String(source.textLanguage);
    state.serial = api.load(state.id, options);
    state.nativeState = 'loading';
    if (state.volume !== 1) api.setVolume(state.id, state.volume);
    if (state.muted) api.setMuted(state.id, true);
    if (state.playbackRate !== 1) api.setRate(state.id, state.playbackRate);
    state.plane = null;
    updatePlane(state);
  }

  // --- what the player reports ------------------------------------------------------------

  // How many in-band cues a track keeps before the ones the media window has
  // left behind are dropped, and how far back "behind" is.
  var MAX_INBAND_CUES = 256;
  var INBAND_CUE_WINDOW = 60;
  var MEDIA_ERROR_CODES = table({ network: MEDIA_ERR_NETWORK, manifest: MEDIA_ERR_SRC_NOT_SUPPORTED,
                                  'video-output': MEDIA_ERR_SRC_NOT_SUPPORTED,
                                  'key-system': MEDIA_ERR_SRC_NOT_SUPPORTED, licence: MEDIA_ERR_DECODE,
                                  unavailable: MEDIA_ERR_SRC_NOT_SUPPORTED });

  // A failure of the load: the element's error, `error` at it, and the
  // controller's load rejected with the player's own account of it.
  function mediaFailure(state, failure) {
    var beforeMetadata = state.readyState === HAVE_NOTHING;
    var code = MEDIA_ERROR_CODES[failure.kind];
    if (code === undefined) code = beforeMetadata ? MEDIA_ERR_SRC_NOT_SUPPORTED : MEDIA_ERR_DECODE;
    state.error = makeMediaError(code, failure.message || '');
    state.networkState = beforeMetadata ? NETWORK_NO_SOURCE : NETWORK_IDLE;
    state.nativeState = 'error';
    // Nothing more arrives from this player (onMediaEvent drops events once the
    // state is 'error'), so a seek in flight would never be acknowledged and the
    // element would stay seeking for ever.
    state.seeking = false;
    fireMedia(state, 'error');
    // HTML's dedicated media source failure steps: a source that never reached
    // metadata rejects the pending play() promises. (A 404 here keeps
    // MEDIA_ERR_NETWORK rather than HTML's MEDIA_ERR_SRC_NOT_SUPPORTED, so the
    // element and Shaka's 1001 tell the same story.)
    if (beforeMetadata) {
      rejectPlayPromises(state, domError('Failed to load because no supported source was found.', 'NotSupportedError'));
    }
    settleControllerLoad(state, { kind: failure.kind, httpStatus: failure.httpStatus || 0,
                                  message: failure.message || '', mediaError: state.error });
  }

  function reachMetadata(state) {
    if (state.readyState >= HAVE_METADATA) return;
    state.readyState = HAVE_METADATA;
    fireMedia(state, 'loadedmetadata');
    if (state.videoWidth > 0) fireMedia(state, 'resize');
    settleControllerLoad(state, null);
    // A currentTime written before there was anything to seek in.
    if (!isNaN(state.defaultStart)) {
      var start = state.defaultStart;
      state.defaultStart = NaN;
      seekTo(state, start);
    }
  }

  function setDuration(state, duration) {
    var next = typeof duration === 'number' ? duration : NaN;
    if (next === state.duration || (isNaN(next) && isNaN(state.duration))) return;
    state.duration = next;
    fireMedia(state, 'durationchange');
  }

  function onMetadata(state, p) {
    state.live = !!p.live;
    state.manifest = p.manifest || '';
    state.seekStart = p.seekStart;
    state.seekEnd = p.seekEnd;
    setDuration(state, state.live ? Infinity : p.duration);
    if (p.width > 0 && p.height > 0 && (p.width !== state.videoWidth || p.height !== state.videoHeight)) {
      state.videoWidth = p.width;
      state.videoHeight = p.height;
      if (state.readyState >= HAVE_METADATA) fireMedia(state, 'resize');
      updatePlane(state);
    }
    reachMetadata(state);
  }

  function onState(state, next) {
    state.nativeState = next;
    if (next === 'loading') return;
    if (state.readyState === HAVE_NOTHING) reachMetadata(state);
    if (next === 'buffering') {
      if (state.readyState >= HAVE_FUTURE_DATA) {
        state.readyState = HAVE_CURRENT_DATA;
        // A stall while playing: timeupdate, then waiting. A seek waits for
        // data too, but its own timeupdate comes with `seeked`.
        if (!state.paused) {
          if (!state.seeking) fireMedia(state, 'timeupdate');
          fireMedia(state, 'waiting');
        }
      }
      return;
    }
    if (next === 'ready') {
      var was = state.readyState;
      state.readyState = HAVE_ENOUGH_DATA;
      if (was < HAVE_CURRENT_DATA) fireMedia(state, 'loadeddata');
      if (was < HAVE_FUTURE_DATA) {
        fireMedia(state, 'canplay');
        if (!state.paused) notifyPlaying(state, true);
      }
      if (was < HAVE_ENOUGH_DATA) {
        // Autoplay: only while nothing has played or paused it by hand.
        if (state.autoplaying && state.paused && getAttr(state.node, 'autoplay') !== null) {
          state.paused = false;
          state.autoplaying = false;
          fireMedia(state, 'play');
          notifyPlaying(state, true);
          var api = mediaApi();
          if (api !== null && state.id !== null) api.play(state.id);
        }
        fireMedia(state, 'canplaythrough');
      }
      return;
    }
    if (next === 'ended') {
      if (getAttr(state.node, 'loop') !== null && !state.live) {
        seekTo(state, 0);
        return;
      }
      state.ended = true;
      if (isFinite(state.duration)) state.currentTime = state.duration;
      fireMedia(state, 'timeupdate');
      if (!state.paused) {
        state.paused = true;
        var ended = mediaApi();
        if (ended !== null && state.id !== null) ended.pause(state.id);
        fireMedia(state, 'pause');
        rejectPlayPromises(state, abortError('The media ended.'));
      }
      fireMedia(state, 'ended');
    }
  }

  function onTime(state, p) {
    if (!state.seeking) {
      var previous = state.currentTime;
      state.currentTime = p.position;
      if (!state.paused && state.readyState >= HAVE_FUTURE_DATA) {
        var last = state.played[state.played.length - 1];
        if (last && previous >= last[0] - 0.001 && previous <= last[1] + 0.5 && p.position >= last[1]) last[1] = p.position;
        else state.played.push([Math.min(previous, p.position), p.position]);
      }
    }
    if (state.live) {
      state.seekStart = p.seekStart;
      state.seekEnd = p.seekEnd;
    }
    fireMedia(state, 'timeupdate');
    appTracksAt(state, state.currentTime);
  }

  function onSeeked(state, p) {
    if (!state.seeking) return;
    state.seeking = false;
    state.currentTime = p.position;
    fireMedia(state, 'timeupdate');
    fireMedia(state, 'seeked');
    appTracksAt(state, state.currentTime);
  }

  function onBuffered(state, p) {
    state.buffered = p.ranges || [];
    var now = Date.now();
    // `progress` about every 350 ms while data arrives, as HTML paces it.
    if (!state.lastProgress || now - state.lastProgress >= 350) {
      state.lastProgress = now;
      fireMedia(state, 'progress');
    }
  }

  function onSize(state, p) {
    if (p.width === state.videoWidth && p.height === state.videoHeight) return;
    state.videoWidth = p.width;
    state.videoHeight = p.height;
    if (state.readyState >= HAVE_METADATA) fireMedia(state, 'resize');
    updatePlane(state);
  }

  // The platform's text tracks as TextTracks on the element: added, dropped,
  // and relabelled as the player reports them, and never selected until a
  // script (or the Shaka layer) sets a mode.
  function syncNativeText(state, list) {
    var keep = [];
    (list || []).forEach(function (t) {
      var entry = null;
      for (var i = 0; i < state.nativeText.length; i++) if (state.nativeText[i].nativeId === t.id) entry = state.nativeText[i];
      if (entry === null) {
        var track = makeTextTrack(t.kind === 'captions' ? 'captions' : 'subtitles', t.label || '', t.language || '',
                                  String(t.id), 'disabled', t.id);
        textTrackStates.get(track).media = state;
        entry = { nativeId: t.id, track: track };
        addTrackToList(state, track);
      }
      keep.push(entry);
    });
    state.nativeText.forEach(function (entry) {
      if (keep.indexOf(entry) < 0) removeTrackFromList(state, entry.track);
    });
    state.nativeText = keep;
  }
  function clearNativeText(state) {
    state.nativeText.forEach(function (entry) {
      textTrackStates.get(entry.track).media = null;
      removeTrackFromList(state, entry.track);
    });
    state.nativeText = [];
    state.selectedText = -1;
  }
  function addTrackToList(state, track) {
    var tracks = trackListStates.get(state.textTracks);
    tracks.push(track);
    fill(state.textTracks, tracks);
    queueMediaTask(state, function () { fireAt(state.textTracks, new TrackEvent('addtrack', { track: track })); });
  }
  function removeTrackFromList(state, track) {
    var tracks = trackListStates.get(state.textTracks);
    var at = tracks.indexOf(track);
    if (at < 0) return;
    tracks.splice(at, 1);
    fill(state.textTracks, tracks);
    queueMediaTask(state, function () { fireAt(state.textTracks, new TrackEvent('removetrack', { track: track })); });
  }

  // A mode changed: the platform plays the first in-band track that is not
  // disabled, or no text at all.
  function textModeChanged(state) {
    queueMediaTask(state, function () { fireAt(state.textTracks, new Event('change')); });
    var chosen = -1;
    for (var i = 0; i < state.nativeText.length; i++) {
      if (trackState(state.nativeText[i].track).mode !== 'disabled') {
        chosen = state.nativeText[i].nativeId;
        break;
      }
    }
    if (chosen === state.selectedText) return;
    state.selectedText = chosen;
    var api = mediaApi();
    if (api !== null && state.id !== null) api.selectText(state.id, chosen);
  }

  function sameCues(a, b) {
    if (a.length !== b.length) return false;
    for (var i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
  }
  function setActiveCues(track, state, active) {
    if (sameCues(active, state.active)) return;
    state.active = active;
    setCueList(state.activeList, active);
    fireAt(track, new Event('cuechange'));
  }

  // Two cue times are one when within a millisecond -- or both unknown: a player
  // that only knows the cues active now (ExoPlayer) reports no end.
  function sameTime(a, b) {
    return (isNaN(a) && isNaN(b)) || Math.abs(a - b) < 0.001;
  }

  function onCues(state, p) {
    var entry = null;
    for (var i = 0; i < state.nativeText.length; i++) if (state.nativeText[i].nativeId === p.track) entry = state.nativeText[i];
    if (entry === null) return;
    var track = entry.track, ts = trackState(track);
    if (ts.mode === 'disabled') return;
    var active = (p.cues || []).map(function (c) {
      for (var j = 0; j < ts.cues.length; j++) {
        var known = ts.cues[j];
        if (known.text === c.text && sameTime(known.startTime, c.start) && sameTime(known.endTime, c.end)) {
          return known;
        }
      }
      var cue = new VTTCue(c.start, c.end, c.text);
      track.addCue(cue);
      return cue;
    });
    setActiveCues(track, ts, active);
    // In-band cues keep arriving for as long as the stream plays, and the list
    // above is scanned for every one of them, so what the media window has left
    // behind goes: a browser drops an in-band cue once it is out of the window
    // too. A cue with no end (Android reports NaN) and anything still active
    // stays.
    if (ts.cues.length > MAX_INBAND_CUES) {
      var oldest = state.currentTime - INBAND_CUE_WINDOW;
      var kept = ts.cues.filter(function (cue) {
        return !(cue.endTime < oldest) || active.indexOf(cue) >= 0;
      });
      if (kept.length !== ts.cues.length) {
        for (var k = 0; k < ts.cues.length; k++) {
          if (kept.indexOf(ts.cues[k]) < 0) track.removeCue(ts.cues[k]);
        }
      }
    }
  }

  // Tracks the app made with addTextTrack: their active cues follow the clock.
  function appTracksAt(state, time) {
    var tracks = trackListStates.get(state.textTracks);
    for (var i = 0; i < tracks.length; i++) {
      var ts = trackState(tracks[i]);
      if (ts.nativeId !== null || ts.mode === 'disabled' || ts.cues.length === 0) continue;
      setActiveCues(tracks[i], ts, ts.cues.filter(function (cue) { return cue.startTime <= time && time < cue.endTime; }));
    }
  }

  function onMediaEvent(target, type, payload) {
    var state = target && target.media;
    if (!state || state.target !== target || !payload || payload.serial !== state.serial) return;
    // A load that failed is over: nothing more of it reaches the page.
    if (state.nativeState === 'error') return;
    flushMediaTasks(state);
    switch (type) {
      case 'metadata': onMetadata(state, payload); break;
      case 'state': onState(state, payload.state); break;
      case 'time': onTime(state, payload); break;
      case 'seeked': onSeeked(state, payload); break;
      case 'buffered': onBuffered(state, payload); break;
      case 'size': onSize(state, payload); break;
      case 'tracks':
        state.tracks = payload;
        syncNativeText(state, payload.text);
        break;
      case 'variant': state.variant = payload.id; break;
      case 'cues': onCues(state, payload); break;
      case 'stats': state.stats = payload; break;
      case 'error':
        mediaFailure(state, { kind: payload.kind, httpStatus: payload.httpStatus, message: payload.message });
        break;
      case 'licence':
        // Nobody to ask a licence server: the key system's request fails.
        if (state.listeners.length === 0) {
          var api = mediaApi();
          if (api !== null && state.id !== null) api.provideLicence(state.id, payload.requestId, null);
        }
        break;
    }
    state.listeners.slice().forEach(function (listener) {
      try {
        listener(type, payload);
      } catch (err) {
        global.console.error('Uncaught in a media listener: ' + (err && err.stack || err));
      }
    });
  }
  (function () {
    var api = mediaApi();
    if (api !== null) api.onevent = onMediaEvent;
  })();

  // --- play, pause, seek ----------------------------------------------------------------

  function internalPlay(state) {
    var api = mediaApi();
    if (state.networkState === NETWORK_EMPTY) mediaLoad(state, null);
    if (state.ended) seekTo(state, state.live ? state.seekStart : 0);
    if (state.paused) {
      state.paused = false;
      state.autoplaying = false;
      queueMediaEvent(state, 'play');
      if (state.readyState <= HAVE_CURRENT_DATA) queueMediaEvent(state, 'waiting');
      else notifyPlaying(state);
    } else if (state.readyState >= HAVE_FUTURE_DATA) {
      var taken = takePlayPromises(state);
      queueMediaTask(state, function () { taken.forEach(function (p) { p.resolve(); }); });
    }
    if (api !== null && state.id !== null) api.play(state.id);
  }

  function internalPause(state) {
    var api = mediaApi();
    if (state.networkState === NETWORK_EMPTY) mediaLoad(state, null);
    state.autoplaying = false;
    if (!state.paused) {
      state.paused = true;
      queueMediaEvent(state, 'timeupdate');
      queueMediaEvent(state, 'pause');
      rejectPlayPromises(state, abortError('The play() request was interrupted by a call to pause().'));
    }
    if (api !== null && state.id !== null) api.pause(state.id);
  }

  function seekTo(state, time) {
    var target = Number(time);
    if (!isFinite(target)) return;
    // Before metadata: the default playback start position, applied once
    // there is something to seek in.
    if (state.readyState === HAVE_NOTHING) {
      state.defaultStart = target;
      return;
    }
    var start = state.live ? state.seekStart : 0;
    var end = state.live ? state.seekEnd : state.duration;
    if (isFinite(end) && target > end) target = end;
    if (target < start) target = start;
    state.seeking = true;
    state.ended = false;
    state.currentTime = target;
    queueMediaEvent(state, 'seeking');
    var api = mediaApi();
    if (api !== null && state.id !== null) api.seek(state.id, target);
  }

  // --- the plane ---------------------------------------------------------------------------
  //
  // The element's rect, from the CSS subset its style parses, in drawable pixels:
  // `left`/`top` (or `right`/`bottom`) for an absolute or fixed element, the size
  // from CSS, else the `width`/`height` attributes, else the video's own; a
  // translate or scale on top. Hidden when not in the document, `display: none`
  // or transparent. Video is always beneath the canvas, whatever its z-index.

  var LENGTH_TO_PX = table({ px: 1, pt: 4 / 3, pc: 16, 'in': 96, cm: 96 / 2.54, mm: 96 / 25.4, q: 96 / 101.6,
                             em: 16, rem: 16, ch: 8, ex: 8 });
  function lengthPx(length, axis) {
    var width = drawableWidth(), height = drawableHeight();
    switch (length.unit) {
      case '%': return length.value / 100 * (axis === 'x' ? width : height);
      case 'vw': return length.value / 100 * width;
      case 'vh': return length.value / 100 * height;
      case 'vmin': return length.value / 100 * Math.min(width, height);
      case 'vmax': return length.value / 100 * Math.max(width, height);
    }
    return length.value * (LENGTH_TO_PX[length.unit] || 1);
  }
  function layerLength(parsed, axis) {
    return parsed && parsed.length ? lengthPx(parsed.length, axis) : null;
  }
  function layerOf(node) {
    return styleStates.get(styleOf(node)).layer;
  }
  function zIndexOf(layer) {
    var z = layer['z-index'];
    return z && !z.global && z.text !== 'auto' ? parseInt(z.text, 10) : 0;
  }
  function precedes(a, b) {
    var found = null;
    walk(documentNode, function (object) {
      if (object === a || object === b) {
        found = object;
        return true;
      }
      return false;
    });
    return found === a;
  }

  function attrLength(node, name) {
    var raw = getAttr(node, name);
    if (raw === null) return 0;
    var value = Number(raw);
    return isFinite(value) && value > 0 ? Math.floor(value) : 0;
  }

  function planeFor(state) {
    var node = state.node, layer = layerOf(node);
    var display = layer.display, opacity = layer.opacity;
    var visible = state.element.isConnected && !(display && display.text === 'none') &&
                  !(opacity && !opacity.global && opacity.opacity <= 0);
    // HTML's non-negative-integer rules: a negative or unparseable width or
    // height attribute is invalid and ignored, so the default size stands. (Not
    // `>>> 0`, which turns -1 into 4294967295 and sends that as the plane.)
    var attrWidth = attrLength(node, 'width');
    var attrHeight = attrLength(node, 'height');
    var w = layerLength(layer.width, 'x'), h = layerLength(layer.height, 'y');
    if (w === null && attrWidth > 0) w = attrWidth;
    if (h === null && attrHeight > 0) h = attrHeight;
    var vw = state.videoWidth, vh = state.videoHeight;
    if (w === null && h === null) {
      w = vw || 300;
      h = vh || 150;
    } else if (w === null) {
      w = vw && vh ? h * vw / vh : 300;
    } else if (h === null) {
      h = vw && vh ? w * vh / vw : 150;
    }
    var position = layer.position && !layer.position.global ? layer.position.text : 'static';
    var x = 0, y = 0;
    var left = layerLength(layer.left, 'x'), right = layerLength(layer.right, 'x');
    var top = layerLength(layer.top, 'y'), bottom = layerLength(layer.bottom, 'y');
    if (position === 'absolute' || position === 'fixed') {
      x = left !== null ? left : right !== null ? drawableWidth() - right - w : 0;
      y = top !== null ? top : bottom !== null ? drawableHeight() - bottom - h : 0;
    } else if (position === 'relative' || position === 'sticky') {
      x = left !== null ? left : right !== null ? -right : 0;
      y = top !== null ? top : bottom !== null ? -bottom : 0;
    }
    var transform = layer.transform;
    if (transform && transform.functions) {
      transform.functions.forEach(function (f) {
        var a = f.args;
        switch (f.name) {
          case 'translate': x += lengthPx(a[0], 'x'); if (a[1]) y += lengthPx(a[1], 'y'); break;
          case 'translateX': x += lengthPx(a[0], 'x'); break;
          case 'translateY': y += lengthPx(a[0], 'y'); break;
          case 'scale': case 'scaleX': case 'scaleY': {
            var sx = f.name === 'scaleY' ? 1 : a[0], sy = f.name === 'scaleX' ? 1 : (a.length > 1 ? a[1] : a[0]);
            // Around the centre: transform-origin's default.
            x += w * (1 - sx) / 2;
            y += h * (1 - sy) / 2;
            w *= sx;
            h *= sy;
            break;
          }
        }
      });
    }
    return { x: x, y: y, w: w, h: h, visible: visible && w > 0 && h > 0, z: zIndexOf(layer) };
  }

  function updatePlane(state) {
    // The rect is computed the same way for all three; where it goes is not. An
    // <iframe>'s becomes a compositor layer, a <canvas>'s its own layer, and a
    // <video>'s a platform plane beneath every one of them.
    if (state.frame) return updateFramePlane(state);
    if (state.canvas) return updateCanvasPlane(state);
    if (state.id === null || state.audio) return;
    var api = mediaApi();
    if (api === null) return;
    var plane = planeFor(state);
    if (plane.visible && aboveACanvas(state.element, plane)) {
      announce('media-above-canvas',
        'ScreenKit: a <video> whose z-index would put it above a canvas is shown beneath it: video is ' +
        'always composited beneath the app\'s drawable, and the app clears transparent where it should show ' +
        '(give the video a lower z-index than the canvas to say what you mean). This is said once.');
    }
    var last = state.plane;
    if (last !== null && last.x === plane.x && last.y === plane.y && last.w === plane.w && last.h === plane.h &&
        last.visible === plane.visible && last.z === plane.z) return;
    state.plane = plane;
    api.setPlane(state.id, plane.x, plane.y, plane.w, plane.h, plane.visible, plane.z);
  }

  // Style, attributes and the tree change in bursts; the plane follows once,
  // from a microtask.
  var planesDirty = [];
  function planeChanged(state) {
    if (planesDirty.indexOf(state) >= 0) return;
    planesDirty.push(state);
    if (planesDirty.length === 1) {
      Promise.resolve().then(function () {
        var dirty = planesDirty;
        planesDirty = [];
        dirty.forEach(updatePlane);
      });
    }
  }
  function mediaLayerChanged(node) {
    var state = mediaStates.get(node.object);
    if (state) planeChanged(state);
  }

  // Leaving the document pauses, as HTML's removal steps do -- after a stable
  // state, so a node moved within the document keeps playing -- and hides the
  // plane; coming back shows it again.
  function mediaTreeChanged(object) {
    if (mediaElementCount === 0) return;
    var found = [];
    var own = mediaStates.get(object);
    if (own) found.push(own);
    if (nodes.get(object).type === ELEMENT_NODE || nodes.get(object).type === DOCUMENT_NODE) {
      walk(object, function (kid) {
        var state = mediaStates.get(kid);
        if (state) found.push(state);
        return false;
      });
    }
    if (found.length === 0) return;
    Promise.resolve().then(function () {
      found.forEach(function (state) {
        var connected = state.element.isConnected;
        if (connected === state.connected) return;
        state.connected = connected;
        if (!connected && !state.paused) internalPause(state);
        updatePlane(state);
      });
    });
  }

  function allMedia(visit) {
    if (mediaElementCount === 0) return;
    walk(documentNode, function (object) {
      var state = mediaStates.get(object);
      if (state) visit(state);
      return false;
    });
  }
  // Percent and viewport units are the drawable's.
  addListener(global, 'resize', function () { allMedia(planeChanged); });

  function mediaAttributeChanged(node, name) {
    var state = mediaStates.get(node.object);
    if (!state) return;
    if (name === 'src' && getAttr(node, 'src') !== null) mediaLoad(state, null);
    else if (name === 'width' || name === 'height') planeChanged(state);
  }

  // --- the controller @screenkit/shaka drives -------------------------------------------------

  function copyTracks(tracks) {
    return tracks === null ? { variants: [], audio: [], text: [] } : JSON.parse(JSON.stringify({
      variants: tracks.variants, audio: tracks.audio, text: tracks.text
    }));
  }

  function controllerFor(state) {
    if (state.controller !== null) return state.controller;
    state.controller = {
      capabilities: function () { return mediaCaps(); },
      // Runs the load algorithm on this source; resolves once `loadedmetadata`
      // has fired, rejects with {kind, httpStatus, message} if the load fails
      // and with {kind: 'interrupted'} if another load or an unload replaces it.
      load: function (options) {
        options = options || {};
        return new Promise(function (resolve, reject) {
          var source = mediaSource(options.url);
          source.mimeType = options.mimeType || '';
          source.startTime = options.startTime;
          source.drm = options.drm || null;
          source.abr = options.abr || null;
          source.audioLanguage = options.audioLanguage || '';
          source.textLanguage = options.textLanguage || '';
          mediaLoad(state, source);
          state.controllerLoad = { resolve: resolve, reject: reject };
        });
      },
      unload: function () {
        if (state.networkState === NETWORK_EMPTY && state.id === null) {
          settleControllerLoad(state, { kind: 'interrupted', message: 'unloaded' });
          return;
        }
        mediaLoad(state, NO_SOURCE);
      },
      loaded: function () { return state.networkState !== NETWORK_EMPTY && state.error === null; },
      info: function () {
        return {
          live: state.live, seekStart: state.seekStart, seekEnd: state.seekEnd, duration: state.duration,
          manifest: state.manifest, state: state.nativeState, readyState: state.readyState,
          buffering: state.nativeState === 'loading' || state.nativeState === 'buffering',
          variant: state.variant, url: state.currentSrc, textTrack: state.selectedText
        };
      },
      tracks: function () { return copyTracks(state.tracks); },
      stats: function () { return state.stats === null ? null : JSON.parse(JSON.stringify(state.stats)); },
      selectVariant: function (id) {
        var a = mediaApi();
        if (a !== null && state.id !== null) a.selectVariant(state.id, Number(id));
      },
      setAbr: function (abr) {
        var a = mediaApi();
        if (a !== null && state.id !== null) a.setAbr(state.id, abr || {});
      },
      selectAudioLanguage: function (language, role) {
        var a = mediaApi();
        if (a !== null && state.id !== null) a.selectAudioLanguage(state.id, String(language || ''), String(role || ''));
      },
      // The in-band track with the player's id `id` (-1: none) becomes the one
      // text track not disabled: 'showing' when visible, 'hidden' when not.
      selectText: function (id, visible) {
        state.nativeText.forEach(function (entry) {
          entry.track.mode = entry.nativeId === id ? (visible ? 'showing' : 'hidden') : 'disabled';
        });
      },
      provideLicence: function (requestId, bytes) {
        var a = mediaApi();
        if (a !== null && state.id !== null) a.provideLicence(state.id, requestId, bytes || null);
      },
      // `listener(type, payload)` hears every event the player reports, after
      // the element has handled it. Returns the unlisten function.
      listen: function (listener) {
        state.listeners.push(listener);
        return function () {
          var at = state.listeners.indexOf(listener);
          if (at >= 0) state.listeners.splice(at, 1);
        };
      }
    };
    return state.controller;
  }

  // --- HTMLMediaElement / HTMLVideoElement / HTMLAudioElement ------------------------------------

  function HTMLMediaElement() { throw new TypeError("Failed to construct 'HTMLMediaElement': Illegal constructor"); }
  inherit(HTMLMediaElement, HTMLElement);
  [['NETWORK_EMPTY', NETWORK_EMPTY], ['NETWORK_IDLE', NETWORK_IDLE], ['NETWORK_LOADING', NETWORK_LOADING],
   ['NETWORK_NO_SOURCE', NETWORK_NO_SOURCE], ['HAVE_NOTHING', HAVE_NOTHING], ['HAVE_METADATA', HAVE_METADATA],
   ['HAVE_CURRENT_DATA', HAVE_CURRENT_DATA], ['HAVE_FUTURE_DATA', HAVE_FUTURE_DATA],
   ['HAVE_ENOUGH_DATA', HAVE_ENOUGH_DATA]].forEach(function (constant) {
    HTMLMediaElement[constant[0]] = constant[1];
    HTMLMediaElement.prototype[constant[0]] = constant[1];
  });

  function booleanAttribute(name) {
    return {
      get: function () { mediaState(this); return getAttr(self(this), name) !== null; },
      set: function (value) {
        mediaState(this);
        if (value) setAttr(self(this), name, '');
        else removeAttr(self(this), name);
      }
    };
  }
  function reflectedUrl(name) {
    return {
      get: function () {
        var value = getAttr(self(this), name);
        if (value === null) return '';
        try {
          return new global.URL(value, String(global.location.href)).href;
        } catch (e) {
          return value;
        }
      },
      set: function (value) { setAttr(self(this), name, String(value)); }
    };
  }

  // The platforms spell some codecs differently -- Android says "vp8" and "vp9"
  // where Linux says the ISO-BMFF "vp08" and "vp09" -- so a codec is playable
  // when any spelling of it is one the platform listed, and `canPlayType` gives
  // the same answer everywhere.
  var CODEC_SPELLINGS = [['avc1', 'avc3'], ['hvc1', 'hev1'], ['vp08', 'vp8'], ['vp09', 'vp9']];
  function codecListed(codecs, codec) {
    if (codecs.indexOf(codec) >= 0) return true;
    for (var i = 0; i < CODEC_SPELLINGS.length; i++) {
      if (CODEC_SPELLINGS[i].indexOf(codec) < 0) continue;
      for (var j = 0; j < CODEC_SPELLINGS[i].length; j++) {
        if (codecs.indexOf(CODEC_SPELLINGS[i][j]) >= 0) return true;
      }
    }
    return false;
  }
  function canPlay(state, type) {
    if (state.audio) return '';
    var m = /^\s*([^;\s]+)\s*(;.*)?$/.exec(String(type));
    if (!m) return '';
    var mime = m[1].toLowerCase(), params = m[2] || '';
    var caps = mediaCaps();
    // No video output -- Linux without Wayland -- means every load is refused,
    // so nothing here is playable, whatever the codecs say.
    if (!caps.available || !caps.videoOutput) return '';
    var playable = HLS_TYPES[mime] === 1 ? caps.hls : DASH_TYPES[mime] === 1 ? caps.dash
                 : caps.containers.indexOf(mime) >= 0;
    if (!playable) return '';
    var codecs = /codecs\s*=\s*(?:"([^"]*)"|([^;\s]*))/i.exec(params);
    if (!codecs) return 'maybe';
    var list = (codecs[1] !== undefined ? codecs[1] : codecs[2]).split(',');
    for (var i = 0; i < list.length; i++) {
      var codec = list[i].trim().toLowerCase().split('.')[0];
      if (codec === '') continue;
      if (!codecListed(caps.codecs, codec)) return '';
    }
    return 'probably';
  }

  define(HTMLMediaElement.prototype, {
    error: { get: function () { return mediaState(this).error; } },
    src: reflectedUrl('src'),
    currentSrc: { get: function () { return mediaState(this).currentSrc; } },
    // A MediaStream or MediaSource: neither exists here.
    srcObject: {
      get: function () { mediaState(this); return null; },
      set: function (value) {
        mediaState(this);
        if (value === null || value === undefined) return;
        announce('media-srcObject', 'ScreenKit: <video>.srcObject was ignored -- there is no MediaSource or ' +
                                    'MediaStream in this runtime. Play a URL (@screenkit/shaka for adaptive streams).');
      }
    },
    crossOrigin: {
      get: function () {
        var value = getAttr(self(this), 'crossorigin');
        if (value === null) return null;
        return value.toLowerCase() === 'use-credentials' ? 'use-credentials' : 'anonymous';
      },
      set: function (value) {
        if (value === null) removeAttr(self(this), 'crossorigin');
        else setAttr(self(this), 'crossorigin', String(value));
      }
    },
    networkState: { get: function () { return mediaState(this).networkState; } },
    preload: {
      get: function () {
        var value = getAttr(self(this), 'preload');
        value = value === null ? 'auto' : value.toLowerCase();
        return value === 'none' || value === 'metadata' ? value : 'auto';
      },
      set: function (value) { setAttr(self(this), 'preload', String(value)); }
    },
    buffered: { get: function () { return makeTimeRanges(mediaState(this).buffered); } },
    load: function () { mediaLoad(mediaState(this), null); },
    canPlayType: function (type) {
      var state = mediaState(this);
      requireArgs(arguments.length, 1, 'canPlayType', 'HTMLMediaElement');
      return canPlay(state, type);
    },
    readyState: { get: function () { return mediaState(this).readyState; } },
    seeking: { get: function () { return mediaState(this).seeking; } },
    currentTime: {
      get: function () {
        var state = mediaState(this);
        return state.readyState === HAVE_NOTHING && !isNaN(state.defaultStart) ? state.defaultStart : state.currentTime;
      },
      set: function (value) {
        var t = Number(value);
        if (!isFinite(t)) {
          throw new TypeError("Failed to set the 'currentTime' property on 'HTMLMediaElement': The provided double " +
                              'value is non-finite.');
        }
        seekTo(mediaState(this), t);
      }
    },
    fastSeek: function (time) {
      requireArgs(arguments.length, 1, 'fastSeek', 'HTMLMediaElement');
      seekTo(mediaState(this), Number(time));
    },
    duration: { get: function () { return mediaState(this).duration; } },
    getStartDate: function () { mediaState(this); return new Date(NaN); },
    paused: { get: function () { return mediaState(this).paused; } },
    defaultPlaybackRate: {
      get: function () { return mediaState(this).defaultPlaybackRate; },
      set: function (value) {
        var state = mediaState(this), rate = Number(value);
        if (!isFinite(rate)) throw new TypeError('The provided double value is non-finite.');
        if (rate === state.defaultPlaybackRate) return;
        state.defaultPlaybackRate = rate;
        queueMediaEvent(state, 'ratechange');
      }
    },
    playbackRate: {
      get: function () { return mediaState(this).playbackRate; },
      set: function (value) {
        var state = mediaState(this), rate = Number(value);
        if (!isFinite(rate)) throw new TypeError('The provided double value is non-finite.');
        if (rate !== 0 && (rate < 0.0625 || rate > 16)) {
          throw domError("Failed to set the 'playbackRate' property on 'HTMLMediaElement': The provided playback " +
                         'rate (' + rate + ') is not in the supported playback range.', 'NotSupportedError');
        }
        if (rate === state.playbackRate) return;
        state.playbackRate = rate;
        queueMediaEvent(state, 'ratechange');
        var api = mediaApi();
        if (api !== null && state.id !== null) api.setRate(state.id, rate);
      }
    },
    preservesPitch: {
      get: function () { return mediaState(this).preservesPitch; },
      set: function (value) { mediaState(this).preservesPitch = !!value; }
    },
    played: { get: function () { return makeTimeRanges(mediaState(this).played); } },
    seekable: {
      get: function () {
        var state = mediaState(this);
        if (state.readyState === HAVE_NOTHING) return makeTimeRanges([]);
        if (state.live) return makeTimeRanges([[state.seekStart, state.seekEnd]]);
        return makeTimeRanges(isFinite(state.duration) ? [[0, state.duration]] : []);
      }
    },
    ended: { get: function () { return mediaState(this).ended; } },
    autoplay: booleanAttribute('autoplay'),
    loop: booleanAttribute('loop'),
    play: function () {
      var state = mediaState(this);
      if (state.error !== null && (state.error.code === MEDIA_ERR_SRC_NOT_SUPPORTED ||
                                   state.networkState === NETWORK_NO_SOURCE)) {
        return Promise.reject(domError('The element has no supported sources.', 'NotSupportedError'));
      }
      var promise = new Promise(function (resolve, reject) {
        state.pendingPlay.push({ resolve: resolve, reject: reject });
      });
      internalPlay(state);
      return promise;
    },
    pause: function () { internalPause(mediaState(this)); },
    controls: booleanAttribute('controls'),
    volume: {
      get: function () { return mediaState(this).volume; },
      set: function (value) {
        var state = mediaState(this), volume = Number(value);
        if (!isFinite(volume)) throw new TypeError('The provided double value is non-finite.');
        if (volume < 0 || volume > 1) {
          throw domError("Failed to set the 'volume' property on 'HTMLMediaElement': The volume provided (" + volume +
                         ') is outside the range [0, 1].', 'IndexSizeError');
        }
        if (volume === state.volume) return;
        state.volume = volume;
        queueMediaEvent(state, 'volumechange');
        var api = mediaApi();
        if (api !== null && state.id !== null) api.setVolume(state.id, volume);
      }
    },
    muted: {
      get: function () { return mediaState(this).muted; },
      set: function (value) {
        var state = mediaState(this), muted = !!value;
        if (muted === state.muted) return;
        state.muted = muted;
        queueMediaEvent(state, 'volumechange');
        var api = mediaApi();
        if (api !== null && state.id !== null) api.setMuted(state.id, muted);
      }
    },
    defaultMuted: booleanAttribute('muted'),
    textTracks: { get: function () { return mediaState(this).textTracks; } },
    addTextTrack: function (kind, label, language) {
      var state = mediaState(this);
      requireArgs(arguments.length, 1, 'addTextTrack', 'HTMLMediaElement');
      var k = String(kind);
      if (['subtitles', 'captions', 'descriptions', 'chapters', 'metadata'].indexOf(k) < 0) {
        throw new TypeError("Failed to execute 'addTextTrack' on 'HTMLMediaElement': The provided value '" + k +
                            "' is not a valid enum value of type TextTrackKind.");
      }
      var track = makeTextTrack(k, label === undefined ? '' : String(label), language === undefined ? '' : String(language),
                                '', 'hidden', null);
      addTrackToList(state, track);
      return track;
    }
  });
  Object.defineProperty(HTMLMediaElement.prototype, MEDIA_CONTROLLER, {
    get: function () { return controllerFor(mediaState(this)); },
    enumerable: false,
    configurable: true
  });
  global.HTMLMediaElement = HTMLMediaElement;

  function HTMLVideoElement() { throw new TypeError("Failed to construct 'HTMLVideoElement': Illegal constructor"); }
  inherit(HTMLVideoElement, HTMLMediaElement);
  function dimensionAttribute(name) {
    return {
      get: function () {
        var value = getAttr(self(this), name);
        var n = value === null ? 0 : parseInt(value, 10);
        return isFinite(n) && n >= 0 && n <= 2147483647 ? n : 0;
      },
      set: function (value) { setAttr(self(this), name, String(Number(value) >>> 0)); }
    };
  }
  define(HTMLVideoElement.prototype, {
    width: dimensionAttribute('width'),
    height: dimensionAttribute('height'),
    videoWidth: { get: function () { return mediaState(this).videoWidth; } },
    videoHeight: { get: function () { return mediaState(this).videoHeight; } },
    poster: reflectedUrl('poster'),
    playsInline: booleanAttribute('playsinline'),
    disablePictureInPicture: booleanAttribute('disablepictureinpicture'),
    getVideoPlaybackQuality: function () {
      var stats = mediaState(this).stats || {};
      var quality = Object.create(VideoPlaybackQuality.prototype);
      quality.creationTime = global.performance.now();
      quality.totalVideoFrames = (stats.decodedFrames || 0);
      quality.droppedVideoFrames = stats.droppedFrames || 0;
      quality.corruptedVideoFrames = stats.corruptedFrames || 0;
      return quality;
    }
  });
  global.HTMLVideoElement = HTMLVideoElement;

  function HTMLAudioElement() { throw new TypeError("Failed to construct 'HTMLAudioElement': Illegal constructor"); }
  inherit(HTMLAudioElement, HTMLMediaElement);
  global.HTMLAudioElement = HTMLAudioElement;

  function HTMLImageElement() {
    throw new TypeError('Illegal constructor: use new Image() or document.createElement("img")');
  }
  inherit(HTMLImageElement, HTMLElement);
  // `new Image()` and `document.createElement('img')` produce the same thing.
  global.HTMLImageElement = HTMLImageElement;
  global.Image = function Image(w, h) {
    var img = makeImage();
    if (w !== undefined) img.width = w;
    if (h !== undefined) img.height = h;
    return img;
  };
  global.Image.prototype = HTMLImageElement.prototype;

  // -------------------------------------------------------------------------
  // <iframe>: instances, postMessage and the window tree
  //
  // One iframe = one Instance = a second app on its own Hermes runtime, its own
  // thread and its own GL context, drawing into a texture this page composites
  // at the element's CSS rect (Architecture.md 5). The element, `contentWindow`,
  // `sandbox` and the message plumbing are here; making the runtime, gating its
  // package and compositing its frame are `__screenkit.instances`'s.
  //
  // **The iframe boundary is not a security boundary** (Architecture.md 5.2).
  // JS is isolated -- two runtimes share no object, and `postMessage` is the
  // only channel -- but memory is not: a native crash or an OOM in any instance
  // takes every instance with it, including this one. `sandbox` gates
  // capabilities, not memory and not CPU.
  //
  // Two divergences from the web, both deliberate and both recorded in
  // runtime/js/README.md: instances nest one level deep, so an `<iframe>` inside
  // an instance fires `error` rather than embedding a third app; and `src` names
  // a package inside this app, never a URL to download.
  // -------------------------------------------------------------------------

  function instancesApi() {
    var io = global.__screenkit;
    return io && io.instances && typeof io.instances.capabilities === 'function' ? io.instances : null;
  }

  var instanceCapsCache = null;
  function instanceCapabilities() {
    if (instanceCapsCache === null) {
      var api = instancesApi();
      // Without the binding -- a bundle run with no host behind it -- nothing
      // can be embedded and nothing embeds this, which is the truth rather than
      // a stub that would make a feature check lie.
      instanceCapsCache = api === null
        ? { canEmbed: false, hasParent: false, sandboxed: false, allowNetwork: true, allowMedia: true,
            allowStorage: true, allowBackgroundAudio: true, allowBackgroundTimers: true }
        : api.capabilities();
    }
    return instanceCapsCache;
  }

  // A capability this page is sandboxed out of, said once, where the page first
  // trips over its absence. The shape it gets is the platform's own "there is
  // none of this here" -- `__screenkit.net` missing, `mediaCaps().available`
  // false -- so a feature check finds the same answer it would on a device
  // without the thing.
  function sandboxRefusal(token, what) {
    if (!instanceCapabilities().sandboxed) return;
    announce('sandbox:' + token,
      'ScreenKit: this app runs in a sandboxed <iframe> without "' + token + '", so ' + what +
      ' is unavailable. `sandbox` gates capabilities only -- it is not a memory or a CPU boundary ' +
      '(Architecture.md 5.2). This is said once.');
  }

  // --- structuredClone ------------------------------------------------------
  // The subset `postMessage` carries: primitives, plain objects and arrays, and
  // ArrayBuffer by value. Anything else throws DataCloneError rather than
  // arriving as something the receiver cannot tell apart from the real thing.
  // Cycles are preserved, as the structured clone algorithm preserves them.
  //
  // There is no transfer and no MessagePort: the two runtimes share no memory,
  // so nothing can be moved -- only copied.

  function cloneReject(value, type) {
    var name = type || (value && value.constructor && value.constructor.name);
    return domError((name ? 'A ' + name : 'That value') +
                    ' could not be cloned: postMessage carries primitives, plain objects and arrays, ' +
                    'and ArrayBuffer.', 'DataCloneError');
  }

  // How deep a message may nest. Cycles are memoised, so this is only ever
  // reached by a genuinely deep structure, and the clone recurses -- so without
  // a cap that is the engine's own stack overflow (a `RangeError` from
  // somewhere unrelated-looking) rather than an error the page can catch. Far
  // deeper than any real message, and low enough to be reached before the stack
  // is -- including where a frame is several times its usual size, which is
  // where 128 was not (instance/StructuredClone.cpp). The native encoder
  // `postMessage` uses across runtimes caps at the same depth, so both
  // directions refuse the same values.
  var MAX_CLONE_DEPTH = 64;

  function cloneSubset(value, seen, made, depth) {
    if (depth > MAX_CLONE_DEPTH) {
      throw domError('A value nested more than ' + MAX_CLONE_DEPTH + ' deep could not be cloned.',
                     'DataCloneError');
    }
    if (value === null) return null;
    var type = typeof value;
    if (type === 'undefined' || type === 'boolean' || type === 'number' || type === 'string') return value;
    if (type === 'function') throw cloneReject(value, 'function');
    if (type === 'symbol') throw cloneReject(value, 'symbol');
    if (type === 'bigint') throw cloneReject(value, 'BigInt');
    var at = seen.indexOf(value);
    if (at >= 0) return made[at];
    if (value instanceof global.ArrayBuffer) return value.slice(0);
    if (Array.isArray(value)) {
      var array = [];
      seen.push(value);
      made.push(array);
      for (var i = 0; i < value.length; i++) array[i] = cloneSubset(value[i], seen, made, depth + 1);
      return array;
    }
    var prototype = Object.getPrototypeOf(value);
    if (prototype !== Object.prototype && prototype !== null) throw cloneReject(value);
    var copy = {};
    seen.push(value);
    made.push(copy);
    Object.keys(value).forEach(function (key) { copy[key] = cloneSubset(value[key], seen, made, depth + 1); });
    return copy;
  }

  global.structuredClone = function (value, options) {
    if (arguments.length < 1) {
      throw new TypeError("Failed to execute 'structuredClone' on 'Window': 1 argument required, but only 0 present.");
    }
    if (options && options.transfer && options.transfer.length > 0) {
      throw domError('Transfer is not supported in this runtime: postMessage copies, and there are no ' +
                     'MessagePorts to transfer.', 'DataCloneError');
    }
    return cloneSubset(value, [], [], 0);
  };

  // --- the window tree ------------------------------------------------------
  // `contentWindow`, `parent` and `top` are WindowProxies, never another
  // runtime's global: the other side is a separate heap, and the only thing that
  // crosses between them is a copied message.

  function windowProxy(post, focus, up) {
    var proxy = {};
    proxy.postMessage = function (message) {
      if (arguments.length < 1) {
        throw new TypeError("Failed to execute 'postMessage' on 'Window': 1 argument required, but only 0 present.");
      }
      post(message);
    };
    proxy.focus = function () { focus(); };
    proxy.blur = function () {};
    proxy.closed = false;
    proxy.length = 0;
    define(proxy, {
      self: { get: function () { return proxy; } },
      window: { get: function () { return proxy; } },
      parent: { get: function () { return up || proxy; } },
      top: { get: function () { return up || proxy; } }
    });
    return proxy;
  }

  // A `message` event at this window, from a task. Never synchronous, whichever
  // side sent it: a browser queues one, and so does the freeze gate.
  function deliverWindowMessage(data, source) {
    var event = new MessageEvent('message', { data: data, source: source || null, origin: '' });
    event.isTrusted = true;
    dispatchNow(global, event);
  }

  var parentProxy = null;
  function parentWindow() {
    var caps = instanceCapabilities();
    // A top-level page is its own parent and its own top, exactly as in a
    // browser -- which is also how a page detects that it is not embedded.
    if (!caps.hasParent) return global;
    if (parentProxy === null) {
      parentProxy = windowProxy(
        function (message) {
          var api = instancesApi();
          if (api === null || typeof api.parentPost !== 'function') return;
          try {
            api.parentPost(message);
          } catch (e) {
            throw domError(e && e.message ? e.message : 'That value could not be cloned.',
                           (e && e.name) || 'DataCloneError');
          }
        },
        function () {
          var api = instancesApi();
          if (api !== null && typeof api.parentFocus === 'function') api.parentFocus();
        },
        undefined);
    }
    return parentProxy;
  }

  // Posting to this window itself, as a browser does when a script posts to its
  // own window: a copy, delivered as a task.
  global.postMessage = function (message) {
    if (arguments.length < 1) {
      throw new TypeError("Failed to execute 'postMessage' on 'Window': 1 argument required, but only 0 present.");
    }
    var copy = cloneSubset(message, [], [], 0);
    later(function () { deliverWindowMessage(copy, global); });
  };

  // `window.focus()` takes the remote back from whatever instance had it, which
  // is the other half of `iframe.focus()`. In an instance it does nothing: a
  // child cannot take focus from its parent without asking, and asking is
  // `parent.focus()`.
  global.focus = function () {
    var api = instancesApi();
    if (api !== null && typeof api.focus === 'function') api.focus(0);
    focusedElement = null;
  };
  global.blur = function () {};

  define(global, {
    parent: { get: parentWindow },
    top: { get: parentWindow },
    // In a browser `frames` is the window itself and `window[0]` is the first
    // child. Here it is a live-read array-like of the contentWindows, which is
    // what `frames.length` and `frames[0]` -- the whole of what pages use -- are
    // about, without adding numeric properties to the global object.
    //
    // Both count the `<iframe>`s *in the document*, in document order, as a
    // browser counts child browsing contexts: an element that was made and never
    // inserted is not one of them.
    frames: {
      get: function () {
        var list = {}, n = 0;
        allFrames(function (state) { list[n++] = frameContentWindow(state); });
        list.length = n;
        return list;
      }
    },
    length: {
      get: function () {
        var n = 0;
        allFrames(function () { n++; });
        return n;
      }
    }
  });

  // --- the lifecycle --------------------------------------------------------
  // `pause` and `resume` at `window`, from the host: the freeze gate is closed
  // immediately after the `pause` dispatch and opened just before `resume`, so a
  // page hears about a pause while it can still run (Architecture.md 5.1).
  // `suspend` and `memorywarning` belong to the deferred fourth state and are
  // deliberately absent.
  global.__screenkitLifecycle = function (type) {
    var event = new Event(type);
    event.isTrusted = true;
    dispatchNow(global, event);
  };

  // --- HTMLIFrameElement ----------------------------------------------------

  var frameStates = new WeakMap();
  // How many <iframe>s were ever made: tree, style and attribute changes only
  // look for one once there is one.
  var frameElementCount = 0;
  // The element holding focus, or null for the body -- which is what
  // `document.activeElement` answers with.
  var focusedElement = null;

  function frameState(element) {
    var state = frameStates.get(element);
    if (!state) throw new TypeError('Illegal invocation');
    return state;
  }

  function makeFrame() {
    var element = newElement('iframe', HTMLIFrameElement.prototype);
    var node = nodes.get(element);
    var state = {
      element: element, node: node, frame: true,
      id: null, serial: 0, plane: null, connected: false,
      // `planeFor` reads these for a <video>'s intrinsic size. An <iframe> has
      // none, so the CSS default of 300x150 stands, as it does in a browser.
      videoWidth: 0, videoHeight: 0,
      loaded: false, failed: false, loadPending: false, contentWindow: null, sandboxList: null
    };
    frameStates.set(element, state);
    // Backed: its style parses the CSS subset, and the rect is where the
    // instance's frame is composited.
    node.backed = true;
    frameElementCount++;
    return element;
  }

  function frameFire(state, type) {
    var event = new Event(type);
    event.isTrusted = true;
    dispatchNow(state.element, event);
  }

  function frameFail(state, message) {
    state.failed = true;
    state.loaded = false;
    global.console.error('ScreenKit: <iframe> ' + message);
    later(function () { frameFire(state, 'error'); });
  }

  // What an `src` names, resolved against the document: a path inside this app's
  // own package. A network URL is passed through unchanged so the native side
  // refuses it with the one message that says what is allowed.
  function frameSource(src) {
    var url;
    try {
      url = new global.URL(String(src), String(global.location.href)).href;
    } catch (e) {
      return String(src);
    }
    if (isNetworkUrl(url)) return url;
    var path = url.replace(/^screenkit:\/*/i, '').split(/[?#]/)[0];
    // Confinement resolves a relative path against the asset root and an
    // absolute one against the filesystem, so the leading slash goes, exactly as
    // it does for every other package asset (`resolveResource`). A malformed
    // escape is handed over as written for the native side to refuse by name --
    // `decodeURI` would throw, and this runs inside the load's microtask.
    var trimmed = path.replace(/^\/+/, '');
    try {
      return decodeURI(trimmed);
    } catch (e) {
      return trimmed;
    }
  }

  function frameSandboxTokens(state) {
    var value = getAttr(state.node, 'sandbox');
    return value === null ? null : value;
  }

  // Appending the element and setting its `src` both ask for a load, and a page
  // does them in either order in the same turn. Coalescing into a microtask, as
  // the plane is coalesced, makes that one load rather than a load that is torn
  // down and made again -- and keeps a later `src` write a real reload.
  function frameLoad(state) {
    if (state.loadPending) return;
    state.loadPending = true;
    Promise.resolve().then(function () {
      state.loadPending = false;
      if (!state.element.isConnected) return;
      try {
        frameLoadNow(state);
      } catch (e) {
        // Nothing is watching this microtask, so a throw here -- a src the
        // binding refuses, a runtime shutting down -- would otherwise be an
        // unhandled rejection and an element that stays blank without ever
        // saying why.
        frameFail(state, (e && e.message) || String(e));
      }
    });
  }

  function frameLoadNow(state) {
    var src = getAttr(state.node, 'src');
    if (src === null || src === '') return;
    var api = instancesApi();
    var caps = instanceCapabilities();
    if (api === null || !caps.canEmbed) {
      // One level of nesting: `Architecture.md` 5 allows a tree and this build
      // does not, which is a recorded divergence rather than a silent inert
      // element.
      frameFail(state, 'src="' + src + '" is refused: ' + (caps.hasParent
        ? 'instances nest one level deep, so an <iframe> inside an <iframe> embeds nothing'
        : 'this runtime has no compositor to embed one in (a windowed host has)'));
      return;
    }
    if (state.id === null) state.id = api.create(state.element);
    var plane = planeFor(state);
    var options = { src: frameSource(src), width: Math.round(plane.w), height: Math.round(plane.h) };
    var sandbox = frameSandboxTokens(state);
    if (sandbox !== null) options.sandbox = sandbox;
    state.loaded = false;
    state.failed = false;
    state.plane = null;
    state.serial = api.load(state.id, options);
    updateFramePlane(state);
  }

  function frameUnload(state) {
    state.loadPending = false;
    var api = instancesApi();
    if (state.id === null || api === null || typeof api.unload !== 'function') return;
    api.unload(state.id);
    state.loaded = false;
    state.failed = false;
    state.plane = null;
    if (focusedElement === state.element) focusedElement = null;
  }

  function updateFramePlane(state) {
    var api = instancesApi();
    if (state.id === null || api === null || typeof api.setPlane !== 'function') return;
    var plane = planeFor(state);
    var layer = layerOf(state.node);
    var opacity = layer.opacity && !layer.opacity.global ? layer.opacity.opacity : 1;
    var last = state.plane;
    if (last !== null && last.x === plane.x && last.y === plane.y && last.w === plane.w &&
        last.h === plane.h && last.visible === plane.visible && last.z === plane.z &&
        last.opacity === opacity) {
      return;
    }
    plane.opacity = opacity;
    state.plane = plane;
    api.setPlane(state.id, plane.x, plane.y, plane.w, plane.h, plane.visible, plane.z, opacity);
  }

  function frameLayerChanged(node) {
    var state = frameStates.get(node.object);
    if (state) planeChanged(state);
  }

  // Leaving the document terminates the instance, as HTML terminates a removed
  // iframe's browsing context; coming back loads it again, fresh. Deferred to a
  // microtask, as the media path is, so a node moved within the document is not
  // torn down and rebuilt.
  function frameTreeChanged(object) {
    if (frameElementCount === 0) return;
    var found = [];
    var own = frameStates.get(object);
    if (own) found.push(own);
    var node = nodes.get(object);
    if (node.type === ELEMENT_NODE || node.type === DOCUMENT_NODE) {
      walk(object, function (kid) {
        var state = frameStates.get(kid);
        if (state) found.push(state);
        return false;
      });
    }
    if (found.length === 0) return;
    Promise.resolve().then(function () {
      found.forEach(function (state) {
        var connected = state.element.isConnected;
        if (connected === state.connected) return;
        state.connected = connected;
        if (connected) frameLoad(state);
        else frameUnload(state);
      });
    });
  }

  function frameAttributeChanged(node, name) {
    var state = frameStates.get(node.object);
    if (!state) return;
    if (name === 'src') {
      var src = getAttr(node, 'src');
      if (src === null || src === '') {
        // HTML processes the attribute on every set, change and removal, and an
        // absent or empty `src` navigates to about:blank -- the old document
        // goes. Here that is the instance: leaving it running would keep a
        // thread and a layer for a frame the page has emptied.
        frameUnload(state);
      } else if (state.element.isConnected) {
        frameLoad(state);
      }
    } else if (name === 'sandbox') {
      if (state.sandboxList !== null) fill(state.sandboxList, classTokens(node, 'sandbox'));
      // A sandbox change applies to the next load, as it does in a browser.
    } else {
      planeChanged(state);
    }
  }

  function allFrames(visit) {
    if (frameElementCount === 0) return;
    walk(documentNode, function (object) {
      var state = frameStates.get(object);
      if (state) visit(state);
      return false;
    });
  }
  // Percent and viewport units are the drawable's.
  addListener(global, 'resize', function () { allFrames(planeChanged); });

  function frameContentWindow(state) {
    if (state.contentWindow === null) {
      state.contentWindow = windowProxy(
        function (message) {
          var api = instancesApi();
          if (api === null || state.id === null || typeof api.post !== 'function') return;
          try {
            api.post(state.id, message);
          } catch (e) {
            throw domError(e && e.message ? e.message : 'That value could not be cloned.',
                           (e && e.name) || 'DataCloneError');
          }
        },
        function () { state.element.focus(); },
        global);
    }
    return state.contentWindow;
  }

  function HTMLIFrameElement() {
    throw new TypeError("Failed to construct 'HTMLIFrameElement': Illegal constructor");
  }
  inherit(HTMLIFrameElement, HTMLElement);
  define(HTMLIFrameElement.prototype, {
    src: reflectedUrl('src'),
    width: dimensionAttribute('width'),
    height: dimensionAttribute('height'),
    sandbox: {
      get: function () {
        var state = frameState(this);
        if (state.sandboxList === null) {
          state.sandboxList = collection(DOMTokenList.prototype, classTokens(state.node, 'sandbox'));
          tokenOwners.set(state.sandboxList, { node: state.node, name: 'sandbox' });
        }
        return state.sandboxList;
      },
      set: function (value) { setAttr(frameState(this).node, 'sandbox', String(value)); }
    },
    contentWindow: { get: function () { return frameContentWindow(frameState(this)); } },
    // There is no second document to reach into: the instance is a separate
    // runtime with a document of its own, and `postMessage` is the only channel.
    contentDocument: { get: function () { frameState(this); return null; } },
    focus: function () {
      var state = frameState(this);
      var api = instancesApi();
      if (state.id === null || api === null || typeof api.focus !== 'function') return;
      api.focus(state.id);
      focusedElement = state.element;
    },
    blur: function () {
      frameState(this);
      global.focus();
    }
  });
  defineEventHandlers(HTMLIFrameElement.prototype, ['load', 'error']);
  global.HTMLIFrameElement = HTMLIFrameElement;

  // Focus and blur exist on every element, and move nothing anywhere else:
  // an <iframe> is the one thing here that can hold focus, because it is the one
  // thing with a browsing context behind it.
  define(HTMLElement.prototype, {
    focus: function () { self(this); },
    blur: function () { self(this); }
  });

  (function wireInstances() {
    var api = instancesApi();
    if (api === null) return;
    api.onevent = function (target, type, payload) {
      var state = frameStates.get(target);
      if (!state) return;
      if (type === 'load') {
        state.loaded = true;
        state.failed = false;
        frameFire(state, 'load');
        return;
      }
      if (type === 'error') {
        state.failed = true;
        state.loaded = false;
        if (payload && payload.message) {
          global.console.error('ScreenKit: <iframe src="' + (getAttr(state.node, 'src') || '') +
                               '"> failed: ' + payload.message);
        }
        frameFire(state, 'error');
        return;
      }
      if (type === 'message') {
        // From the instance to this page: `source` is the iframe's
        // contentWindow, so a listener can reply to exactly who sent it.
        deliverWindowMessage(payload ? payload.data : undefined, frameContentWindow(state));
        return;
      }
      if (type === 'focus' || type === 'blur') {
        // A switch this page did not make itself -- `window.parent.focus()` in
        // the instance -- still has to move `document.activeElement`.
        if (type === 'focus') focusedElement = state.element;
        else if (focusedElement === state.element) focusedElement = null;
        frameFire(state, type);
      }
    };
    api.onparentmessage = function (data) {
      deliverWindowMessage(data, parentWindow());
    };
  })();

  // -------------------------------------------------------------------------
  // Fonts: FontFace and document.fonts
  //
  // Evidence: `new FontFace` and `document.fonts` referenced; Pixi, Phaser and
  // Lightning load web fonts through them. A face loads its first source that
  // parses -- url() through fetch, local() from the installed fonts, or bytes
  // given directly -- into __screenkit.text, and once it is in document.fonts the
  // 2D context draws with it.
  // -------------------------------------------------------------------------

  var fontFaceStates = new WeakMap();

  function FontFace(family, source, descriptors) {
    this.family = String(family);
    this.source = source;
    this.style = (descriptors && descriptors.style) || 'normal';
    this.weight = (descriptors && descriptors.weight) || 'normal';
    this.display = (descriptors && descriptors.display) || 'auto';
    this.status = 'unloaded';
    var face = this, state = { face: -1, resolve: null, reject: null };
    fontFaceStates.set(this, state);
    this.loaded = new Promise(function (resolve, reject) { state.resolve = resolve; state.reject = reject; });
    // A failure is reported through status and load(); `loaded` alone is not an unhandled rejection.
    this.loaded.catch(function () {});
    if (source !== null && typeof source === 'object') this.load();
  }
  FontFace.prototype.load = function () {
    var face = this, state = fontFaceStates.get(this);
    if (!state) throw new TypeError('Illegal invocation');
    if (this.status !== 'unloaded') return this.loaded;
    this.status = 'loading';
    loadFontSource(this.source).then(function (id) {
      state.face = id;
      face.status = 'loaded';
      fontFacesChanged++;
      state.resolve(face);
    }, function (error) {
      face.status = 'error';
      state.reject(error);
    });
    return this.loaded;
  };
  global.FontFace = FontFace;

  // A FontFace source -> a promise of the face id in __screenkit.text.
  function loadFontSource(source) {
    var api = textApi();
    var failed = function () { return domError('A network error occurred.', 'NetworkError'); };
    if (api === null) return Promise.reject(failed());
    if (source !== null && typeof source === 'object') {
      try {
        return Promise.resolve(api.addFont(source).face);
      } catch (error) {
        return Promise.reject(domError('Failed to load font: ' + error.message, 'SyntaxError'));
      }
    }
    var entries = [], pattern = /(url|local)\(\s*(["']?)(.*?)\2\s*\)/g, match;
    while ((match = pattern.exec(String(source))) !== null) entries.push({ kind: match[1], value: match[3] });
    if (entries.length === 0) {
      return Promise.reject(domError("Failed to load font: '" + source + "' is not a valid source list.", 'SyntaxError'));
    }
    return entries.reduce(function (previous, entry) {
      return previous.catch(function () {
        if (entry.kind === 'local') {
          var added = api.addSystemFont(entry.value);
          if (added === null) throw failed();
          return added.face;
        }
        return global.fetch(entry.value).then(function (response) {
          if (!response.ok) throw failed();
          return response.arrayBuffer();
        }).then(function (bytes) {
          return api.addFont(bytes).face;
        });
      });
    }, Promise.reject(failed())).catch(function (error) {
      throw error && error.name === 'NetworkError' ? error : failed();
    });
  }

  function FontFaceSet() { this._faces = []; this.status = 'loaded'; this.ready = Promise.resolve(this); }
  FontFaceSet.prototype.add = function (face) {
    if (this._faces.indexOf(face) < 0) this._faces.push(face);
    fontFacesChanged++;
    return this;
  };
  FontFaceSet.prototype['delete'] = function (face) {
    var i = this._faces.indexOf(face);
    if (i < 0) return false;
    this._faces.splice(i, 1);
    fontFacesChanged++;
    return true;
  };
  FontFaceSet.prototype.has = function (face) { return this._faces.indexOf(face) >= 0; };
  FontFaceSet.prototype.clear = function () { this._faces = []; fontFacesChanged++; };
  FontFaceSet.prototype.check = function () { return true; };
  FontFaceSet.prototype.load = function () {
    return Promise.all(this._faces.map(function (f) { return f.load(); }));
  };
  FontFaceSet.prototype.forEach = function (fn, thisArg) {
    for (var i = 0; i < this._faces.length; i++) fn.call(thisArg, this._faces[i], this._faces[i], this);
  };
  Object.defineProperty(FontFaceSet.prototype, 'size', { get: function () { return this._faces.length; } });

  // -------------------------------------------------------------------------
  // Path2D
  //
  // Evidence: `new Path2D` x6, `arcTo` x4, `moveTo`, `closePath`, `rect`. It is
  // only consumed by a 2D context, which this runtime deliberately does not
  // provide (see README). Recording the commands keeps construction working and
  // leaves the path data intact for a future 2D backend.
  // -------------------------------------------------------------------------

  function Path2D(path) {
    this._ops = path && path._ops ? path._ops.slice() : [];
  }
  ['moveTo', 'lineTo', 'arc', 'arcTo', 'rect', 'roundRect', 'closePath', 'ellipse',
   'bezierCurveTo', 'quadraticCurveTo'].forEach(function (name) {
    Path2D.prototype[name] = function () {
      this._ops.push([name, Array.prototype.slice.call(arguments)]);
    };
  });
  Path2D.prototype.addPath = function (other) {
    if (other && other._ops) this._ops = this._ops.concat(other._ops);
  };
  global.Path2D = Path2D;

  // -------------------------------------------------------------------------
  // document
  //
  // Evidence: Document `createElement("link","canvas","div")`,
  // `getElementById("app")`, `documentElement`, `head`,
  // `querySelector("meta[property=csp-nonce]")`, `querySelectorAll`,
  // `getElementsByTagName("link")`, `fonts` (m2-lightning-dom-usage.json).
  //
  // The document starts as the markup every index.html has: <html> with a
  // <head> and a <body>. index.html itself is a manifest of entry points and is
  // never parsed (Architecture.md 6.1), so nothing else from it is here.
  // -------------------------------------------------------------------------

  // The mount point a web build's index.html declares. Markup is never parsed,
  // so when no element in the tree has this id, the lookup makes one -- a
  // <div id="app"> appended to <body> -- rather than returning null to a
  // bundle that was written against that markup. Any other id is a plain
  // search of the tree.
  var ROOT_ID = 'app';

  function childNamed(parent, localName) {
    if (parent === null) return null;
    var kids = nodes.get(parent).kids;
    for (var i = 0; i < kids.length; i++) {
      if (nodes.get(kids[i]).localName === localName) return kids[i];
    }
    return null;
  }
  function documentElementOf(doc) {
    var kids = elementKids(nodes.get(doc || documentNode));
    return kids.length ? kids[0] : null;
  }

  define(Document.prototype, {
    nodeName: { get: function () { self(this); return '#document'; } },
    documentElement: { get: function () { self(this); return documentElementOf(this); } },
    head: { get: function () { self(this); return this === documentNode ? childNamed(documentElementOf(), 'head') : null; } },
    body: { get: function () { self(this); return this === documentNode ? childNamed(documentElementOf(), 'body') : null; } },
    // Focus moves only to an <iframe> -- a second browsing context is the one
    // thing here that can hold it -- and is the body the rest of the time.
    activeElement: {
      get: function () {
        self(this);
        if (this === documentNode && focusedElement !== null && focusedElement.isConnected) {
          return focusedElement;
        }
        return this.body || documentElementOf();
      }
    },
    defaultView: { get: function () { self(this); return global; } },
    hasFocus: function () { self(this); return true; },
    createTextNode: function (data) {
      self(this);
      requireArgs(arguments.length, 1, 'createTextNode', 'Document');
      return makeText(String(data), this === documentNode ? null : this);
    },
    createElement: function (tagName) {
      self(this);
      requireArgs(arguments.length, 1, 'createElement', 'Document');
      var tag = elementName(tagName);
      if (tag === 'canvas') return makeCanvas();
      if (tag === 'img' || tag === 'image') return makeImage();
      if (tag === 'video') return makeMedia(tag, HTMLVideoElement.prototype);
      if (tag === 'audio') return makeMedia(tag, HTMLAudioElement.prototype);
      if (tag === 'iframe') return makeFrame();
      return newElement(tag);
    },
    // three.js makes its canvas this way. The HTML namespace gives the same
    // elements createElement does -- a canvas that is backed, an <img> that
    // loads -- with the name as given; any other namespace (SVG, MathML) gives a
    // plain Element, which lives in the tree and never renders.
    createElementNS: function (namespace, qualifiedName) {
      self(this);
      requireArgs(arguments.length, 2, 'createElementNS', 'Document');
      var ns = namespace === null || namespace === undefined || String(namespace) === '' ? null : String(namespace);
      var name = String(qualifiedName);
      elementName(name);
      var local = name.indexOf(':') >= 0 ? name.slice(name.indexOf(':') + 1) : name;
      if (ns === HTML_NAMESPACE) {
        if (local === 'canvas') return makeCanvas();
        if (local === 'img') return makeImage();
        return this.createElement(local);
      }
      var element = newElement(local, Element.prototype);
      nodes.get(element).namespace = ns;
      if (name.indexOf(':') >= 0) nodes.get(element).prefix = name.slice(0, name.indexOf(':'));
      return element;
    },
    getElementById: function (id) {
      self(this);
      requireArgs(arguments.length, 1, 'getElementById', 'Document');
      var wanted = String(id), found = null;
      if (wanted !== '') {
        walk(this, function (object, node) {
          if (getAttr(node, 'id') !== wanted) return false;
          found = object;
          return true;
        });
      }
      if (found === null && wanted === ROOT_ID) {
        found = newElement('div');
        setAttr(nodes.get(found), 'id', ROOT_ID);
        var mount = this.body || documentElementOf();
        if (mount !== null) mount.appendChild(found);
      }
      return found;
    }
  });

  documentNode = Object.create(Document.prototype);
  register(documentNode, DOCUMENT_NODE, null);
  (function () {
    var html = newElement('html');
    documentNode.appendChild(html);
    html.appendChild(newElement('head'));
    html.appendChild(newElement('body'));
  })();
  documentNode.fonts = new FontFaceSet();
  global.Document = Document;
  global.document = documentNode;

  // document.readyState and the load events. The document is 'loading' while the
  // app's code first runs, as in a browser, and the host calls
  // __screenkitDocumentLoaded once it has: the bundle evaluated, or -- for a
  // package -- its entry module resolved. Then 'interactive' with
  // DOMContentLoaded, and on the next task 'complete' with window's load. Code
  // that waits for either (Phaser boots on DOMContentLoaded) gets it, and code
  // that checks readyState first finds the right answer. Once only.
  var documentReadyState = 'loading';
  Object.defineProperty(Document.prototype, 'readyState', {
    get: function () { self(this); return this === documentNode ? documentReadyState : 'complete'; },
    enumerable: true,
    configurable: true
  });
  function trusted(event) {
    event.isTrusted = true;
    return event;
  }
  global.__screenkitDocumentLoaded = function () {
    if (documentReadyState !== 'loading') return;
    documentReadyState = 'interactive';
    dispatchNow(documentNode, trusted(new Event('readystatechange')));
    dispatchNow(documentNode, trusted(new Event('DOMContentLoaded', { bubbles: true })));
    global.setTimeout(function () {
      documentReadyState = 'complete';
      dispatchNow(documentNode, trusted(new Event('readystatechange')));
      dispatchNow(global, trusted(new Event('load')));
    }, 0);
  };

  // -------------------------------------------------------------------------
  // XML: DOMParser
  //
  // parseFromString for the XML types, into an XMLDocument of elements,
  // attributes and text -- what bitmap-font loaders read (PixiJS's and Phaser's
  // both walk BMFont XML with getElementsByTagName and getAttribute), and XML
  // data files generally. HTML parsing stays absent (Architecture.md 3):
  // 'text/html' throws NotSupportedError. Comments, processing instructions and
  // the doctype are read and dropped; CDATA becomes text; the five predefined
  // entities and character references are decoded, and namespace prefixes are
  // resolved. Malformed XML gives what a browser gives: a document whose root is
  // <parsererror>, holding the reason.
  // -------------------------------------------------------------------------

  var XML_TYPES = table({ 'text/xml': 1, 'application/xml': 1, 'application/xhtml+xml': 1, 'image/svg+xml': 1 });
  var XML_NAMESPACE = 'http://www.w3.org/XML/1998/namespace';
  var PARSER_ERROR_NAMESPACE = 'http://www.mozilla.org/newlayout/xml/parsererror.xml';
  var XML_ENTITIES = table({ lt: '<', gt: '>', amp: '&', quot: '"', apos: "'" });

  function XMLDocument() { throw new TypeError("Failed to construct 'XMLDocument': Illegal constructor"); }
  inherit(XMLDocument, Document);
  global.XMLDocument = XMLDocument;

  function makeXmlDocument(contentType) {
    var doc = Object.create(XMLDocument.prototype);
    register(doc, DOCUMENT_NODE, null).contentType = contentType;
    return doc;
  }
  function xmlElement(doc, qualifiedName, namespace) {
    var colon = qualifiedName.indexOf(':');
    var element = newElement(colon >= 0 ? qualifiedName.slice(colon + 1) : qualifiedName, Element.prototype);
    var node = nodes.get(element);
    node.namespace = namespace;
    node.prefix = colon >= 0 ? qualifiedName.slice(0, colon) : null;
    node.ownerDocument = doc;
    return element;
  }

  function XmlSyntaxError(message) { this.message = message; }

  function parseXml(text, doc) {
    var pos = 0, stack = [], scopes = [{ xml: XML_NAMESPACE }], rootSeen = false;
    function fail(message) {
      var line = text.slice(0, pos).split('\n').length;
      throw new XmlSyntaxError(message + ' (line ' + line + ')');
    }
    function decode(value) {
      return value.replace(/&([^;&<\s]*);?/g, function (match, name) {
        if (match.charAt(match.length - 1) !== ';') fail('an "&" that starts no entity');
        if (name.charAt(0) === '#') {
          var code = name.charAt(1) === 'x' ? parseInt(name.slice(2), 16) : parseInt(name.slice(1), 10);
          if (!(code >= 0 && code <= 0x10FFFF)) fail('an invalid character reference &' + name + ';');
          return String.fromCodePoint(code);
        }
        if (!XML_ENTITIES[name]) fail('an undefined entity &' + name + ';');
        return XML_ENTITIES[name];
      });
    }
    function lookup(prefix) {
      for (var i = scopes.length - 1; i >= 0; i--) {
        if (Object.prototype.hasOwnProperty.call(scopes[i], prefix)) return scopes[i][prefix];
      }
      return undefined;
    }
    function appendText(value) {
      if (value === '') return;
      if (stack.length === 0) {
        if (/\S/.test(value)) fail('text outside the root element');
        return;
      }
      var parent = nodes.get(stack[stack.length - 1]);
      var last = parent.kids.length ? nodes.get(parent.kids[parent.kids.length - 1]) : null;
      if (last !== null && last.type === TEXT_NODE) last.data += value;
      else insert(parent.object, makeText(value, doc), null);
    }
    function skipPast(terminator, what) {
      var end = text.indexOf(terminator, pos);
      if (end < 0) fail('an unterminated ' + what);
      var inner = text.slice(pos, end);
      pos = end + terminator.length;
      return inner;
    }
    var nameRe = /[A-Za-z_:\u00C0-\uFFFF][-A-Za-z0-9_:.\u00B7\u00C0-\uFFFF]*/y;
    var attrRe = /\s*([A-Za-z_:\u00C0-\uFFFF][-A-Za-z0-9_:.\u00B7\u00C0-\uFFFF]*)\s*=\s*(?:"([^"<]*)"|'([^'<]*)')/y;
    while (pos < text.length) {
      var lt = text.indexOf('<', pos);
      if (lt < 0) {
        appendText(decode(text.slice(pos)));
        break;
      }
      if (lt > pos) appendText(decode(text.slice(pos, lt)));
      pos = lt;
      if (text.startsWith('<?', pos)) { pos += 2; skipPast('?>', 'processing instruction'); continue; }
      if (text.startsWith('<!--', pos)) { pos += 4; skipPast('-->', 'comment'); continue; }
      if (text.startsWith('<![CDATA[', pos)) {
        pos += 9;
        var cdata = skipPast(']]>', 'CDATA section');
        if (stack.length === 0) fail('CDATA outside the root element');
        appendText(cdata);
        continue;
      }
      if (text.startsWith('<!DOCTYPE', pos)) {
        for (var depth = 0; pos < text.length; pos++) {
          var c = text.charAt(pos);
          if (c === '[') depth++;
          else if (c === ']') depth--;
          else if (c === '>' && depth === 0) break;
        }
        if (pos >= text.length) fail('an unterminated doctype');
        pos++;
        continue;
      }
      if (text.charAt(pos + 1) === '/') {
        pos += 2;
        nameRe.lastIndex = pos;
        var closing = nameRe.exec(text);
        if (closing === null) fail('a malformed end tag');
        pos = nameRe.lastIndex;
        while (/\s/.test(text.charAt(pos))) pos++;
        if (text.charAt(pos) !== '>') fail('a malformed end tag </' + closing[0] + '>');
        pos++;
        var open = stack.pop();
        if (open === undefined) fail('an end tag </' + closing[0] + '> with no open element');
        if (qualifiedNameOf(nodes.get(open)) !== closing[0]) {
          fail('the end tag </' + closing[0] + '> does not match <' + qualifiedNameOf(nodes.get(open)) + '>');
        }
        scopes.pop();
        continue;
      }
      pos++;
      nameRe.lastIndex = pos;
      var opening = nameRe.exec(text);
      if (opening === null) fail('a "<" that starts no tag');
      pos = nameRe.lastIndex;
      var attrs = [], scope = {};
      for (;;) {
        attrRe.lastIndex = pos;
        var attr = attrRe.exec(text);
        if (attr === null) break;
        pos = attrRe.lastIndex;
        var attrName = attr[1], attrValue = decode(attr[2] !== undefined ? attr[2] : attr[3]);
        for (var k = 0; k < attrs.length; k++) {
          if (attrs[k][0] === attrName) fail('the attribute ' + attrName + ' twice on <' + opening[0] + '>');
        }
        attrs.push([attrName, attrValue]);
        if (attrName === 'xmlns') scope[''] = attrValue === '' ? null : attrValue;
        else if (attrName.indexOf('xmlns:') === 0) scope[attrName.slice(6)] = attrValue;
      }
      while (/\s/.test(text.charAt(pos))) pos++;
      var selfClosing = text.startsWith('/>', pos);
      if (!selfClosing && text.charAt(pos) !== '>') fail('a malformed start tag <' + opening[0] + '>');
      pos += selfClosing ? 2 : 1;
      if (stack.length === 0 && rootSeen) fail('a second root element <' + opening[0] + '>');
      scopes.push(scope);
      var colon = opening[0].indexOf(':');
      var namespace = lookup(colon >= 0 ? opening[0].slice(0, colon) : '');
      if (colon >= 0 && namespace === undefined) fail('the unbound prefix in <' + opening[0] + '>');
      var element = xmlElement(doc, opening[0], namespace === undefined ? null : namespace);
      nodes.get(element).attrs = attrs;
      insert(stack.length ? stack[stack.length - 1] : doc, element, null);
      rootSeen = true;
      if (selfClosing) scopes.pop();
      else stack.push(element);
    }
    if (stack.length > 0) fail('<' + qualifiedNameOf(nodes.get(stack[stack.length - 1])) + '> is never closed');
    if (!rootSeen) fail('no root element');
  }

  function DOMParser() {
    if (!(this instanceof DOMParser)) {
      throw new TypeError("Failed to construct 'DOMParser': Please use the 'new' operator.");
    }
  }
  DOMParser.prototype.parseFromString = function (string, type) {
    requireArgs(arguments.length, 2, 'parseFromString', 'DOMParser');
    var mime = String(type);
    if (mime === 'text/html') {
      throw domError("Failed to execute 'parseFromString' on 'DOMParser': ScreenKit has no HTML parser -- " +
                     'parse XML (text/xml, application/xml, image/svg+xml) instead.', 'NotSupportedError');
    }
    if (XML_TYPES[mime] !== 1) {
      throw new TypeError("Failed to execute 'parseFromString' on 'DOMParser': The provided value '" + mime +
                          "' is not a valid enum value of type DOMParserSupportedType.");
    }
    var doc = makeXmlDocument(mime);
    try {
      parseXml(String(string), doc);
    } catch (err) {
      if (!(err instanceof XmlSyntaxError)) throw err;
      doc = makeXmlDocument(mime);
      var report = xmlElement(doc, 'parsererror', PARSER_ERROR_NAMESPACE);
      insert(doc, report, null);
      insert(report, makeText('XML parsing error: ' + err.message, doc), null);
    }
    return doc;
  };
  global.DOMParser = DOMParser;

  // -------------------------------------------------------------------------
  // Input: the host's key events
  //
  // The native InputRouter (runtime/core/include/screenkit/Input.h) turns SDL
  // keyboard, remote and gamepad events into (type, key, code, keyCode, repeat,
  // modifiers), one task per event, and calls this on the JS thread. As in a
  // browser, a key event is dispatched at the focused element --
  // `document.activeElement`, which is always <body> here -- and travels the
  // whole event path: capture down from window, then bubbling back up through
  // <html>, the document and window.
  //
  // What comes back is a stepper, not a finished dispatch: each call runs one
  // listener callback and returns true, then false when the dispatch is over.
  // The router runs a microtask checkpoint after every step, because that is
  // what a browser does for events it generates itself (HTML's "clean up after
  // running a callback") -- a promise the document listener resolves settles
  // before the window listener runs. Stepping from native is the only way to
  // get there: a checkpoint needs an empty JS stack.
  //
  // Back is delivered and nothing more: `GoBack` is a key like any other. What
  // an app does with Back at its root -- leave, or stay -- is the app's call,
  // made by calling `window.close()` (above), because no rule here can tell a
  // page that handled Back from one that ignored it.
  // -------------------------------------------------------------------------

  var MOD_SHIFT = 1, MOD_CONTROL = 2, MOD_ALT = 4, MOD_META = 8;
  global.__screenkitKey = function (type, key, code, keyCode, repeat, modifiers) {
    var event = new KeyboardEvent(type, {
      key: key, code: code, keyCode: keyCode, which: keyCode, repeat: repeat,
      shiftKey: !!(modifiers & MOD_SHIFT), ctrlKey: !!(modifiers & MOD_CONTROL),
      altKey: !!(modifiers & MOD_ALT), metaKey: !!(modifiers & MOD_META),
      bubbles: true, cancelable: true
    });
    event.isTrusted = true;
    return dispatcher(documentNode.activeElement || documentNode, event);
  };


  // -------------------------------------------------------------------------
  // Gamepads: the W3C Gamepad API
  //
  // SDL's gamepads, each in the `standard` mapping: buttons 0-3 South, East,
  // West, North; 4-5 the shoulders; 6-7 the triggers, analog; 8 Back, 9 Start;
  // 10-11 the stick clicks; 12-15 D-pad up, down, left, right; 16 Guide. Axes:
  // left X, left Y, right X, right Y, -1..1 with up and left negative. A button
  // is `pressed` (and `touched`) past 0.1.
  //
  // The main thread polls every pad once a frame into a snapshot
  // (core/src/input/Gamepads.h). `__screenkit.gamepads.read` copies it into
  // `gamepadBuffer`, and the Gamepad objects are refreshed from that in place:
  // a connection keeps one object, handed out by every getGamepads(), and a call
  // that finds nothing changed allocates no Gamepad and no GamepadButton. A pad
  // that leaves keeps its object with `connected` false, no longer refreshed.
  // `timestamp` is the wall-clock time the snapshot stamped on the last change,
  // moved onto performance.now()'s clock, so Phaser's `timestamp < created`
  // holds.
  //
  // Connections arrive as tasks from InputRouter, each naming its connection's
  // serial: a pad that came and went while the runtime was paused still fires
  // both events, the first with an object describing the pad as it was.
  //
  // The first getGamepads() claims the gamepads for the rest of the process:
  // InputRouter stops turning their buttons and sticks into arrow keys and
  // Enter, which a game reading the pads would otherwise get twice.
  //
  // Evidence: Phaser's gamepad plugin (device/Input.js, input/gamepad/) enables
  // itself on `navigator.getGamepads`, polls it every frame, and reads `id`,
  // `index`, `buttons[i].value`, `axes`, `timestamp`, assigns `connected`, and
  // takes `event.gamepad.index` from window's connection events.
  // -------------------------------------------------------------------------

  var GAMEPAD_BUTTONS = 17, GAMEPAD_AXES = 4;
  // One slot of what __screenkit.gamepads.read writes (bindings/Gamepads.h):
  // connection serial (0 for an empty slot), last change in epoch ms, flags,
  // then the buttons and the axes.
  var GAMEPAD_SLOT = 3 + GAMEPAD_BUTTONS + GAMEPAD_AXES;
  var GAMEPAD_FLAG_RUMBLE = 1;
  var GAMEPAD_PRESSED = 0.1;
  var GAMEPAD_EFFECT_MAX_MS = 5000;
  var gamepadIo = io && io.gamepads ? io.gamepads : null;
  var gamepadBuffer = new Float64Array(4 * GAMEPAD_SLOT);
  // index -> the connected Gamepad in that slot, trailing empty slots trimmed.
  var gamepadSlots = [];
  // serial -> Gamepad, from when the page can first see it until its
  // gamepaddisconnected has fired.
  var gamepadsBySerial = Object.create(null);
  var gamepadStates = new WeakMap();
  var gamepadButtonStates = new WeakMap();
  var hapticStates = new WeakMap();
  var HAPTIC_EFFECTS = Object.freeze(['dual-rumble']);

  function internalState(map, object, name) {
    var state = map.get(object);
    if (state === undefined) throw new TypeError('Illegal invocation: the receiver is not a ' + name);
    return state;
  }

  function GamepadButton() { throw new TypeError("Failed to construct 'GamepadButton': Illegal constructor"); }
  define(GamepadButton.prototype, {
    pressed: { get: function () { return internalState(gamepadButtonStates, this, 'GamepadButton').pressed; } },
    touched: { get: function () { return internalState(gamepadButtonStates, this, 'GamepadButton').pressed; } },
    value: { get: function () { return internalState(gamepadButtonStates, this, 'GamepadButton').value; } }
  });
  global.GamepadButton = GamepadButton;

  function Gamepad() { throw new TypeError("Failed to construct 'Gamepad': Illegal constructor"); }
  function gamepadGetter(name) {
    return { get: function () { return internalState(gamepadStates, this, 'Gamepad')[name]; } };
  }
  define(Gamepad.prototype, {
    id: gamepadGetter('id'),
    index: gamepadGetter('index'),
    mapping: { get: function () { internalState(gamepadStates, this, 'Gamepad'); return 'standard'; } },
    timestamp: gamepadGetter('timestamp'),
    axes: gamepadGetter('axes'),
    buttons: gamepadGetter('buttons'),
    vibrationActuator: gamepadGetter('actuator')
  });
  global.Gamepad = Gamepad;

  function newGamepad(index, serial, id, rumble, live) {
    var pad = Object.create(Gamepad.prototype);
    var buttons = [];
    var buttonStates = [];
    for (var b = 0; b < GAMEPAD_BUTTONS; b++) {
      var button = Object.create(GamepadButton.prototype);
      var buttonState = { value: 0, pressed: false };
      gamepadButtonStates.set(button, buttonState);
      buttons.push(button);
      buttonStates.push(buttonState);
    }
    var state = {
      index: index, serial: serial, id: String(id), live: live,
      timestamp: global.performance.now(), changed: -1,
      axes: [0, 0, 0, 0], buttons: buttons, buttonStates: buttonStates, actuator: null
    };
    gamepadStates.set(pad, state);
    if (rumble) {
      state.actuator = Object.create(GamepadHapticActuator.prototype);
      hapticStates.set(state.actuator, { pad: pad, effect: null });
    }
    // An own, writable property rather than a getter: Phaser assigns it when
    // it loses the pads, and a strict-mode assignment to a getter throws.
    pad.connected = live;
    return pad;
  }

  function gamepadLeft(pad) {
    var state = gamepadStates.get(pad);
    state.live = false;
    pad.connected = false;
    // An effect still playing on it is preempted now, not when its timer fires.
    if (state.actuator !== null) endEffect(hapticStates.get(state.actuator), 'preempted');
  }

  // Bring every Gamepad the page can see up to the snapshot. `claim` is
  // getGamepads(): only the app asking claims the pads, not an event firing.
  function refreshGamepads(claim) {
    var count = 0;
    if (gamepadIo !== null) {
      count = gamepadIo.read(gamepadBuffer.buffer, claim);
      // More pads than the buffer holds: grow it and read again until they fit.
      while (count * GAMEPAD_SLOT > gamepadBuffer.length) {
        gamepadBuffer = new Float64Array(count * GAMEPAD_SLOT);
        count = gamepadIo.read(gamepadBuffer.buffer, false);
      }
    }
    var slots = Math.max(count, gamepadSlots.length);
    for (var i = 0; i < slots; i++) {
      var base = i * GAMEPAD_SLOT;
      var serial = i < count ? gamepadBuffer[base] : 0;
      var pad = gamepadSlots[i] || null;
      if (pad !== null && gamepadStates.get(pad).serial !== serial) {
        gamepadLeft(pad);
        gamepadSlots[i] = null;
        pad = null;
      }
      if (serial === 0) continue;
      if (pad === null) {
        var id = gamepadIo.id(i);
        pad = newGamepad(i, serial, id === undefined ? '' : id,
                         (gamepadBuffer[base + 2] & GAMEPAD_FLAG_RUMBLE) !== 0, true);
        gamepadSlots[i] = pad;
        gamepadsBySerial[serial] = pad;
      }
      var state = gamepadStates.get(pad);
      // The snapshot's stamp moves on every change, and only then.
      if (state.changed === gamepadBuffer[base + 1]) continue;
      state.changed = gamepadBuffer[base + 1];
      state.timestamp = state.changed - TIME_ORIGIN;
      for (var b = 0; b < GAMEPAD_BUTTONS; b++) {
        var buttonState = state.buttonStates[b];
        buttonState.value = gamepadBuffer[base + 3 + b];
        buttonState.pressed = buttonState.value > GAMEPAD_PRESSED;
      }
      for (var a = 0; a < GAMEPAD_AXES; a++) state.axes[a] = gamepadBuffer[base + 3 + GAMEPAD_BUTTONS + a];
    }
    while (gamepadSlots.length > 0 && !gamepadSlots[gamepadSlots.length - 1]) gamepadSlots.pop();
  }

  // navigator.getGamepads(): at least four slots, null where no pad is. A host
  // with no gamepads -- no SDL gamepad subsystem, or no __screenkit.gamepads --
  // answers four nulls.
  function getGamepads() {
    refreshGamepads(true);
    var pads = [];
    var length = Math.max(4, gamepadSlots.length);
    for (var i = 0; i < length; i++) pads.push(gamepadSlots[i] || null);
    return pads;
  }

  function GamepadEvent(type, init) {
    if (init === null || typeof init !== 'object' || init.gamepad === undefined) {
      throw new TypeError("Failed to construct 'GamepadEvent': required member gamepad is undefined.");
    }
    if (!(init.gamepad instanceof Gamepad)) {
      throw new TypeError("Failed to construct 'GamepadEvent': member gamepad is not of type 'Gamepad'.");
    }
    Event.call(this, type, init);
    this.gamepad = init.gamepad;
  }
  GamepadEvent.prototype = Object.create(Event.prototype);
  GamepadEvent.prototype.constructor = GamepadEvent;
  global.GamepadEvent = GamepadEvent;

  // A connection or disconnection (InputRouter, native), fired at window. The
  // snapshot is read first, so the event's gamepad is current -- or, for a pad
  // already gone by the time the task runs, not connected.
  global.__screenkitGamepad = function (type, index, serial, id, rumble) {
    refreshGamepads(false);
    var pad = gamepadsBySerial[serial];
    if (pad === undefined) {
      pad = newGamepad(index, serial, id, rumble, false);
      if (type === 'gamepadconnected') gamepadsBySerial[serial] = pad;
    }
    if (type === 'gamepaddisconnected') delete gamepadsBySerial[serial];
    var event = new GamepadEvent(type, { gamepad: pad });
    event.isTrusted = true;
    return dispatcher(global, event);
  };

  // Rumble: `dual-rumble` only, through SDL_RumbleGamepad -- strong is the
  // low-frequency motor, weak the high. Magnitudes are clamped to 0..1 and the
  // duration and start delay to 5 s rather than rejected. An effect resolves
  // 'complete' when it has run, or 'preempted' when another effect or reset()
  // comes first or the pad is gone.
  function GamepadHapticActuator() {
    throw new TypeError("Failed to construct 'GamepadHapticActuator': Illegal constructor");
  }

  // A number from an effect's parameters, clamped to 0..max; missing or NaN is 0.
  function effectParameter(params, name, max) {
    var value = params === null || typeof params !== 'object' ? 0 : Number(params[name]);
    return value > 0 ? Math.min(value, max) : 0;
  }

  // End the effect in progress, if any, with `result`. True when it had
  // started rumbling.
  function endEffect(haptic, result) {
    var effect = haptic.effect;
    if (effect === null) return false;
    haptic.effect = null;
    if (effect.timer !== 0) global.clearTimeout(effect.timer);
    effect.timer = 0;
    effect.resolve(result);
    return effect.started;
  }

  // False when the pad is gone.
  function rumbleGamepad(pad, strong, weak, durationMs) {
    var state = gamepadStates.get(pad);
    if (!state.live || gamepadIo === null) return false;
    return gamepadIo.rumble(state.index, Math.round(strong * 65535), Math.round(weak * 65535), durationMs) === true;
  }

  define(GamepadHapticActuator.prototype, {
    effects: {
      get: function () {
        internalState(hapticStates, this, 'GamepadHapticActuator');
        return HAPTIC_EFFECTS;
      }
    },
    playEffect: function (type, params) {
      var haptic = hapticStates.get(this);
      if (haptic === undefined) {
        return Promise.reject(new TypeError('Illegal invocation: the receiver is not a GamepadHapticActuator'));
      }
      if (String(type) !== 'dual-rumble') {
        return Promise.reject(domError("Failed to execute 'playEffect' on 'GamepadHapticActuator': '" + String(type) +
                                       "' is not a supported effect type; this gamepad plays 'dual-rumble'.",
                                       'NotSupportedError'));
      }
      // SDL takes whole milliseconds, and so does the timer that ends the effect.
      var duration = Math.round(effectParameter(params, 'duration', GAMEPAD_EFFECT_MAX_MS));
      var startDelay = effectParameter(params, 'startDelay', GAMEPAD_EFFECT_MAX_MS);
      var strong = effectParameter(params, 'strongMagnitude', 1);
      var weak = effectParameter(params, 'weakMagnitude', 1);
      var wasRumbling = endEffect(haptic, 'preempted');
      return new Promise(function (resolve) {
        var effect = { resolve: resolve, timer: 0, started: false };
        haptic.effect = effect;
        function start() {
          effect.timer = 0;
          effect.started = true;
          if (!rumbleGamepad(haptic.pad, strong, weak, duration)) {
            endEffect(haptic, 'preempted');
            return;
          }
          effect.timer = global.setTimeout(function () {
            effect.timer = 0;
            if (haptic.effect !== effect) return;
            refreshGamepads(false);
            endEffect(haptic, gamepadStates.get(haptic.pad).live ? 'complete' : 'preempted');
          }, duration);
        }
        if (startDelay > 0) {
          // What this preempted stops now, not when this starts.
          if (wasRumbling) rumbleGamepad(haptic.pad, 0, 0, 0);
          effect.timer = global.setTimeout(start, startDelay);
        } else {
          start();
        }
      });
    },
    reset: function () {
      var haptic = hapticStates.get(this);
      if (haptic === undefined) {
        return Promise.reject(new TypeError('Illegal invocation: the receiver is not a GamepadHapticActuator'));
      }
      endEffect(haptic, 'preempted');
      rumbleGamepad(haptic.pad, 0, 0, 0);
      return Promise.resolve('complete');
    }
  });
  global.GamepadHapticActuator = GamepadHapticActuator;


  // -------------------------------------------------------------------------
  // Present only frames that painted
  //
  // WebGL composites a canvas only in a frame that modified its drawing buffer;
  // an untouched canvas keeps showing its last image. The host was calling
  // eglSwapBuffers on *every* frame instead. That was invisible while the only
  // app redrew every frame (M4's triangle) and fatal for a real one: Lightning
  // draws its scene once and then goes idle -- measured, 9 draws in the first
  // second and 0 per second after -- so every idle frame presented an undefined
  // back buffer and the screen stayed black.
  //
  // So this marks a frame as painted when something draws into the *default*
  // framebuffer, and the host's frame hook swaps only then. Draws into an
  // offscreen framebuffer (render-to-texture) do not count: presenting after one
  // of those would show exactly the stale buffer this exists to avoid.
  //
  // **One flag per canvas.** The page's frame sets `__screenkitPainted`, as it
  // always has; every canvas with a layer of its own sets
  // `__screenkitLayerPainted[contextId]`, and the frame boundary publishes only
  // the layers that painted -- so an idle canvas keeps its last image and costs
  // nothing, exactly as the page's own frame does (gfx/VendoredWebGL.h).
  // -------------------------------------------------------------------------

  function trackPaint(g, key) {
    if (g === null || typeof g !== 'object') return;  // headless: nothing is ever presented
    var mark;
    if (key === null) {
      global.__screenkitPainted = false;
      mark = function () { global.__screenkitPainted = true; };
    } else {
      var flags = global.__screenkitLayerPainted;
      if (!flags) {
        flags = Object.create(null);
        global.__screenkitLayerPainted = flags;
      }
      flags[key] = false;
      mark = function () { flags[key] = true; };
    }
    var drawingToDefault = true;

    var bindFramebuffer = g.bindFramebuffer;
    if (typeof bindFramebuffer === 'function') {
      g.bindFramebuffer = function (target, framebuffer) {
        if (target === g.FRAMEBUFFER || target === g.DRAW_FRAMEBUFFER) {
          drawingToDefault = framebuffer === null || framebuffer === undefined;
        }
        return bindFramebuffer.apply(g, arguments);
      };
    }

    ['clear', 'drawArrays', 'drawElements', 'drawArraysInstanced', 'drawElementsInstanced',
     'drawRangeElements', 'clearBufferfv', 'clearBufferiv', 'clearBufferuiv', 'clearBufferfi',
     'blitFramebuffer'].forEach(function (name) {
      var original = g[name];
      if (typeof original !== 'function') return;
      g[name] = function () {
        if (drawingToDefault) mark();
        return original.apply(g, arguments);
      };
    });
  }

  trackPaint(glContext(), null);


  // -------------------------------------------------------------------------
  // Deliberately absent: Worker, OffscreenCanvas, and a 2D context
  //
  // Worker and OffscreenCanvas are feature-detected -- `hasWorker = !!self.Worker`
  // and `typeof OffscreenCanvas < "u" ? new OffscreenCanvas(0,0) : null` -- and
  // Lightning takes a main-thread path without them.
  //
  // The 2D context is NOT feature-detected everywhere, and that is why it is
  // absent rather than stubbed. Its non-null-safe callers in Lightning are all
  // paths this runtime does not take: CanvasTextRenderer (web fonts -- every font
  // here is MSDF, drawn through WebGL), CanvasCoreRenderer (renderMode "canvas"),
  // and loadSvg (no SVG). The remaining callers check for null. The one that
  // crashed an unmodified app, CanvasTextRenderer.init, is removed at build time
  // by the ScreenKit Vite config, because Blits registers it unconditionally. A
  // stub here would instead make getContext("2d") look like it works. Defining any of them without a real backend would
  // flip that detection and route rendering into something broken. Leaving them
  // undefined is the correct shim, not an omission. See runtime/js/README.md.
  // -------------------------------------------------------------------------

  return SHIM;
})(typeof globalThis !== 'undefined' ? globalThis : this);
