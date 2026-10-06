---
title: Embedding apps with <iframe>
description: One iframe is one instance — its own Hermes runtime, thread, GL context and layer — with focus deciding which one runs.
---

**One `<iframe>` is one instance**: its own Hermes runtime, on its own thread, with its own GL
context in the host's share group, drawing into a texture the host composites at the element's CSS
rect.

```js
const game = document.createElement('iframe')
game.src = 'games/tetris.skpkg'          // a package inside this app
game.sandbox = 'allow-media allow-storage'
game.style.cssText = 'position: absolute; left: 12%; top: 14%; width: 76%; height: 72%'
document.body.appendChild(game)

game.addEventListener('load', () => game.focus())   // the launcher pauses, the game runs
game.contentWindow.postMessage({ type: 'resume-save', slot: 3 })
window.addEventListener('message', (e) => { /* child → parent */ })
```

## Three ways this differs from the web

- **`src` names a local package, not a URL.** The path is confined to the parent's own package. A
  launcher ships the games it embeds, so nothing is downloaded — there is no cache and no integrity
  check to get wrong. An `http(s)` src is refused with a documented error.
- **Instances nest one level deep.** An `<iframe>` inside an instance fires `error` on that element,
  and the instance keeps running.
- **`postMessage` carries a structured-clone subset**: primitives, plain objects and arrays, and
  `ArrayBuffer` by value, with cycles preserved. Anything else throws `DataCloneError` at the sender.
  The two runtimes share no memory, so there is no transfer and no `MessagePort`.

`postMessage` is the only cross-instance channel. It maps onto the per-instance task queues, so there
is no second IPC surface to learn.

## Exactly one browsing context runs

```
Loading ──▶ Running ──▶ Paused ──▶ Terminated
               ▲          │
               └──────────┘  resume
```

`focus()` is atomic: it pauses the outgoing context before resuming the incoming one. Removing the
element terminates its instance.

| State | rAF | Timers | Input | Audio/Video | JS heap | GL resources |
|---|---|---|---|---|---|---|
| `Running` | firing | firing | routed | playing | live | live |
| `Paused` | stopped | **frozen** | none | stopped | retained | retained |
| `Terminated` | — | — | — | — | freed | freed |

- `Paused` **freezes the task queue** rather than letting timers pile up, so a backgrounded game
  burns no CPU and fires no burst of stale callbacks on resume. Resuming is instant.
- A paused context keeps its **last rendered frame** as a compositor texture, so a launcher can show
  a live-looking tile of a frozen game for free.
- `Paused` stops the *page*, not the *window*. A launcher that freezes itself to give a game the
  remote still owns the compositor, so it keeps serving the present — and nothing else.

Contexts receive `pause` and `resume` on `window`.

:::caution[`Suspended` is not implemented]
Dropping regenerable GL resources while keeping the heap, and memory-pressure LRU escalation, are
deferred — they need a per-platform memory-pressure signal that does not exist yet. `suspend` and
`memorywarning` are **not dispatched**, deliberately, so a page never waits on an event that will
never come. On tvOS, a launcher embedding many games must terminate what it is not showing.
:::

## Input follows focus

Instances receive input and `resize` only while focused, and the focused context is the one that
runs. A focus switch releases whatever keys were held, so no context is left with a key down it will
never see go up.

## The boundary is not a security boundary

On the web an `<iframe>` implies origin isolation and a separate address space. Here it does not:

- **JavaScript is isolated** — separate Hermes runtimes share no objects, and `postMessage` is the
  only channel.
- **Memory is not.** A native crash or OOM in any instance kills every instance, the launcher
  included. There is no hard per-instance memory cap.
- **`sandbox` gates capabilities**, not memory or CPU.

:::danger
This cannot be presented to third-party developers as containment. Embed code you trust.
:::
