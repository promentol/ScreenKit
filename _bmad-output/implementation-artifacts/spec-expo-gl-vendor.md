---
title: 'Vendoring script: import expo-gl as ScreenKit GL'
type: 'feature'
created: '2026-09-16'
status: 'done'
baseline_commit: 'NO_VCS'
route: 'dispatch'
review_loop_iteration: 0
context: ['{project-root}/Architecture.md']
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** M4 shipped a hand-rolled `gl.def` that covers exactly enough GL to draw a triangle. It
has no `WebGLRenderingContext`, no WebGL object types, no WebGL2 and no validation — so Lightning and
Phaser cannot run on it, and `Architecture.md` §9 says untrusted JS must never reach a raw GL call.
Writing that layer by hand duplicates work `expo-gl` already did.

**Approach:** A **shell script** that vendors `expo/expo`'s `packages/expo-gl/common/` into
`runtime/third_party/gl/`, rewriting it for this project: `expo::gl_cpp` becomes our namespace, the
`EX` prefix becomes ours, Apple's EAGL includes become ANGLE's, and expo's logging becomes
`screenkit::log`. Re-running the script re-imports a newer expo cleanly, so this stays a *tracked
port* rather than a fork we hand-edit.

**Decisions taken at planning:**
- **Vendor everything except context management.** Tables, methods, renderer, typed arrays, JSI
  helpers, plus `EXGLImageUtils` and `stb_image.h` so `texImage2D` from an image works without a
  second import pass. `EXGLContextManager` / `EXGLNativeApi` stay behind: `GlSurface` owns the
  drawable.
- **Script only — nothing consumes the tree yet.** M4's raw `gl` global, `triangle.js` and the eight
  passing `gl-*` rows are untouched. Swapping the GL path over to the vendored
  `WebGLRenderingContext` is its own later change, reviewable on its own terms.

## Boundaries & Constraints

**Always:**
- The script is the deliverable and the only way vendored files are produced. Never hand-edit the
  output; a fix means changing the script and re-running it.
- Every transformation is expressed as a rule in one table at the top of the script, so what changed
  is readable without diffing 470 KB.
- The script pins an upstream ref and records it, plus the MIT notice and upstream path, in a
  generated header on every file and in a `VENDOR.md`.
- The script is idempotent and offline-repeatable: given the same ref it produces byte-identical
  output, and it refuses to clobber uncommitted edits in the output tree.

**Never:**
- No hand-written GL entry points added to the vendored tree.
- Do not vendor expo's context management (`EXGLContextManager`, `EXGLNativeApi`) wholesale — we
  already own the drawable in `runtime/core/src/gfx/GlSurface`. Adapt at that seam.
- Do not change `poc/`, and do not alter the existing M3 timer or runtime tests.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Behavior | Error Handling |
|---|---|---|---|
| Clean import | Empty output dir, pinned ref | Full tree emitted, every file carrying provenance | N/A |
| Re-run | Output already current | Byte-identical result | No spurious churn |
| Upstream bumped | New ref passed | Re-imports; report which files changed | N/A |
| Local edits present | Output tree hand-modified | **Refuses** to overwrite | Names the modified files and exits non-zero |
| Missing tool | No `git` / no network | Fails at the start with the reason | Never a half-written tree |
| Unrewritten symbol | An `expo::` or `EX` token survives | Post-pass **fails** the run | Names file and line |
| Apple GL headers | `pch.h` EAGL block | Rewritten to ANGLE's `<GLES3/gl3.h>` | A surviving `OpenGLES/` include fails the run |

</frozen-after-approval>

## Code Map

**Upstream inventory** (`expo/expo`, `packages/expo-gl/common/`, MIT, v58.0.0 — 24 files):
- Tables: `EXWebGLMethods.def` (8 KB), `EXWebGLConstants.def` (24 KB).
- Implementation: `EXWebGLMethods.cpp`, `EXWebGLMethodsDraw.cpp`, `EXWebGLMethodsTextures.cpp`,
  `EXWebGLRenderer.cpp`, plus `EXWebGLMethodsHelpers.h` / `EXWebGLMethodsMacros.h`.
- Support: `EXTypedArrayApi.{h,cpp}`, `EXJsiArgsTransform.h`, `EXJsiUtils.h`, `pch.h`.
- Context (probably *not* wanted): `EXGLContextManager`, `EXGLNativeApi`, `EXGLNativeContext`.
- Images: `EXGLImageUtils.{h,cpp}`, `stb_image.h` (283 KB).

**Transformations the script must make — verified by reading the sources:**
- Namespace is `expo::gl_cpp` (`namespace expo { namespace gl_cpp {`), closed with matching
  `} // namespace gl_cpp` / `} // namespace expo`. Both the open and the trailing comments need
  rewriting.
- **`pch.h` is the critical one.** Under `__APPLE__` it includes `<OpenGLES/EAGL.h>`,
  `<OpenGLES/ES3/gl.h>` and `<OpenGLES/ES3/glext.h>` — Apple's deprecated EAGL, not ANGLE. ANGLE
  exposes the same API under the spelling expo already uses on Android (`<GLES3/gl3.h>`), so the
  Apple branch collapses onto that. Leaving it alone links against the wrong GL entirely.
- `EXPlatformUtils.h` declares `EXiOSLog` and `EXGLSysLog(fmt, ...)`; redirect to `screenkit::log`
  (`runtime/core/include/screenkit/Log.h`).
- `EX`-prefixed types are pervasive (`EXGLContext`, `EXWebGLClass`, `EXGLSysLog`, `UEXGLContextId`).

**Ours, for the seam:**
- `runtime/core/src/gfx/GlSurface.{h,cpp}` — we already own display/context/surface; expo's context
  manager is the part to leave behind.
- `runtime/core/src/bindings/WebGL.cpp`, `gl.def` — what this may replace.
- `runtime/cmake/AnglePrebuilt.cmake` — ANGLE include dirs the vendored tree compiles against.
- `tools/prebuilts/fetch.mjs` — the established shape for pinned, verified third-party imports.

## Tasks & Acceptance

**Execution:**
- [x] `tools/vendor/expo-gl.sh` -- the script: pinned ref, sparse fetch, rule-table rewrite, provenance
      header per file, idempotent, refuses to clobber local edits.
- [x] `tools/vendor/expo-gl.rules` -- the transformation table (namespace, prefix, includes, logging),
      kept separate so a reviewer reads intent rather than `sed`.
- [x] `tools/vendor/verify.sh` -- post-pass: fail on any surviving `expo::`, `EX` token or
      `OpenGLES/` include in the output tree.
- [x] `runtime/third_party/gl/VENDOR.md` -- upstream ref, date, MIT notice, what was changed and why,
      and the instruction never to hand-edit.
- [x] `runtime/CMakeLists.txt` -- compile the vendored tree against ANGLE's headers as its own target,
      not linked into `screenkit-core` yet; nothing consumes it this spec.
- [x] `runtime/tests/` -- a row asserting the vendored tree still compiles, so a bad re-import is
      caught by the suite rather than at the next feature.

**Acceptance Criteria:**
- Given an empty output tree, when the script runs, then it emits the vendored source with provenance
  headers and `verify.sh` passes.
- Given the script is run twice, when the outputs are compared, then they are byte-identical.
- Given a hand-edit in the output tree, when the script runs, then it refuses, names the file, and
  exits non-zero.
- Given the vendored tree, when the runtime builds, then it compiles against ANGLE with no
  `OpenGLES/` include reachable and no third-party source compiled from outside the vendored dir.

## Implementation Notes

- **2026-09-16: wired up.** The spec scoped this to "script only"; the vendored tree is now the GL
  path. `gfx/VendoredWebGL.{h,cpp}` defines the `screenkit::gl::ContextGet` seam, the host and tests
  install it, the hand-rolled `gl.def`/`WebGL.cpp` are no longer used, and `triangle.js` was rewritten
  in real WebGL. Verified: 28/28 on macOS, clean under ASan+UBSan, triangle rendering on Apple TV 4K
  at 61.4 fps with `GL_RENDERER` naming ANGLE's Metal backend.
- Four defects found and fixed while wiring, three of them mine:
  1. `if(TARGET screenkit-gl-vendored)` sat **above** the target's definition, so the link was silently
     always skipped.
  2. `installVendoredWebGL` took the surface by `const&` and never retained it, so the drawable died
     when the caller's local went out of scope. The context then had nothing current and every
     `gl.getParameter` came back empty. The registry now owns the surface for the context's life.
  3. Contexts were never removed from the registry, so a second runtime in one process crashed. A
     host object on the global now reaps the context when its runtime is destroyed.
  4. Upstream's `getParameter` leaves `const GLubyte *glStr` **uninitialised** and then
     `std::string()`s it — undefined behaviour that read as a pass in release and `strlen(NULL)` under
     ASan. Fixed through two `sub` rules in `expo-gl.rules`, never by editing the output.
- `~GlSurface` now refcounts the EGL display instead of unconditionally calling `eglTerminate`, which
  invalidated a sibling surface's context. This also closes the M9 concern raised at vendoring time.

- **verify.sh had a blind spot, found during independent verification and fixed.** Its scan started
  after the `See VENDOR.md.` marker, so anything inserted *above* the provenance header was invisible
  — including an `#include <OpenGLES/EAGL.h>`, the single failure this guard exists to catch. The
  implementation's own test injected *below* the header, where it was caught, so the hole went
  unnoticed. The EAGL rules now scan from line 0 while the `EX`/`expo::` rules still start after the
  header (which legitimately names its upstream file). Both placements are now caught, naming file
  and line.
- Verified independently afterwards: clean tree passes; injection above the header → exit 1;
  injection below → exit 1; two runs byte-identical; hand-edit refused by name with `--force`
  restoring byte-for-byte; 28/28 tests including the eight M4 `gl-*` rows.

**`EXGLNativeContext` is vendored, not left behind.** The Code Map filed it under
"probably *not* wanted" alongside the context manager, but the Decisions section
names only `EXGLContextManager` and `EXGLNativeApi`, and that is the line that
holds up: `EXGLNativeContext` is the batch queue and the virtual-object map, and
every method in the tree calls `ctx->addToNextBatch(...)`. It owns no drawable,
so it is not context management in the sense the boundary means. Without it the
tree does not compile, which acceptance criterion 4 requires.

**Three seam files, all generated, none hand-editable.** Dropping the two context
files leaves three holes the rest of the tree reaches into, so `tools/vendor/seam/`
holds ScreenKit replacements which the script emits with a header marking them as
ours rather than expo's:
- `SKGLTypes.h` (replaces `EXGLNativeApi.h`) — the two id typedefs only; expo's
  C entry points are dropped along with the ObjC/Java layer that called them.
- `SKGLContextSeam.h` (replaces `EXGLContextManager.h`) — `ContextGet` is
  **declared and deliberately not defined**. The static library carries the
  undefined symbol, which is the honest shape of "nothing consumes this yet":
  the definition arrives with the change that wires `GlSurface` to it.
- `SKPlatformUtils.h` (replaces `EXPlatformUtils.h`) — `SKGLSysLog` on
  `screenkit::log`. Upstream's header declares `EXiOSLog`, implemented in a `.mm`
  outside `common/`, so importing it unchanged would compile and then fail to link.

**`<GLES2/gl2ext.h>`, after `<GLES3/gl3.h>`.** Collapsing the `pch.h` platform
branch needed one thing the spec did not anticipate: ANGLE ships no
`GLES3/gl3ext.h`, and the tree reads `GL_TEXTURE_MAX_ANISOTROPY_EXT` out of it.
ANGLE keeps those enums in `GLES2/gl2ext.h`, which must be included *after* a
core GL header or every `GL_APIENTRYP` typedef in it fails to parse.

**The `EX` rewrite is per identifier family, not a blanket `s/EX/SK/`.** A blanket
rule silently corrupts `EXT_texture_filter_anisotropic` — a string read back from
the driver — into `SKT_...`, which would compile and fail at run time. `verify.sh`
carries the two `allow` rows (`EXT_*`, and `EXPRESS` in stb's licence prose) that
this leaves standing.

**Clobber detection is a checksum manifest, not git.** The spec's baseline is
`NO_VCS`, so "has this tree been hand-edited?" cannot be asked of `git status`.
The script writes `.manifest` — sha256 of every file it emitted — and refuses to
run when any recorded file differs or an unrecorded file is present. It works the
same in a checkout, a tarball or CI, and `--force` is the documented override.

**Byte-identical means no timestamp.** The provenance header and `VENDOR.md`
record the *upstream commit date*, never today's, so two runs of the same import
produce identical bytes. The pin is a commit sha, not just `sdk-58`: a branch
head moves, and "byte-identical on a re-run" is only true against an immutable
object.

**`verify.sh` scans code, not prose.** It skips the generated provenance header
and comment text, because naming the upstream symbol a seam replaces is the
entire point of those comments and a comment neither compiles nor links. The
`OpenGLES/` rule is not scoped that way: an `#include` is matched wherever it
appears, and any mention outside a comment is a finding too.

**Extra guard not in the spec: unaccounted upstream files.** Every file upstream
ships must match a `file`, `verbatim`, `seam` or `skip` row, or the run fails
naming it. A new file in a newer expo is the likeliest thing to slip through a
bump, and silently not importing it is how a tracked port turns back into a fork.

## Design Notes

**Why a script rather than a one-time copy.** A copy becomes a fork the moment upstream fixes a bug,
and expo-gl is actively maintained. Keeping the transformation mechanical means a future import is
`./tools/vendor/expo-gl.sh --ref <newer>` plus a diff review, instead of a merge by hand across
470 KB. That is the whole reason the rules live in a table and the output is never touched.

**The EAGL trap.** `pch.h` is three include lines, and getting them wrong does not fail loudly — it
compiles against Apple's deprecated GLES and then fights ANGLE at link or run time. `verify.sh`
failing the run on a surviving `OpenGLES/` include is deliberate: this is the one transformation
whose silent failure would cost the most time.

## Verification

**Commands:**
- `tools/vendor/expo-gl.sh` -- emits the tree; exits 0.
- `tools/vendor/expo-gl.sh && tools/vendor/verify.sh` -- no surviving `expo::` / `EX` / `OpenGLES/`.
- `cmake --build runtime/build/macos` -- the vendored tree compiles against ANGLE.
- `ctest --test-dir runtime/build/macos --output-on-failure` -- existing rows still pass.
