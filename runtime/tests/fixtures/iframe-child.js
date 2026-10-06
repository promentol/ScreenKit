// The package a launcher embeds in an `<iframe>`: a second app, on its own
// runtime and its own thread, drawing into its own layer (Architecture.md 5).
//
// Everything the `iframe-*` rows need to observe is reported through
// `postMessage`, because that is the only channel there is: the two runtimes
// share no object, so a row cannot read this app's variables and this app
// cannot read the row's. What it proves, per matrix line:
//
//   - it runs and paints: it clears its drawable to a flat colour every frame,
//     which the parent composites and a row reads back as pixels;
//   - `Paused` is real: the rAF and timer counters it reports inside its own
//     `pause` and `resume` handlers are the same number;
//   - a message is a copy: it mutates what it was handed and reports what it
//     saw, which the sender then checks against its own;
//   - `source` can reply, and `window.parent` reaches the launcher;
//   - one level of nesting: an `<iframe>` in here fires `error`;
//   - `sandbox` shows up as the capabilities it was, or was not, given.
(function () {
  var canvas = document.createElement('canvas');
  document.body.appendChild(canvas);
  var gl = canvas.getContext('webgl');

  // Distinctive on purpose: a row that reads this back out of the parent's
  // frame is reading *this* app's pixels and could not be reading its own.
  var frames = 0;
  var timers = 0;

  function paint() {
    gl.clearColor(0, 1, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    frames++;
    requestAnimationFrame(paint);
  }
  requestAnimationFrame(paint);
  setInterval(function () { timers++; }, 4);

  function tell(message) {
    window.parent.postMessage(message);
  }

  // Sent from inside the dispatch, before the freeze gate closes behind it --
  // which is the whole reason the event and the freeze are one task -- and
  // carrying the counters, so the parent can compare the two numbers rather
  // than take "it was paused" on trust.
  window.addEventListener('pause', function () {
    tell({ lifecycle: 'pause', frames: frames, timers: timers });
  });
  window.addEventListener('resume', function () {
    tell({ lifecycle: 'resume', frames: frames, timers: timers });
  });

  // Input follows focus: a key the remote sent while this app had it. Reported
  // rather than acted on, so the row can say which context received it.
  var keys = [];
  window.addEventListener('keydown', function (e) {
    keys.push(e.key);
    tell({ key: e.key, keys: keys.length });
  });

  window.addEventListener('message', function (event) {
    var data = event.data;
    if (!data || typeof data !== 'object') return;
    if (data.ask === 'keys') {
      event.source.postMessage({ sawKeys: keys.join(','), tag: data.tag });
    } else if (data.ask === 'counters') {
      // Replying through `source` rather than `window.parent`: the matrix says
      // the source can reply, so this is where that is proven.
      event.source.postMessage({ frames: frames, timers: timers, tag: data.tag });
    } else if (data.ask === 'mutate') {
      // The receiver holds its own copy: growing it here must not grow the
      // sender's. The cycle proves a cyclic message survives as a cycle.
      data.list.push('child');
      tell({
        sawLength: data.list.length,
        sawNested: data.list[0].n,
        cyclic: data.self === data,
        bytes: data.bytes ? new Uint8Array(data.bytes)[0] : -1
      });
    } else if (data.ask === 'capabilities') {
      var caps = globalThis.__screenkit.instances.capabilities();
      var report = {
        net: !!globalThis.__screenkit.net,
        media: !!globalThis.__screenkit.media,
        canEmbed: caps.canEmbed,
        hasParent: caps.hasParent,
        sandboxed: caps.sandboxed,
        parentIsSelf: window.parent === window,
        topIsParent: window.top === window.parent,
        cloneThrows: (function () {
          try {
            window.parent.postMessage(function () {});
            return 'allowed';
          } catch (e) {
            return e.name;
          }
        })()
      };
      if (report.net) {
        tell(report);
      } else {
        // No client at all, so this never reaches the network: it is the
        // documented unavailable shape, and asking for it is what makes the
        // sandbox say why -- once.
        fetch('https://example.invalid/').then(function () {
          report.fetch = 'resolved';
          tell(report);
        }, function (error) {
          report.fetch = String(error && error.message ? error.message : error);
          tell(report);
        });
      }
    } else if (data.ask === 'nest') {
      // One level of nesting: an `<iframe>` in here is refused, loudly.
      var nested = document.createElement('iframe');
      nested.onerror = function () { tell({ nested: 'error', frames: frames }); };
      nested.onload = function () { tell({ nested: 'LOADED' }); };
      nested.src = 'anything.skpkg';
      document.body.appendChild(nested);
    } else if (data.ask === 'focus-parent') {
      window.parent.focus();
    } else if (data.ask === 'focus-parent-after') {
      // Hand the remote back after this many of *this* app's frames, so the row
      // can see the launcher genuinely frozen in between rather than racing a
      // pause and a resume that were asked for in the same turn.
      var target = frames + (data.frames || 10);
      (function waitFrames() {
        if (frames >= target) {
          window.parent.focus();
          return;
        }
        requestAnimationFrame(waitFrames);
      })();
    }
  });

  tell({ ready: true });
})();
