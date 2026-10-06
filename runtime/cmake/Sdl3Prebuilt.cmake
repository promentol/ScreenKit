# Locate the prebuilt SDL3 from tools/prebuilts and expose it as an imported
# target. SDL3 is never compiled here: the official release ships one
# SDL3.xcframework carrying genuine macOS, iOS and tvOS slices (the tvOS
# simulator slice reports LC_BUILD_VERSION platform 8), so unlike ANGLE it needs
# no retagging.
#
# The runtime uses SDL3 for timers today -- SDL_AddTimerNS owns the event loop's
# scheduling. Window, input and audio arrive with their own specs.
#
# The version pin lives in tools/prebuilts/manifest.json and nowhere else --
# read it here so the build and the manifest cannot drift.

set(SCREENKIT_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../../tools/prebuilts/manifest.json")
if(NOT EXISTS "${SCREENKIT_MANIFEST}")
  message(FATAL_ERROR "prebuilts manifest not found at ${SCREENKIT_MANIFEST}")
endif()

file(READ "${SCREENKIT_MANIFEST}" _sdl_manifest)
string(JSON SDL3_VERSION GET "${_sdl_manifest}" sdl3 version)

string(JSON _sdl_framework_rel ERROR_VARIABLE _sdl_target_err
       GET "${_sdl_manifest}" sdl3 targets "${SCREENKIT_TARGET}" framework)
if(_sdl_target_err)
  string(JSON _sdl_why ERROR_VARIABLE _sdl_ignored
         GET "${_sdl_manifest}" sdl3 unavailable "${SCREENKIT_TARGET}")
  if(_sdl_why AND NOT _sdl_why STREQUAL "NOTFOUND")
    message(FATAL_ERROR "SDL3 has no prebuilt for \"${SCREENKIT_TARGET}\":\n  ${_sdl_why}")
  endif()
  message(FATAL_ERROR
    "unknown prebuilts target \"${SCREENKIT_TARGET}\".\n"
    "  Run: node tools/prebuilts/fetch.mjs --list")
endif()

# SCREENKIT_PREBUILTS is resolved by HermesPrebuilt.cmake, which runs first.
if(NOT DEFINED SCREENKIT_PREBUILTS)
  if(DEFINED ENV{SCREENKIT_PREBUILTS})
    set(SCREENKIT_PREBUILTS "$ENV{SCREENKIT_PREBUILTS}")
  else()
    set(SCREENKIT_PREBUILTS "$ENV{HOME}/.screenkit/prebuilts")
  endif()
endif()

# Every Apple slice comes out of one .dmg, so the cache holds a single unpacked
# xcframework and the target key only picks the framework inside it.
set(SDL3_ROOT      "${SCREENKIT_PREBUILTS}/sdl3/${SDL3_VERSION}/apple")
set(SDL3_FRAMEWORK "${SDL3_ROOT}/${_sdl_framework_rel}")
get_filename_component(SDL3_FRAMEWORK_DIR "${SDL3_FRAMEWORK}" DIRECTORY)

set(_sdl_fetch_script "${CMAKE_CURRENT_LIST_DIR}/../../tools/prebuilts/fetch.mjs")
set(_sdl_fetch_hint "node tools/prebuilts/fetch.mjs --dep sdl3 ${SCREENKIT_TARGET}")

# A cold cache is not an error: fetch on demand, so `cmake -S runtime -B ...` on
# a fresh clone reaches a build without a separate bootstrap step. Nothing is
# compiled by this -- the archive is downloaded, checksum-verified and unpacked.
if(NOT EXISTS "${SDL3_FRAMEWORK}/SDL3")
  find_program(NODE_EXECUTABLE node)
  if(NODE_EXECUTABLE)
    message(STATUS "SDL3 ${SDL3_VERSION} not in the prebuilts cache -- fetching")
    execute_process(
      COMMAND "${NODE_EXECUTABLE}" "${_sdl_fetch_script}" --dep sdl3 "${SCREENKIT_TARGET}"
      RESULT_VARIABLE _sdl_fetch_result)
    if(NOT _sdl_fetch_result EQUAL 0)
      message(FATAL_ERROR "fetching SDL3 failed (exit ${_sdl_fetch_result}).\n  Run: ${_sdl_fetch_hint}")
    endif()
  endif()
endif()

if(NOT EXISTS "${SDL3_FRAMEWORK}/SDL3")
  message(FATAL_ERROR "SDL3.framework missing at ${SDL3_FRAMEWORK}.\n  Run: ${_sdl_fetch_hint}")
endif()

# Headers live in SDL3.framework/Headers, which is exactly what makes
# `#include <SDL3/SDL_timer.h>` resolve through the framework search path. There
# is no separate include directory to add.
message(STATUS "SDL3 ${SDL3_VERSION} / ${SCREENKIT_TARGET}")
message(STATUS "  framework: ${SDL3_FRAMEWORK}")

add_library(sdl3::sdl3 SHARED IMPORTED GLOBAL)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  # The macOS slice is a versioned framework; its install name points through
  # Versions/A, so that is the binary to import.
  set_target_properties(sdl3::sdl3 PROPERTIES
    IMPORTED_LOCATION "${SDL3_FRAMEWORK}/Versions/A/SDL3")
else()
  set_target_properties(sdl3::sdl3 PROPERTIES IMPORTED_LOCATION "${SDL3_FRAMEWORK}/SDL3")
endif()
set_target_properties(sdl3::sdl3 PROPERTIES FRAMEWORK TRUE)
# -F, not an include dir: the umbrella spelling <SDL3/SDL_x.h> only resolves when
# the compiler is told where the framework itself lives.
target_compile_options(sdl3::sdl3 INTERFACE "SHELL:-F ${SDL3_FRAMEWORK_DIR}")

# The framework's install name is @rpath/SDL3.framework/... so consumers need an
# rpath pointing at the directory that holds it. Bundled apps get a second one
# (@executable_path/Frameworks) where the framework is copied in.
set(SDL3_RPATH "${SDL3_FRAMEWORK_DIR}")
