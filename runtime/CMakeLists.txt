# ScreenKit native runtime.
#
# Ninja only -- never the Xcode generator (Architecture.md 11): the Xcode
# generator spawns a full xcodebuild per configure-time probe, which cost the
# SDL3 PoC 12+ minutes against 41s under Ninja.
cmake_minimum_required(VERSION 3.24)
project(screenkit LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJC)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ---- target key -------------------------------------------------------------
# One vocabulary shared with tools/prebuilts: apple-macos-arm64,
# apple-tvos-simulator-arm64, android-arm64, and so on.
if(NOT DEFINED SCREENKIT_TARGET)
  if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    # Gradle configures one tree per ABI (runtime/android/app/build.gradle).
    # arm64-v8a for Android TV, armeabi-v7a for Fire TV sticks' 32-bit userland.
    if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
      set(SCREENKIT_TARGET "android-arm64")
    elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
      set(SCREENKIT_TARGET "android-arm32")
    else()
      message(FATAL_ERROR
        "no prebuilts target for Android ABI ${CMAKE_ANDROID_ARCH_ABI}: arm64-v8a and armeabi-v7a are the targets")
    endif()
  elseif(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    # CMake expands CMAKE_OSX_SYSROOT to the full SDK path, so match case-
    # insensitively: ".../AppleTVSimulator.platform/..." must not read as a
    # device build, or the link fails with "built for 'tvOS'".
    string(TOLOWER "${CMAKE_OSX_SYSROOT}" _sysroot_lower)
    if(_sysroot_lower MATCHES "simulator")
      set(SCREENKIT_TARGET "apple-tvos-simulator-arm64")
    else()
      set(SCREENKIT_TARGET "apple-tvos-device-arm64")
    endif()
  elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    string(TOLOWER "${CMAKE_OSX_SYSROOT}" _sysroot_lower)
    if(_sysroot_lower MATCHES "simulator")
      set(SCREENKIT_TARGET "apple-ios-simulator-arm64")
    else()
      set(SCREENKIT_TARGET "apple-ios-device-arm64")
    endif()
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
      set(SCREENKIT_TARGET "apple-macos-x86_64")
    else()
      set(SCREENKIT_TARGET "apple-macos-arm64")
    endif()
  elseif(APPLE)
    # watchOS, visionOS, anything new. Falling through to the macOS branch would
    # silently link the macOS framework into a build that cannot load it.
    message(FATAL_ERROR
      "no prebuilts target mapping for Apple system \"${CMAKE_SYSTEM_NAME}\". "
      "Pass -DSCREENKIT_TARGET=<key> explicitly; see node tools/prebuilts/fetch.mjs --list.")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
      set(SCREENKIT_TARGET "linux-arm64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
      set(SCREENKIT_TARGET "linux-x86_64")
    else()
      message(FATAL_ERROR "no prebuilts target for Linux on ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
  else()
    message(FATAL_ERROR
      "no prebuilts target for ${CMAKE_SYSTEM_NAME}: Apple platforms, Android and Linux are the targets today.")
  endif()
endif()
set(SCREENKIT_TARGET "${SCREENKIT_TARGET}" CACHE STRING "tools/prebuilts target key")

# ---- runtime version ---------------------------------------------------------
# runtime/VERSION is the single runtimeVersion: `screenkit bundle` writes it into
# every package manifest, and the host is compiled with it as the newest package
# it will run. One file, read by both, so they cannot drift.
set(SCREENKIT_RUNTIME_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(READ "${SCREENKIT_RUNTIME_VERSION_FILE}" SCREENKIT_RUNTIME_VERSION)
string(STRIP "${SCREENKIT_RUNTIME_VERSION}" SCREENKIT_RUNTIME_VERSION)
# 1 to 4294967295: the host compiles it into a uint32, and `screenkit bundle`
# enforces the same range, so a manifest can never carry a number the gate would
# truncate. The digit cap keeps the comparison inside CMake's integer range.
if(NOT SCREENKIT_RUNTIME_VERSION MATCHES "^[1-9][0-9]?[0-9]?[0-9]?[0-9]?[0-9]?[0-9]?[0-9]?[0-9]?[0-9]?$"
   OR SCREENKIT_RUNTIME_VERSION GREATER 4294967295)
  message(FATAL_ERROR "${SCREENKIT_RUNTIME_VERSION_FILE} must hold one integer from 1 to 4294967295, "
                      "found \"${SCREENKIT_RUNTIME_VERSION}\"")
endif()
# A bumped VERSION reconfigures, so the compiled-in gate follows the file.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SCREENKIT_RUNTIME_VERSION_FILE}")
message(STATUS "runtimeVersion ${SCREENKIT_RUNTIME_VERSION}")

option(SCREENKIT_BUILD_TESTS "Build runtime/tests" ON)
set(SCREENKIT_SANITIZE "" CACHE STRING
    "Sanitizers to build with, e.g. address or address,undefined")

# The JavaScript engine behind JSI (core/src/engine/Engine.h). Hermes everywhere;
# on Linux SpiderMonkey instead, which may JIT (EMBEDDED_LINUX_EXPERIMENTS.md).
set(SCREENKIT_ENGINE "hermes" CACHE STRING "The JavaScript engine: hermes, or spidermonkey (Linux)")
set_property(CACHE SCREENKIT_ENGINE PROPERTY STRINGS hermes spidermonkey)
if(SCREENKIT_ENGINE STREQUAL "spidermonkey")
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
    message(FATAL_ERROR "SCREENKIT_ENGINE=spidermonkey is a Linux engine; ${SCREENKIT_TARGET} runs Hermes")
  endif()
  if(SCREENKIT_BUILD_TESTS)
    message(FATAL_ERROR "runtime/tests run on Hermes; configure SpiderMonkey with -DSCREENKIT_BUILD_TESTS=OFF "
                        "(its own suite is runtime/tests/jsi/run.sh)")
  endif()
  include(cmake/SpiderMonkey.cmake)
  set(SCREENKIT_JSI_ENGINE screenkit-spidermonkey)
elseif(SCREENKIT_ENGINE STREQUAL "hermes")
  include(cmake/HermesPrebuilt.cmake)
  set(SCREENKIT_JSI_ENGINE hermes::vm)
else()
  message(FATAL_ERROR "SCREENKIT_ENGINE is \"${SCREENKIT_ENGINE}\": hermes or spidermonkey")
endif()
if(APPLE)
  include(cmake/Sdl3Prebuilt.cmake)
  include(cmake/Sdl3TtfPrebuilt.cmake)
  include(cmake/AnglePrebuilt.cmake)
  # The GLES the vendored WebGL and the GL surface call: ANGLE over Metal.
  set(SCREENKIT_GLES angle::angle)
elseif(ANDROID)
  include(cmake/AndroidSystem.cmake)
  # The device's GLES, through SDL's EGL context, as on Linux: there is no ANGLE
  # to link on Android (Architecture.md 9).
  set(SCREENKIT_GLES gles::gles)
else()
  include(cmake/LinuxSystem.cmake)
  # The system's GLES, through SDL's EGL context (core/src/gfx/GlSurfaceSdl.cpp).
  set(SCREENKIT_GLES gles::gles)
endif()

# ---- sanitizers -------------------------------------------------------------
if(SCREENKIT_SANITIZE)
  message(STATUS "sanitizers: ${SCREENKIT_SANITIZE}")
  add_compile_options(-fsanitize=${SCREENKIT_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${SCREENKIT_SANITIZE})
endif()

# ---- core -------------------------------------------------------------------
add_library(screenkit-core STATIC
  core/src/Log.cpp
  core/src/bindings/Canvas.cpp
  core/src/bindings/Console.cpp
  core/src/bindings/Gamepads.cpp
  core/src/bindings/Timers.cpp
  core/src/bindings/HostIO.cpp
  core/src/bindings/Instance.cpp
  core/src/bindings/ImageDecode.cpp
  core/src/bindings/Net.cpp
  core/src/bindings/Text.cpp
  core/src/bindings/WebGL.cpp
  core/src/bundle/Package.cpp
  core/src/compositor/Compositor.cpp
  core/src/instance/Instance.cpp
  core/src/instance/StructuredClone.cpp
  core/src/gfx/FrameStats.cpp
  core/src/gfx/FrameCapture.cpp
  core/src/gfx/GlSurfaceLayers.cpp
  core/src/gfx/VendoredWebGL.cpp
  core/src/input/Gamepads.cpp
  core/src/input/Input.cpp
  core/src/viewport/Viewport.cpp
  core/src/hermes/BytecodeLoader.cpp
  core/src/hermes/HermesHost.cpp
  core/src/hermes/RuntimeImpl.cpp
  core/src/loop/EventLoop.cpp
  core/src/loop/TimerRegistry.cpp
  core/src/loop/WorkQueue.cpp
  core/src/jsi/EventEmitter.cpp
  core/src/jsi/JSIUtils.cpp
  core/src/jsi/LazyObject.cpp
  core/src/jsi/NativeModule.cpp
  core/src/jsi/SharedObject.cpp
  core/src/jsi/SharedRef.cpp
  core/src/net/Bytes.cpp
  core/src/net/IoQueue.cpp
  core/src/net/Url.cpp
  core/src/text/FontLibrary.cpp)

# core/src so a platform shell can include the private gfx headers it has to
# drive (GlSurface, FrameStats). core/include stays the embedder API.
target_include_directories(screenkit-core PUBLIC core/include core/src)
# runtime/VERSION: the package gate lives in core now (core/src/bundle/Package.cpp),
# because an <iframe> instance gates its own package with the same code the host
# gates the app with.
target_compile_definitions(screenkit-core PRIVATE
  SCREENKIT_RUNTIME_VERSION=${SCREENKIT_RUNTIME_VERSION})
target_link_libraries(screenkit-core PUBLIC ${SCREENKIT_JSI_ENGINE} sdl3::sdl3 sdl3::ttf ${SCREENKIT_GLES})
if(SCREENKIT_ENGINE STREQUAL "spidermonkey")
  target_sources(screenkit-core PRIVATE core/src/spidermonkey/SpiderMonkeyEngine.cpp)
  target_compile_definitions(screenkit-core PRIVATE SCREENKIT_ENGINE_SPIDERMONKEY=1)
else()
  target_sources(screenkit-core PRIVATE core/src/hermes/HermesEngine.cpp)
endif()
if(APPLE)
  target_sources(screenkit-core PRIVATE
    core/src/gfx/GlSurface.cpp
    core/src/gfx/MetalLayerSize.mm
    core/src/gfx/angle_stubs.mm)
else()
  target_sources(screenkit-core PRIVATE core/src/gfx/GlSurfaceSdl.cpp)
endif()
target_compile_options(screenkit-core PRIVATE -Wall -Wextra)
set_target_properties(screenkit-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- the HTTP client -------------------------------------------------------------
# Each platform's own client behind the NetService seam, and no networking, TLS
# or protocol code of ours (Architecture.md 10) -- what React Native does. On
# Apple that is NSURLSession, with Security.framework for the test suite's trust
# anchors; on Android, OkHttp over JNI. Linux has no platform client to point at
# and gets a vendored cpp-httplib over the image's own OpenSSL instead, which is
# the one place that rule bends -- the TLS and the CA roots are still the
# image's. A platform with none of the three builds the Unavailable client,
# where every request fails cleanly.
if(APPLE)
  target_sources(screenkit-core PRIVATE core/src/net/NetServiceApple.mm)
  # ARC: the session, its tasks and the blocks capturing them are released by
  # the compiler, not by hand.
  set_source_files_properties(core/src/net/NetServiceApple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(screenkit-core PUBLIC "-framework Security" "-framework Foundation")
elseif(ANDROID)
  # OkHttp through runtime/android/app/src/main/java/dev/screenkit/net/HttpClient.java.
  # The NDK publishes no HTTP or TLS library, and the platform's own client is
  # the one the OS maintains. jni.h is in the sysroot.
  target_sources(screenkit-core PRIVATE core/src/net/NetServiceAndroid.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # cpp-httplib (third_party/httplib, pinned by sha256) plus the cookie jar this
  # platform needs because it has no store to point at. OpenSSL is resolved and
  # required by cmake/LinuxSystem.cmake, so an image without it fails there.
  include(cmake/HttplibVendored.cmake)
  target_sources(screenkit-core PRIVATE
    core/src/net/NetServiceLinux.cpp
    core/src/net/LinuxCookieJar.cpp)
  # Both PRIVATE: NetServiceLinux.cpp is the only file that includes either
  # header, and nothing above the seam links OpenSSL or reaches for httplib.h.
  target_link_libraries(screenkit-core PRIVATE openssl::openssl httplib::httplib)
  # On the target, not in the .cpp: this changes cpp-httplib's class layouts,
  # and a second translation unit that ever included the header without it
  # would be an ODR violation that links and then misbehaves. No
  # CPPHTTPLIB_ZLIB_SUPPORT: response bodies are decoded by NetServiceLinux.cpp
  # itself (cpp-httplib's decoder cannot read raw deflate), so the library's
  # compression support would be code nothing calls.
  target_compile_definitions(screenkit-core PRIVATE
    CPPHTTPLIB_OPENSSL_SUPPORT=1)
else()
  target_sources(screenkit-core PRIVATE core/src/net/NetServiceUnavailable.cpp)
endif()

# ---- the media player -------------------------------------------------------------
# One <video> element's player behind the MediaPlayer seam (core/src/media/
# MediaPlayer.h), each platform's own -- what react-native-video does: AVPlayer
# on Apple, Media3 ExoPlayer on Android, the image's libvlc on Linux, and
# the Unavailable one elsewhere, whose every load fails cleanly. Video is
# composited beneath the app's drawable by the platform and never passes
# through the runtime's GL. `__screenkit.media` (bindings/Media.cpp) is the one
# caller; the Unavailable player is also built on its own for media-unavailable
# (tests/CMakeLists.txt).
target_sources(screenkit-core PRIVATE
  core/src/bindings/Media.cpp
  core/src/media/VideoHost.cpp)
if(APPLE)
  # [apple-media]
  # AVPlayer (core/src/media/MediaPlayerApple.mm): HLS and progressive, ABR,
  # media selection, legible output for cues, FairPlay through
  # AVContentKeySession, the plane an AVPlayerLayer beneath the metal view. ARC,
  # like every other .mm in core.
  target_sources(screenkit-core PRIVATE core/src/media/MediaPlayerApple.mm)
  set_source_files_properties(core/src/media/MediaPlayerApple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(screenkit-core PUBLIC
    "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo" "-framework QuartzCore"
    "-framework AudioToolbox")
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_libraries(screenkit-core PUBLIC "-framework AppKit")
  else()
    target_link_libraries(screenkit-core PUBLIC "-framework UIKit")
  endif()
  # [/apple-media]
elseif(ANDROID)
  # [android-media]
  # Media3 ExoPlayer through runtime/android/app/src/main/java/dev/screenkit/
  # media/VideoPlayer.java (HLS, DASH, progressive; Widevine and ClearKey through
  # MediaDrm), resolved in JNI_OnLoad (MediaPlayerAndroid.h). jni.h is in the
  # sysroot; nothing else is linked.
  target_sources(screenkit-core PRIVATE core/src/media/MediaPlayerAndroid.cpp)
  # [/android-media]
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # [linux-media]
  # libvlc -- the image's VLC 3.0.21 -- plays; its pictures come out through
  # libvlc's memory output into linear dma-bufs committed to a Wayland subsurface
  # beneath the window (core/src/media/linux/WaylandVideo.cpp). The imported
  # targets and the scanner are cmake/LinuxSystem.cmake's; the protocol code is
  # generated here from the pinned wayland-protocols XMLs.
  set(_wl_dir "${CMAKE_BINARY_DIR}/wayland-protocols")
  file(MAKE_DIRECTORY "${_wl_dir}")
  set(_wl_sources)
  foreach(_protocol linux-dmabuf-v1 viewporter presentation-time)
    set(_xml "${SCREENKIT_WAYLAND_PROTOCOLS_DIR}/${_protocol}.xml")
    add_custom_command(
      OUTPUT "${_wl_dir}/${_protocol}-client-protocol.h" "${_wl_dir}/${_protocol}-protocol.c"
      COMMAND "${SCREENKIT_WAYLAND_SCANNER}" client-header "${_xml}" "${_wl_dir}/${_protocol}-client-protocol.h"
      COMMAND "${SCREENKIT_WAYLAND_SCANNER}" private-code "${_xml}" "${_wl_dir}/${_protocol}-protocol.c"
      DEPENDS "${_xml}"
      COMMENT "wayland-scanner ${_protocol}"
      VERBATIM)
    list(APPEND _wl_sources "${_wl_dir}/${_protocol}-protocol.c" "${_wl_dir}/${_protocol}-client-protocol.h")
  endforeach()
  target_sources(screenkit-core PRIVATE
    core/src/media/MediaPlayerLinux.cpp
    core/src/media/linux/WaylandVideo.cpp
    core/src/media/linux/V4l2Probe.c
    ${_wl_sources})
  target_include_directories(screenkit-core PRIVATE "${_wl_dir}")
  target_link_libraries(screenkit-core PRIVATE vlc::vlc wayland::client libdrm::drm gbm::gbm)

  # The ScreenKit VLC plugin (core/src/media/linux/vlc-plugin/): a VLC module
  # VLC loads itself, from VLC_PLUGIN_PATH, which the player points at the
  # vlc-plugins/ folder beside the executable. VLC finds modules by file name,
  # lib*_plugin.so, and by the vlc_entry__3_0_0f symbol, the only one exported.
  add_library(screenkit-vlc-plugin MODULE
    core/src/media/linux/vlc-plugin/screenkit_vlc.c
    core/src/media/linux/V4l2Probe.c)
  set_target_properties(screenkit-vlc-plugin PROPERTIES
    OUTPUT_NAME screenkit_plugin
    PREFIX lib
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vlc-plugins"
    C_STANDARD 11
    C_EXTENSIONS ON
    C_VISIBILITY_PRESET hidden)
  target_compile_definitions(screenkit-vlc-plugin PRIVATE
    __PLUGIN__ MODULE_STRING="screenkit" _FILE_OFFSET_BITS=64 _REENTRANT _THREAD_SAFE)
  target_compile_options(screenkit-vlc-plugin PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(screenkit-vlc-plugin PRIVATE vlc::plugin ffmpeg44::avcodec pthread)
  add_dependencies(screenkit-core screenkit-vlc-plugin)
  # [/linux-media]
else()
  target_sources(screenkit-core PRIVATE core/src/media/MediaPlayerUnavailable.cpp)
endif()

# ---- system fonts -----------------------------------------------------------
# The fonts a canvas names without loading ("20px Arial", "sans-serif") come from
# the OS: CoreText on Apple, /system/fonts on Android, the font directories on
# Linux. Elsewhere there are none yet, and text draws only in fonts the app loads
# itself.
if(APPLE)
  target_sources(screenkit-core PRIVATE core/src/text/SystemFontsApple.mm)
  set_source_files_properties(core/src/text/SystemFontsApple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(screenkit-core PUBLIC "-framework CoreText")
elseif(ANDROID)
  target_sources(screenkit-core PRIVATE core/src/text/SystemFontsAndroid.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(screenkit-core PRIVATE core/src/text/SystemFontsLinux.cpp)
else()
  target_sources(screenkit-core PRIVATE core/src/text/SystemFontsNone.cpp)
endif()
# The system zlib: core/src/gfx/FrameCapture.cpp writes PNGs with it.
find_package(ZLIB REQUIRED)
target_link_libraries(screenkit-core PUBLIC ZLIB::ZLIB)
if(ANDROID)
  # core/src/Log.cpp writes to logcat.
  target_link_libraries(screenkit-core PUBLIC android::log)
endif()

# Which holes this target's ANGLE archives have is decided by AnglePrebuilt.cmake,
# which reads the archive rather than guessing from the platform.
if(ANGLE_STUB_DEFINES)
  set_source_files_properties(core/src/gfx/angle_stubs.mm PROPERTIES
    COMPILE_DEFINITIONS "${ANGLE_STUB_DEFINES}")
endif()

# ---- vendored expo-gl -------------------------------------------------------
# third_party/gl/ is a tracked port of expo/expo's packages/expo-gl/common/,
# produced by tools/vendor/expo-gl.sh and never hand-edited (see its VENDOR.md).
# It is compiled here so a bad re-import is a build failure rather than a
# surprise at the next feature -- but it is deliberately NOT linked into
# screenkit-core: nothing consumes it yet, core/src/bindings/gl.def and the bare
# `gl` global are still what JS reaches.
set(SCREENKIT_GL_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gl")

# Globbed, not listed. The acceptance property is "no third-party source
# compiled from outside the vendored dir", and a glob rooted at that dir is what
# states it -- an explicit list would also have to be re-edited on every import,
# which is the hand-maintenance this port exists to avoid. CONFIGURE_DEPENDS
# makes a re-import re-run the glob.
file(GLOB SCREENKIT_GL_VENDOR_SOURCES CONFIGURE_DEPENDS
     "${SCREENKIT_GL_VENDOR_DIR}/*.cpp")

if(NOT SCREENKIT_GL_VENDOR_SOURCES)
  message(STATUS "vendored expo-gl: absent -- run tools/vendor/expo-gl.sh")
else()
  add_library(screenkit-gl-vendored STATIC ${SCREENKIT_GL_VENDOR_SOURCES})

  # The vendored dir first, so its own headers win; core/include for
  # screenkit/Log.h, which the SKPlatformUtils seam routes logging to.
  target_include_directories(screenkit-gl-vendored PUBLIC
    "${SCREENKIT_GL_VENDOR_DIR}" core/include)

  # angle::angle carries ANGLE's headers -- the whole point of the rewrite is
  # that <GLES3/gl3.h> resolves to ANGLE and not to Apple's deprecated GLES.
  # hermes::vm carries jsi/jsi.h.
  target_link_libraries(screenkit-gl-vendored PUBLIC ${SCREENKIT_GLES} ${SCREENKIT_JSI_ENGINE})
  set_target_properties(screenkit-gl-vendored PROPERTIES POSITION_INDEPENDENT_CODE ON)

  # The vendored WebGL is now the GL path. screenkit-core supplies the two
  # symbols that archive deliberately leaves undefined: screenkit::log, and
  # screenkit::gl::ContextGet from gfx/VendoredWebGL.cpp. Linked here rather
  # than beside the other core libraries because the target does not exist yet
  # at that point -- an if(TARGET) there is silently always false.
  target_link_libraries(screenkit-core PUBLIC screenkit-gl-vendored)
  target_include_directories(screenkit-core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gl")

  # No -Wall -Wextra: this is upstream's code, and warnings we are not allowed
  # to fix in place (a fix is a rules change) would only train people to ignore
  # the build log. What upstream does trip is silenced by name rather than with
  # a blanket -w, so a *new* warning from a re-import is still visible -- and
  # each flag is probed, because the set differs between clang and gcc.
  include(CheckCXXCompilerFlag)
  foreach(_gl_w -Wno-unused-parameter -Wno-unused-variable -Wno-deprecated-declarations
                -Wno-vla-cxx-extension)
    string(MAKE_C_IDENTIFIER "HAVE${_gl_w}" _gl_w_var)
    check_cxx_compiler_flag("${_gl_w}" ${_gl_w_var})
    if(${_gl_w_var})
      target_compile_options(screenkit-gl-vendored PRIVATE "${_gl_w}")
    endif()
  endforeach()

  list(LENGTH SCREENKIT_GL_VENDOR_SOURCES _gl_vendor_count)
  message(STATUS "vendored expo-gl: ${_gl_vendor_count} translation units (not linked into screenkit-core)")
endif()

# ---- the DOM shim prelude ---------------------------------------------------
# js/dom-shim.js is evaluated before every app bundle, so it is compiled with the
# same pinned hermesc the fixtures are rather than shipped as source: a syntax
# error becomes a build failure instead of a fatal at launch, and cold start
# never pays to parse it.
#
# It lands next to the host binary because that is where the host looks for it.
# NSBundle answers for both shapes -- for a command-line tool the main bundle's
# resource path IS the directory holding the executable, and for the tvOS .app
# the file is copied inside (see apple/CMakeLists.txt). The Android APK carries
# its own copy in assets, compiled by Gradle (android/app/build.gradle); the one
# built here only makes a syntax error fail that build too.
set(SCREENKIT_DOM_SHIM_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/js/dom-shim.js")
set(SCREENKIT_DOM_SHIM_HBC "${CMAKE_BINARY_DIR}/dom-shim.hbc")

if(SCREENKIT_ENGINE STREQUAL "spidermonkey")
  # SpiderMonkey runs the prelude from its source, beside the host
  # (engine::domShimFileName). Precompiling it is screenkit-smc's job, which needs
  # the engine itself to run; the source is what every build can produce.
  add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/dom-shim.js"
    COMMAND "${CMAKE_COMMAND}" -E copy "${SCREENKIT_DOM_SHIM_SOURCE}" "${CMAKE_BINARY_DIR}/dom-shim.js"
    DEPENDS "${SCREENKIT_DOM_SHIM_SOURCE}"
    COMMENT "the DOM shim prelude, as source, for SpiderMonkey"
    VERBATIM)
  add_custom_target(screenkit-dom-shim ALL DEPENDS "${CMAKE_BINARY_DIR}/dom-shim.js")
elseif(NOT HERMESC)
  message(FATAL_ERROR
    "hermesc not found in the prebuilts cache -- the DOM shim prelude cannot be built, "
    "and a host without its prelude is a broken artifact dressed up as a successful build.\n"
    "  Run: node tools/prebuilts/fetch.mjs --hermesc")
endif()

if(NOT SCREENKIT_ENGINE STREQUAL "spidermonkey")
add_custom_command(
  OUTPUT "${SCREENKIT_DOM_SHIM_HBC}"
  # -Xes6-block-scoping: without it hermesc compiles `let`/`const` as `var`, and a
  # closure made in `for (let ...)` sees only the last iteration. See HermesHost.cpp.
  COMMAND "${HERMESC}" -emit-binary -O -Xes6-block-scoping
          -out "${SCREENKIT_DOM_SHIM_HBC}" "${SCREENKIT_DOM_SHIM_SOURCE}"
  DEPENDS "${SCREENKIT_DOM_SHIM_SOURCE}"
  COMMENT "compiling the DOM shim prelude with the pinned hermesc"
  VERBATIM)

add_custom_target(screenkit-dom-shim ALL DEPENDS "${SCREENKIT_DOM_SHIM_HBC}")
endif()

# ---- @screenkit/shaka, for the media rows ------------------------------------
# The Shaka-shaped player (packages/@screenkit/shaka) is app code, bundled by an
# app's own build; the media rows load it the way the suite loads the prelude:
# compiled by the pinned hermesc beside the host, so a syntax error fails the
# build. tests/ShakaScript.cmake turns the module into a script. Only where the
# suite is built (see the tests section below): not for tvOS.
if(SCREENKIT_BUILD_TESTS AND ((APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "tvOS") OR ANDROID
                              OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
  set(SCREENKIT_SHAKA_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../packages/@screenkit/shaka/src/index.js")
  set(SCREENKIT_SHAKA_SCRIPT "${CMAKE_BINARY_DIR}/shaka-script.js")
  set(SCREENKIT_SHAKA_HBC "${CMAKE_BINARY_DIR}/shaka.hbc")
  add_custom_command(
    OUTPUT "${SCREENKIT_SHAKA_HBC}"
    COMMAND "${CMAKE_COMMAND}" -DIN=${SCREENKIT_SHAKA_SOURCE} -DOUT=${SCREENKIT_SHAKA_SCRIPT}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ShakaScript.cmake"
    COMMAND "${HERMESC}" -emit-binary -O -Xes6-block-scoping
            -out "${SCREENKIT_SHAKA_HBC}" "${SCREENKIT_SHAKA_SCRIPT}"
    DEPENDS "${SCREENKIT_SHAKA_SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ShakaScript.cmake"
    COMMENT "compiling @screenkit/shaka for the media rows"
    VERBATIM)
  add_custom_target(screenkit-shaka-script ALL DEPENDS "${SCREENKIT_SHAKA_HBC}")
endif()

# ---- platform shell ---------------------------------------------------------
if(APPLE)
  add_subdirectory(apple)
elseif(ANDROID)
  # libscreenkit.so, loaded by the APK's activity (runtime/android/).
  add_subdirectory(android)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(linux)
endif()

# ---- tests ------------------------------------------------------------------
# The whole suite drives the macOS host and ANGLE's offscreen surfaces; the other
# two targets have neither. Android and Linux each build one target out of the
# same sources -- the net rows, which need no drawable -- so the client under
# them is measured on a device rather than only compiled for one
# (runtime/tests/CMakeLists.txt). `pi.sh build` configures with tests off, and
# `pi.sh test` turns them on.
if(SCREENKIT_BUILD_TESTS AND ((APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "tvOS") OR ANDROID
                              OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
  if(NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    enable_testing()
  endif()
  add_subdirectory(tests)
endif()
