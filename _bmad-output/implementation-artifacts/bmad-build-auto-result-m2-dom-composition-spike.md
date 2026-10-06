---
status: blocked
---

# BMad Build Auto Result

Status: blocked
Blocking condition: intent gap — M2's exit criterion cannot be measured on this machine, and the
intent does not say which of several defensible substitutes to use.

## The gate, as written

> **M2 — DOM composition spike — GATE.** The WHATWG-package DOM layer fits under **500 KB bytecode**
> and **300 ms cold start on Fire TV**. On failure, hand-written minimal shim.

It has two halves. One is measurable today. The other cannot be run at all.

| Half | Status | Evidence |
|---|---|---|
| Under 500 KB bytecode | **measurable now** | `hermesc` is in the prebuilts cache at `~/.screenkit/prebuilts/hermes/260318099.0.2/compiler/hermesc/osx-bin/hermesc`. The seven packages are named in `Architecture.md` §3. Nothing blocks bundling them and compiling to `.hbc`. |
| Under 300 ms cold start **on Fire TV** | **not runnable** | There is no Android capability anywhere in the project. |

## Why the cold-start half cannot run

- `runtime/` contains exactly one platform directory: `apple`. There is no Android branch in
  `runtime/CMakeLists.txt`.
- `tools/prebuilts/manifest.json` lists **no** Android targets for `angle` or `hermes`; both record
  Android under `unavailable`. For ANGLE the recorded reason is decisive: *"No public prebuilt ANGLE
  for Android exists. Android ships ANGLE as an optional system driver, not a linkable library."*
- Standing up an Android host is **M10** (`Android TV / Fire TV shell`), which has not started. It
  needs the NDK toolchain, prefab wiring, SDL3 for Android, and an ANGLE source build — not spike work.
- No Fire TV device is attached.

## Why this is an intent gap rather than a decision to make

Three readings are defensible, they produce observably different outcomes, and the invocation intent
(the milestone row alone) selects between none of them:

1. **Measure the bytecode half now, defer cold start until M10 exists.** The gate reports a partial
   result and does not close. Cheapest, and honest about what was measured.
2. **Measure cold start on a proxy** (macOS or the tvOS simulator) and extrapolate to Fire TV. This is
   explicitly disallowed by this workflow's own READY FOR DEVELOPMENT standard — *"ACs observe the
   outermost surface the intent references — never a more internal proxy for it"* — and
   `Architecture.md` §3 says **"Measure before committing."** An M3 Pro number is not a Fire TV number.
3. **Build enough Android to measure it.** That is M10 pulled forward, and it inverts the
   dependency the architecture states at line 626: *"M1 and M2 run before M0 — they are the only
   items that can invalidate the architecture."*

Picking one would resolve the gap by fiat, which step 2 forbids.

## Unanswered questions

1. Should M2 close on the bytecode budget alone for now, with cold start re-run when M10 lands?
2. If cold start must be measured now, on what? A Fire TV Stick, an Android TV emulator, or an
   agreed proxy device with a stated scaling assumption?
3. Is the 300 ms budget wall-clock from process start, or from runtime init to first DOM call? The
   architecture states the number but not the boundary, and the two differ by the Hermes boot cost
   already measured in M3.

## What is ready to run the moment question 1 is answered

The bytecode half needs no new infrastructure: npm-install the seven packages named in
`Architecture.md` §3 (`symbol-tree`, `webidl-conversions`, `whatwg-url`, `@asamuzakjp/dom-selector`,
`css-tree`, `@asamuzakjp/css-color`, `whatwg-mimetype` + `data-urls`), bundle them, compile with the
pinned `hermesc`, and compare the `.hbc` against 500 KB. That is a contained, decidable measurement
and it answers the half of the gate that most affects the architecture.
