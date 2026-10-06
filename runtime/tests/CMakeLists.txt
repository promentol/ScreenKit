# ---- the net rows, cross-compiled for Android -------------------------------
#
# Only the net rows: the rest of the suite drives ANGLE's offscreen surfaces and
# the macOS shell, and GlSurfaceSdl has no offscreen path (it needs a window), so
# the whole binary does not cross-compile in any useful sense. What is built here
# is the same RuntimeTests.cpp with SCREENKIT_NET_TESTS_ONLY -- a runtime with
# the prelude but no drawable -- and the GL-dependent rows, net-image among them,
# are left out of what is run.
#
# It is a library rather than an executable because a row needs three things
# only the app process has: a JavaVM whose class loader can see
# dev.screenkit.net.Transport, the INTERNET permission, and the app's own
# storage for the cookie jar. The debug APK loads it through
# dev.screenkit.net.NetTests; tools/android/android.sh test starts the same node
# fixture CTest uses, publishes its ports with `adb reverse`, and fails on a skip.
#
# No add_test() here: CTest configures for the host, and these rows run on a
# device. -Pscreenkit.netTests is what asks Gradle to build this target.
if(ANDROID)
  add_library(screenkit-net-tests SHARED RuntimeTests.cpp ../host/FramePacing.cpp)
  target_link_libraries(screenkit-net-tests PRIVATE screenkit-core)
  target_compile_options(screenkit-net-tests PRIVATE -Wall -Wextra)
  target_compile_definitions(screenkit-net-tests PRIVATE
    SCREENKIT_NET_TESTS_ONLY=1
    SCREENKIT_MANIFEST_BYTECODE_VERSION=${HERMES_BYTECODE_VERSION}
    # No ANGLE on Android; the row that reads this is not in the list that runs.
    SCREENKIT_ANGLE_HEADERS_REF="${ANGLE_HEADERS_REF}"
    # Both are replaced at run time by the device paths android.sh pushes to.
    SCREENKIT_DOM_SHIM_HBC="${SCREENKIT_DOM_SHIM_HBC}"
    SCREENKIT_NET_FIXTURE_DIR="net-fixture"
    SCREENKIT_HERMES_BUILTINS_JSON="${CMAKE_CURRENT_SOURCE_DIR}/../../packages/@screenkit/vite-plugin/src/hermes-builtins.json")
  # The prelude the rows evaluate, compiled by the same pinned hermesc the host's
  # is, and the Shaka-shaped player the media rows drive; android.sh pushes both.
  add_dependencies(screenkit-net-tests screenkit-dom-shim screenkit-shaka-script)
  target_compile_definitions(screenkit-net-tests PRIVATE SCREENKIT_SHAKA_HBC="${SCREENKIT_SHAKA_HBC}")
  return()
endif()

# ---- the net rows, cross-compiled for linux-arm64 ----------------------------
#
# The same idea as the Android target above and for the same reason: the rest of
# the suite drives ANGLE's offscreen surfaces and the macOS shell, and
# GlSurfaceSdl has no offscreen path, so only the rows that need no drawable
# cross-compile in any useful sense. What changes is the shape -- a Linux row
# needs no JavaVM and no app sandbox, so this is a plain executable that
# tools/batocera/pi.sh test copies to the device and runs over ssh, with the
# fixture on this machine reached through an ssh reverse tunnel on the same port
# numbers (which is why the certificates' 127.0.0.1 still matches).
#
# No add_test() here either: CTest configures for the host, and these rows run on
# a Raspberry Pi.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(screenkit-net-tests RuntimeTests.cpp ../host/FramePacing.cpp)
  target_link_libraries(screenkit-net-tests PRIVATE screenkit-core)
  target_compile_options(screenkit-net-tests PRIVATE -Wall -Wextra)
  target_compile_definitions(screenkit-net-tests PRIVATE
    SCREENKIT_NET_TESTS_ONLY=1
    SCREENKIT_MANIFEST_BYTECODE_VERSION=${HERMES_BYTECODE_VERSION}
    # No ANGLE on this target; the row that reads this is not one that runs here.
    SCREENKIT_ANGLE_HEADERS_REF="${ANGLE_HEADERS_REF}"
    # Both are replaced at run time by the device paths pi.sh test pushes to.
    SCREENKIT_DOM_SHIM_HBC="${SCREENKIT_DOM_SHIM_HBC}"
    SCREENKIT_NET_FIXTURE_DIR="net-fixture"
    SCREENKIT_HERMES_BUILTINS_JSON="${CMAKE_CURRENT_SOURCE_DIR}/../../packages/@screenkit/vite-plugin/src/hermes-builtins.json")
  # libhermesvm.so sits beside the binary on the device, as it does for the host.
  set_target_properties(screenkit-net-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/../lib:$ORIGIN")
  # pi.sh test pushes the prelude and the Shaka-shaped player beside the binary.
  add_dependencies(screenkit-net-tests screenkit-dom-shim screenkit-shaka-script)
  target_compile_definitions(screenkit-net-tests PRIVATE SCREENKIT_SHAKA_HBC="${SCREENKIT_SHAKA_HBC}")
  return()
endif()

# Fixtures are derived, never committed: a .hbc only means anything next to the
# Hermes it was compiled by, and that version moves with manifest.json.
if(NOT HERMESC)
  message(FATAL_ERROR
    "hermesc not found in the prebuilts cache -- the bytecode fixtures cannot be built.\n"
    "  Run: node tools/prebuilts/fetch.mjs --hermesc\n"
    "  Or configure with -DSCREENKIT_BUILD_TESTS=OFF")
endif()

find_program(NODE_EXECUTABLE node REQUIRED)

set(FIXTURE_DIR "${CMAKE_BINARY_DIR}/fixtures")
set(SCREENKIT_NET_FIXTURE_DIR "${CMAKE_BINARY_DIR}/net-fixture")
set(FIXTURE_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/block-scoping.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dom-canvas.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/hello.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/hermes-builtins.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/iframe-child.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/package-entry.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/throws.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/timers.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/triangle.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/make-fixtures.mjs"
  # The packed-entry packages are packed by the CLI's own packer.
  "${CMAKE_CURRENT_SOURCE_DIR}/../../packages/@screenkit/cli/src/pack.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../packages/@screenkit/cli/src/errors.js"
  # The fixture packages carry runtimeVersion, read from here.
  "${SCREENKIT_RUNTIME_VERSION_FILE}")

add_custom_command(
  OUTPUT "${FIXTURE_DIR}/hello.hbc"
         "${FIXTURE_DIR}/timers.hbc"
         "${FIXTURE_DIR}/throws.hbc"
         "${FIXTURE_DIR}/triangle.hbc"
         "${FIXTURE_DIR}/dom-canvas.hbc"
         "${FIXTURE_DIR}/block-scoping.hbc"
         "${FIXTURE_DIR}/hermes-builtins.hbc"
         "${FIXTURE_DIR}/block-scoping.js"
         "${FIXTURE_DIR}/version-mismatch.hbc"
         "${FIXTURE_DIR}/truncated.hbc"
         "${FIXTURE_DIR}/stub.hbc"
         "${FIXTURE_DIR}/hello.js"
         "${FIXTURE_DIR}/throws.js"
         "${FIXTURE_DIR}/garbage.js"
         "${FIXTURE_DIR}/timers.js"
         "${FIXTURE_DIR}/assets/img/tex.png"
         "${FIXTURE_DIR}/assets/img/alpha.png"
         "${FIXTURE_DIR}/assets/img/sheet.png"
         "${FIXTURE_DIR}/assets/img/large.png"
         "${FIXTURE_DIR}/triangle.js"
         "${FIXTURE_DIR}/dom-canvas.js"
         "${FIXTURE_DIR}/package-entry.hbc"
         "${FIXTURE_DIR}/packages/hello.skpkg/manifest.json"
         "${FIXTURE_DIR}/packages/bytecode-mismatch.skpkg/manifest.json"
         "${FIXTURE_DIR}/packages/runtime-too-new.skpkg/manifest.json"
         "${FIXTURE_DIR}/packages/missing-entry.skpkg/manifest.json"
         "${FIXTURE_DIR}/packages/entry-escapes.skpkg/manifest.json"
         "${FIXTURE_DIR}/packages/bad-manifest.skpkg/manifest.json"
         "${FIXTURE_DIR}/packages/no-manifest.skpkg/app.hbc"
         "${FIXTURE_DIR}/packages/format-2.skpkg/manifest.json"
         "${FIXTURE_DIR}/packages/not-an-object.skpkg/manifest.json"
         "${FIXTURE_DIR}/packages/runtime-zero.skpkg/manifest.json"
         "${FIXTURE_DIR}/packages/entry-directory.skpkg/manifest.json"
         "${FIXTURE_DIR}/iframe-child.hbc"
         "${FIXTURE_DIR}/packages/iframe-child.skpkg/manifest.json"
         "${FIXTURE_DIR}/packages/file.skpkg"
         "${FIXTURE_DIR}/packages/entry-throws.skpkg/manifest.json"
         "${FIXTURE_DIR}/packages/entry-rejects.skpkg/manifest.json"
  COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/make-fixtures.mjs"
          "${HERMESC}" "${FIXTURE_DIR}"
  DEPENDS ${FIXTURE_SOURCES}
  COMMENT "compiling test fixtures with the pinned hermesc"
  VERBATIM)

add_custom_target(screenkit-fixtures DEPENDS "${FIXTURE_DIR}/hello.hbc")

add_executable(screenkit-runtime-tests RuntimeTests.cpp ../host/FramePacing.cpp)
add_dependencies(screenkit-runtime-tests screenkit-fixtures screenkit-dom-shim screenkit-shaka-script)
# The Shaka-shaped player the media rows drive, compiled beside the prelude.
target_compile_definitions(screenkit-runtime-tests PRIVATE SCREENKIT_SHAKA_HBC="${SCREENKIT_SHAKA_HBC}")
target_link_libraries(screenkit-runtime-tests PRIVATE screenkit-core)
# The commit the ANGLE headers were checked out at (manifest angle.headers.ref).
# gl-context compares it with the one the linked libraries report.
target_compile_definitions(screenkit-runtime-tests PRIVATE
  SCREENKIT_ANGLE_HEADERS_REF="${ANGLE_HEADERS_REF}")
target_compile_options(screenkit-runtime-tests PRIVATE -Wall -Wextra)
# The prelude is named, not copied. The suite loads the same artifact the host
# does, so a test cannot pass against a stale second copy of the shim.
target_compile_definitions(screenkit-runtime-tests PRIVATE
  SCREENKIT_MANIFEST_BYTECODE_VERSION=${HERMES_BYTECODE_VERSION}
  SCREENKIT_DOM_SHIM_HBC="${SCREENKIT_DOM_SHIM_HBC}"
  # Where the networking fixture server (tests/net/server.mjs) writes its ports.
  SCREENKIT_NET_FIXTURE_DIR="${SCREENKIT_NET_FIXTURE_DIR}"
  # The ES built-ins the pinned Hermes lacks, as @screenkit/vite-plugin records
  # them for its polyfills. hermes-builtins holds the engine to that list.
  SCREENKIT_HERMES_BUILTINS_JSON="${CMAKE_CURRENT_SOURCE_DIR}/../../packages/@screenkit/vite-plugin/src/hermes-builtins.json")
set_target_properties(screenkit-runtime-tests PROPERTIES
  BUILD_RPATH   "${HERMES_RPATH}"
  INSTALL_RPATH "${HERMES_RPATH}")

# A UBSan diagnostic prints and carries on by default, so a sanitizer build would
# report a clean suite while flagging undefined behaviour. Make it fatal.
set(SCREENKIT_TEST_ENV "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
# ASan's container-overflow check only holds when every user of a libc++
# container is instrumented, and the prebuilt ANGLE archive is not. Its calls to
# template instantiations it shares with our instrumented objects (a
# vector<string>::push_back, say) resolve to the instrumented copies, which then
# read the unannotated elements ANGLE's own inlined code wrote and report a
# container-overflow inside eglInitialize -- on every GL row, whichever of our
# files happened to instantiate the template. That is ASan's documented false
# positive for mixed instrumentation, so this one check is off; heap overflows,
# use-after-free and UBSan still halt the run.
if(SCREENKIT_SANITIZE MATCHES "address")
  string(APPEND SCREENKIT_TEST_ENV ";ASAN_OPTIONS=detect_container_overflow=0")
endif()

# One CTest entry per matrix row, so a failure names the row rather than "tests".
set(SCREENKIT_TEST_CASES
  valid-bytecode
  source-fallback
  block-scoping
  float-bit-patterns
  garbage-source
  version-mismatch
  corrupt-bytecode
  truncated-header
  js-throws
  teardown-race
  evaluate-cancelled-by-shutdown
  evaluate-from-js-thread
  callback-throws
  console-levels
  log-sink-swap
  bytecode-version-pin
  timers-ordering
  timers-cancel
  timers-freeze
  # The JSI object model: SharedObject lifetime, SharedRef, EventEmitter listener
  # isolation and LazyObject module construction.
  object-model-gc
  object-model-release
  object-model-listener-isolation
  object-model-lazy-module
  missing-bundle
  # M4. Each of these runs against a real ANGLE context on an offscreen
  # pbuffer, so they exercise the same entry points the windowed host does --
  # only the drawable differs.
  gl-context
  gl-shader-error
  gl-link-error
  gl-typed-array
  gl-buffer-data
  gl-drawing-buffer-resize
  gl-error-passthrough
  gl-triangle
  gl-teardown
  gl-shared-context
  gl-pixel-store
  gl-frame-stats
  gl-typed-array-shadowed
  frame-pacing-interval
  frame-pacing-deadline
  gl-present-without-shim
  # The DOM shim. One row per line of the spec's matrix, each against the real
  # prelude bytecode and a real ANGLE context -- the shim hands out the context
  # the host made, so a mocked `gl` would test the wrong thing entirely.
  dom-canvas-create
  dom-gl-handoff
  dom-webgl2
  dom-dimensions
  dom-drawing-buffer-shape
  dom-dimension-write
  dom-second-canvas
  dom-unknown-element
  dom-loader-tolerance
  dom-environment
  dom-events
  dom-encoding
  dom-asset-confinement
  dom-texture-chain
  dom-fetch
  dom-image-element
  dom-fonts
  dom-identity-and-absence
  dom-coverage
  dom-root-lookup
  dom-unsupported-context
  dom-canvas-fixture
  dom-present-frame
  input-keyboard-map
  input-gamepad-map
  input-router-dispatch
  input-event-loop
  # The W3C Gamepad API over SDL3 (spec-web-gamepad-api-sdl3.md): one row per
  # matrix line, each driving a virtual SDL gamepad through InputRouter into the
  # prelude, and Phaser's gamepad plugin reduced to the calls it makes.
  gamepad-startup
  gamepad-button
  gamepad-trigger
  gamepad-stick
  gamepad-slots
  gamepad-disconnect
  gamepad-paused
  gamepad-no-subsystem
  gamepad-app-writes
  gamepad-claim
  gamepad-rumble
  gamepad-phaser-poll
  dom-window-is-global
  dom-navigator
  dom-event-target
  dom-location-hash
  dom-bounding-rect
  dom-url-search-params-iteration
  gl-premultiplied-alpha
  promise-rejection-logged
  # M5 finish: a JS-side fatal the host can see, and what the pinned Hermes lacks.
  report-failure
  hermes-builtins
  # M6: the element tree, selectors, events along the tree, and style.
  dom-tree-move
  dom-tree-cycle
  dom-tree-operations
  dom-document-tree
  dom-attributes
  dom-selectors
  dom-selector-syntax
  dom-event-propagation
  dom-key-target
  dom-window-close
  dom-style-declaration
  dom-canvas-style
  dom-canvas-style-attribute
  dom-event-handlers
  dom-mutation-observer
  dom-image-bitmap-crop
  dom-image-decode-async
  dom-xml-parser
  dom-canvas-2d
  dom-canvas-text
  dom-document-ready
  # HTMLMediaElement's state machine against a scripted player (no media).
  media-element-model
  # M9: `<iframe>` instances. One row per line of the spec's matrix, each
  # driving a real launcher page with a real child package over a real ANGLE
  # context -- the share group, the freeze gate and the composite are none of
  # them visible from JS alone.
  iframe-embed
  iframe-message
  iframe-lifecycle
  iframe-focus
  iframe-remove
  iframe-bad-src
  iframe-nested
  iframe-sandbox
  iframe-two
  iframe-shutdown
  iframe-none-costs
  iframe-child-fails
  iframe-no-compositor
  iframe-input
  # M6 finish: composited canvases. Every canvas its own GL context and its own
  # layer, composited by the same list an instance's layer goes through -- so
  # the rows that matter are pixel reads out of the page's own framebuffer.
  canvas-isolation
  canvas-placed
  canvas-composite
  canvas-idle
  canvas-remove
  canvas-teardown
  canvas-over-iframe
  canvas-over-video
  canvas-2d-no-layer
  canvas-refused)

foreach(case IN LISTS SCREENKIT_TEST_CASES)
  add_test(NAME ${case}
           COMMAND screenkit-runtime-tests ${case} "${FIXTURE_DIR}")
  set_tests_properties(${case} PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77
                       ENVIRONMENT "${SCREENKIT_TEST_ENV}")
  # An `iframe-*` row starts a second runtime, a second JS thread and a second
  # GL context per instance, and `iframe-remove` does that ten times over. Under
  # a sanitizer that is minutes, not the DOM rows' seconds.
  if(case MATCHES "^iframe-" OR case STREQUAL "canvas-over-iframe")
    set_tests_properties(${case} PROPERTIES TIMEOUT 300)
  endif()
endforeach()

# The Unavailable client (core/src/net/NetServiceUnavailable.cpp): what a port
# with no HTTP client of its own builds. No target this repo ships is such a
# platform, so this is the only thing that compiles or runs it. Its own binary
# because it *replaces* this platform's client: the two symbols the seam links
# by are defined here, and the linker never pulls the real client out of
# libscreenkit-core.
add_executable(screenkit-net-unavailable-tests
  NetUnavailableTests.cpp
  ../core/src/net/NetServiceUnavailable.cpp)
target_link_libraries(screenkit-net-unavailable-tests PRIVATE screenkit-core)
target_compile_options(screenkit-net-unavailable-tests PRIVATE -Wall -Wextra)
add_test(NAME net-unavailable COMMAND screenkit-net-unavailable-tests)
set_tests_properties(net-unavailable PROPERTIES TIMEOUT 30)

# The Unavailable player (core/src/media/MediaPlayerUnavailable.cpp), the same
# way: what a port with no player of its own builds, and nothing else runs it.
add_executable(screenkit-media-unavailable-tests
  MediaUnavailableTests.cpp
  ../core/src/media/MediaPlayerUnavailable.cpp)
target_link_libraries(screenkit-media-unavailable-tests PRIVATE screenkit-core)
target_compile_options(screenkit-media-unavailable-tests PRIVATE -Wall -Wextra)
set_target_properties(screenkit-media-unavailable-tests PROPERTIES
  BUILD_RPATH   "${HERMES_RPATH}"
  INSTALL_RPATH "${HERMES_RPATH}")
add_test(NAME media-unavailable COMMAND screenkit-media-unavailable-tests)
set_tests_properties(media-unavailable PROPERTIES TIMEOUT 30 ENVIRONMENT "${SCREENKIT_TEST_ENV}")

# Runtime networking (spec-runtime-networking.md): one row per matrix line, each
# against the local fixture server below -- plain HTTP, HTTPS behind a test CA
# generated at start and trusted through RuntimeConfig::testTlsAnchors, and three
# certificates the OS must refuse. Every address is loopback: no row reaches the
# public internet. CTest starts the server before the first row that needs it and
# stops it after the last, including when only some rows are selected with -R.
set(SCREENKIT_NET_TEST_CASES
  net-http-get
  net-https
  net-request-body
  net-streaming-request
  net-streaming-response
  net-http-error-status
  net-redirect
  net-encoded-body
  net-abort-timeout
  net-flow-control
  net-unreachable
  net-cookies
  net-websocket
  net-eventsource
  net-image
  net-shutdown-idle
  net-seam-contract)

add_test(NAME net-fixture-start
         COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/net/start-server.sh" "${NODE_EXECUTABLE}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/net/server.mjs" "${SCREENKIT_NET_FIXTURE_DIR}")
set_tests_properties(net-fixture-start PROPERTIES FIXTURES_SETUP net-server TIMEOUT 60)
add_test(NAME net-fixture-stop
         COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/net/stop-server.sh" "${SCREENKIT_NET_FIXTURE_DIR}")
set_tests_properties(net-fixture-stop PROPERTIES FIXTURES_CLEANUP net-server TIMEOUT 60)

# The video player (spec-video-player.md): one row per line of its matrix,
# against media the same fixture server generates with this machine's ffmpeg at
# start (net/media.mjs) and serves on loopback, with a fake licence server.
list(APPEND SCREENKIT_NET_TEST_CASES
  media-seam
  media-element
  media-hls
  media-mp4
  media-dash
  media-live
  media-seek
  media-ended
  media-tracks
  media-clearkey
  media-licence
  media-licence-exchange
  media-bad-url
  media-unplayable
  media-interrupted
  media-remove
  media-two-players
  media-runtime-pause
  media-shutdown
  media-collected)

foreach(case IN LISTS SCREENKIT_NET_TEST_CASES)
  add_test(NAME ${case}
           COMMAND screenkit-runtime-tests ${case} "${FIXTURE_DIR}")
  set_tests_properties(${case} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
                       FIXTURES_REQUIRED net-server
                       ENVIRONMENT "${SCREENKIT_TEST_ENV}")
endforeach()

# Leak checks. Apple's ASan has no LeakSanitizer, so on Darwin the teardown rows
# run again under leaks(1) in the plain build -- leaks cannot read a process
# whose malloc ASan has replaced. One row per kind of teardown: GL objects left
# live, a runtime torn down mid-evaluation, GC-owned native objects, the texture
# path, and open network connections and sockets.
find_program(LEAKS_EXECUTABLE leaks)
if(APPLE AND NOT SCREENKIT_SANITIZE AND LEAKS_EXECUTABLE)
  foreach(case gl-teardown teardown-race object-model-gc dom-texture-chain
               net-shutdown-idle net-websocket media-shutdown)
    add_test(NAME leaks-${case}
             COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/leaks-row.sh"
                     "$<TARGET_FILE:screenkit-runtime-tests>" ${case} "${FIXTURE_DIR}")
    set_tests_properties(leaks-${case} PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
    if(case MATCHES "^(net|media)-")
      set_tests_properties(leaks-${case} PROPERTIES FIXTURES_REQUIRED net-server)
    endif()
  endforeach()
endif()

# console output as the platform log shows it: the real os_log sink, read back
# out of the unified log with OSLogStore (see OsLogTests.mm). macOS only -- the
# tests are not built for tvOS, and OSLogStore's process scope needs macOS 12.
if(APPLE)
  add_executable(screenkit-oslog-tests OsLogTests.mm ../apple/PlatformLog.mm)
  target_link_libraries(screenkit-oslog-tests PRIVATE
    screenkit-core "-framework Foundation" "-framework OSLog")
  target_compile_options(screenkit-oslog-tests PRIVATE -Wall -Wextra -fobjc-arc)
  set_target_properties(screenkit-oslog-tests PROPERTIES
    BUILD_RPATH   "${HERMES_RPATH}"
    INSTALL_RPATH "${HERMES_RPATH}")
  add_test(NAME console-os-log COMMAND screenkit-oslog-tests)
  set_tests_properties(console-os-log PROPERTIES TIMEOUT 60 ENVIRONMENT "${SCREENKIT_TEST_ENV}")
endif()

# The vendored expo-gl tree. A bad re-import has to be caught by the suite
# rather than at the next feature, so this row does the two things that can go
# wrong: it rebuilds the target (ninja recompiles whatever the import touched,
# so a tree that no longer compiles fails here even though ctest itself does not
# build), and it re-runs the post-pass that proves the rewrite happened -- a
# surviving OpenGLES/ include compiles perfectly and only fails much later.
if(TARGET screenkit-gl-vendored)
  add_test(NAME gl-vendor COMMAND sh -c
    "\"$1\" --build \"$2\" --target screenkit-gl-vendored && \"$3\" \"$4\""
    sh "${CMAKE_COMMAND}" "${CMAKE_BINARY_DIR}"
       "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vendor/verify.sh"
       "${SCREENKIT_GL_VENDOR_DIR}")
  set_tests_properties(gl-vendor PROPERTIES TIMEOUT 300 ENVIRONMENT "${SCREENKIT_TEST_ENV}")
endif()

# The vendored cpp-httplib tree. Its sha256 pin is checked when the *Linux*
# build configures, and nothing else compiles that header -- so without this row
# a hand-edited httplib.h passes every ctest on this machine and is only caught
# on a device build. The check is the same one the Linux configure runs.
add_test(NAME httplib-vendor
         COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vendor/httplib.sh" --check)
set_tests_properties(httplib-vendor PROPERTIES TIMEOUT 60 ENVIRONMENT "${SCREENKIT_TEST_ENV}")

# Smoke tests of the shipped shell, not just the library.
if(TARGET screenkit-host)
  # Exit status AND output. PASS_REGULAR_EXPRESSION alone would be worse than
  # nothing here: CTest ignores the exit code once it is set, so a host that
  # printed the line and then exited 65 would still pass.
  add_test(NAME host-runs-hello COMMAND sh -c
    "out=$(\"$1\" \"$2\" 2>&1); rc=$?; printf '%s\\n' \"$out\"; [ $rc -eq 0 ] || exit $rc; printf '%s' \"$out\" | grep -q 'hello from hermes'"
    sh "$<TARGET_FILE:screenkit-host>" "${FIXTURE_DIR}/hello.hbc")
  set_tests_properties(host-runs-hello PROPERTIES
    TIMEOUT 60 ENVIRONMENT "${SCREENKIT_TEST_ENV}")

  # A bundle that throws must make the host exit non-zero, not merely log.
  add_test(NAME host-refuses-throws
           COMMAND screenkit-host "${FIXTURE_DIR}/throws.hbc")
  set_tests_properties(host-refuses-throws PROPERTIES
    TIMEOUT 60 WILL_FAIL TRUE ENVIRONMENT "${SCREENKIT_TEST_ENV}")

  # .skpkg packages (M5), from make-fixtures.mjs. One that runs, and one row per
  # way the host refuses a package. A refusal is exit 65 (EX_DATAERR, the same
  # code as a refused bundle) with an error naming the file and, for a version,
  # both sides -- and never the entry's own line, which is how "nothing was
  # evaluated" is checked: every refused package still carries a runnable entry.
  set(_host_row "${CMAKE_CURRENT_SOURCE_DIR}/host-row.sh")
  set(_packages "${FIXTURE_DIR}/packages")
  set(_entry_ran "package entry ran")
  math(EXPR _bytecode_next "${HERMES_BYTECODE_VERSION} + 1")
  math(EXPR _runtime_next "${SCREENKIT_RUNTIME_VERSION} + 1")

  add_test(NAME host-runs-package COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           "${_packages}/hello.skpkg" 0
           "${_entry_ran}; fonts/probe.txt says: inside hello.skpkg")

  add_test(NAME host-refuses-package-bytecode COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           "${_packages}/bytecode-mismatch.skpkg" 65
           "Hermes bytecode version mismatch in \"${_packages}/bytecode-mismatch.skpkg/manifest.json\""
           "package is version ${_bytecode_next}, this runtime accepts version ${HERMES_BYTECODE_VERSION}"
           "!${_entry_ran}")

  add_test(NAME host-refuses-package-runtime COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           "${_packages}/runtime-too-new.skpkg" 65
           "runtime version mismatch in \"${_packages}/runtime-too-new.skpkg/manifest.json\""
           "package needs runtimeVersion ${_runtime_next}, this runtime supports runtimeVersion ${SCREENKIT_RUNTIME_VERSION}"
           "!${_entry_ran}")

  add_test(NAME host-refuses-package-missing-entry COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           "${_packages}/missing-entry.skpkg" 65
           "package entry \"app.hbc\" named by \"${_packages}/missing-entry.skpkg/manifest.json\" does not exist"
           "!${_entry_ran}")

  add_test(NAME host-refuses-package-entry-escape COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           "${_packages}/entry-escapes.skpkg" 65
           "resolves outside the package"
           "!hello from hermes")

  add_test(NAME host-refuses-package-bad-manifest COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           "${_packages}/bad-manifest.skpkg" 65
           "package manifest \"${_packages}/bad-manifest.skpkg/manifest.json\" is not valid JSON"
           "!${_entry_ran}")

  add_test(NAME host-refuses-package-no-manifest COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           "${_packages}/no-manifest.skpkg" 65
           "cannot open package manifest \"${_packages}/no-manifest.skpkg/manifest.json\""
           "!${_entry_ran}")

  add_test(NAME host-refuses-package-format COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           "${_packages}/format-2.skpkg" 65
           "package manifest \"${_packages}/format-2.skpkg/manifest.json\" is format 2, this runtime reads format 1"
           "!${_entry_ran}")

  add_test(NAME host-refuses-package-not-object COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           "${_packages}/not-an-object.skpkg" 65
           "package manifest \"${_packages}/not-an-object.skpkg/manifest.json\" is not a JSON object"
           "!${_entry_ran}")

  add_test(NAME host-refuses-package-runtime-zero COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           "${_packages}/runtime-zero.skpkg" 65
           "runtime version mismatch in \"${_packages}/runtime-zero.skpkg/manifest.json\": package needs runtimeVersion 0"
           "!${_entry_ran}")

  add_test(NAME host-refuses-package-entry-directory COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           "${_packages}/entry-directory.skpkg" 65
           "package entry \"fonts\" named by \"${_packages}/entry-directory.skpkg/manifest.json\" is not a regular file"
           "!${_entry_ran}")

  # A .skpkg path that is not a directory is refused by name, not compiled as
  # source -- whether it is a file (runnable bytecode here) or nothing at all.
  add_test(NAME host-refuses-package-file COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           "${_packages}/file.skpkg" 65
           "\"${_packages}/file.skpkg\" is not a package: a .skpkg is the directory `screenkit bundle` writes, and this is a file"
           "!${_entry_ran}")

  add_test(NAME host-refuses-package-absent COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           "${_packages}/absent.skpkg" 65
           "\"${_packages}/absent.skpkg\" is not a package: a .skpkg is the directory `screenkit bundle` writes, and nothing is there")

  # A package whose entry module fails is a failed run, not a clean exit: the
  # packed wrapper reports the rejection and the host exits 65 at its next loop
  # iteration (headless and --window; tvOS ends with SDL_APP_FAILURE, status 1). entry-rejects keeps an interval running, so waiting for idle
  # would sit out the 30 s deadline -- the --within bound is what rules that out.
  add_test(NAME host-refuses-entry-throws COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           --within 10 "${_packages}/entry-throws.skpkg" 65
           "entry started"
           "screenkit bundle: entry \"/assets/index-legacy-A.js\" failed: Error: boom from the entry"
           "the app reported a fatal failure; exiting with 65"
           "!\" ready")

  add_test(NAME host-refuses-entry-rejects COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           --within 5 "${_packages}/entry-rejects.skpkg" 65
           "entry started"
           "late boom from the entry"
           "the app reported a fatal failure; exiting with 65"
           "!still had work pending" "!\" ready")

  add_test(NAME host-window-refuses-entry-throws COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           --within 15 --window "${_packages}/entry-throws.skpkg" 65
           "DOM shim ready"
           "entry started"
           "screenkit bundle: entry \"/assets/index-legacy-A.js\" failed: Error: boom from the entry"
           "the app reported a fatal failure; exiting with 65"
           "!\" ready")

  add_test(NAME host-window-refuses-entry-rejects COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           --within 15 --window "${_packages}/entry-rejects.skpkg" 65
           "DOM shim ready"
           "late boom from the entry"
           "the app reported a fatal failure; exiting with 65"
           "!\" ready")

  # --window: the gate runs before a window is opened, so a refused package
  # never reaches the graphics bootstrap...
  add_test(NAME host-window-refuses-package COMMAND sh "${_host_row}" "$<TARGET_FILE:screenkit-host>"
           --window "${_packages}/bytecode-mismatch.skpkg" 65
           "package is version ${_bytecode_next}, this runtime accepts version ${HERMES_BYTECODE_VERSION}"
           "!drawable ready" "!DOM shim ready" "!${_entry_ran}")

  # ...and an accepted one runs with the prelude loaded and the package as its
  # asset root. Waited on by its line; hello.skpkg schedules nothing after it,
  # and the window must still be open 3 s later and close cleanly on a quit.
  add_test(NAME host-window-runs-package COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/host-window-row.sh"
           "$<TARGET_FILE:screenkit-host>" "${_packages}/hello.skpkg"
           "${_entry_ran}; fonts/probe.txt says: inside hello.skpkg" 30 3)

  # Retired with the cookie jar: `host-storage-directory` and
  # `host-window-storage-directory` checked that each macOS run mode hands the
  # runtime a storage directory, and the only way they could see it was the
  # `cookies.txt` the jar wrote there. Cookies live in the platform store now
  # (spec-platform-http-clients.md), so there was nothing left for the rows to
  # observe -- and `RuntimeConfig::storageDirectory` itself is gone with them.

  set_tests_properties(
    host-runs-package host-refuses-package-bytecode host-refuses-package-runtime
    host-refuses-package-missing-entry host-refuses-package-entry-escape
    host-refuses-package-bad-manifest host-refuses-package-no-manifest
    host-refuses-package-format host-refuses-package-not-object
    host-refuses-package-runtime-zero host-refuses-package-entry-directory
    host-refuses-package-file host-refuses-package-absent
    host-window-refuses-package host-window-runs-package
    host-refuses-entry-throws host-refuses-entry-rejects host-window-refuses-entry-throws
    host-window-refuses-entry-rejects
    PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 ENVIRONMENT "${SCREENKIT_TEST_ENV}")

  # The CLI suite against this host: packages `screenkit bundle` writes must
  # pass this host's gate and run. Without it, a manifest the CLI and the host
  # disagree about stays green on both sides.
  add_test(NAME cli-bundle
           COMMAND "${NODE_EXECUTABLE}" --test "${CMAKE_SOURCE_DIR}/../packages/@screenkit/cli/test")
  set_tests_properties(cli-bundle PROPERTIES TIMEOUT 300
    ENVIRONMENT "${SCREENKIT_TEST_ENV};SCREENKIT_HOST=$<TARGET_FILE:screenkit-host>")
endif()

# The shim is one scope: a name declared twice at its top level replaces the first.
add_test(NAME dom-shim-names
         COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check-shim-names.mjs"
                 "${CMAKE_CURRENT_SOURCE_DIR}/../js/dom-shim.js")

# The Vite plugin's own suite: plugin order, the forced output shape, the
# duplicate-legacy check, where plugin-legacy is found, and the Lightning fixes.
add_test(NAME vite-plugin
         COMMAND "${NODE_EXECUTABLE}" --test "${CMAKE_SOURCE_DIR}/../packages/@screenkit/vite-plugin/test")
set_tests_properties(vite-plugin PROPERTIES TIMEOUT 120)
