# Locate the prebuilt ANGLE from tools/prebuilts and expose it as an imported
# target. ANGLE is never compiled here -- a source build needs depot_tools and
# 10-15 GB, against 22 MB for the prebuilt archives plus headers
# (Architecture.md 10.2).
#
# The version pin lives in tools/prebuilts/manifest.json and nowhere else --
# read it here so the build and the manifest cannot drift.

set(SCREENKIT_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../../tools/prebuilts/manifest.json")
if(NOT EXISTS "${SCREENKIT_MANIFEST}")
  message(FATAL_ERROR "prebuilts manifest not found at ${SCREENKIT_MANIFEST}")
endif()

file(READ "${SCREENKIT_MANIFEST}" _angle_manifest)
string(JSON ANGLE_VERSION GET "${_angle_manifest}" angle version)
string(JSON ANGLE_HEADERS_REF GET "${_angle_manifest}" angle headers ref)

string(JSON _angle_target_json ERROR_VARIABLE _angle_target_err
       GET "${_angle_manifest}" angle targets "${SCREENKIT_TARGET}")
if(_angle_target_err)
  string(JSON _angle_why ERROR_VARIABLE _angle_ignored
         GET "${_angle_manifest}" angle unavailable "${SCREENKIT_TARGET}")
  if(_angle_why AND NOT _angle_why STREQUAL "NOTFOUND")
    message(FATAL_ERROR "ANGLE has no prebuilt for \"${SCREENKIT_TARGET}\":\n  ${_angle_why}")
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

# "chromium/7578" -> "chromium-7578", matching fetch.mjs's versionDir().
string(REPLACE "/" "-" _angle_version_dir "${ANGLE_VERSION}")
set(ANGLE_ROOT    "${SCREENKIT_PREBUILTS}/angle/${_angle_version_dir}")
set(ANGLE_INCLUDE "${ANGLE_ROOT}/include-src/include")
set(ANGLE_LIBDIR  "${ANGLE_ROOT}/${SCREENKIT_TARGET}")

set(_angle_fetch_script "${CMAKE_CURRENT_LIST_DIR}/../../tools/prebuilts/fetch.mjs")
set(_angle_fetch_hint "node tools/prebuilts/fetch.mjs --dep angle ${SCREENKIT_TARGET}")

# A cold cache is not an error: fetch on demand, so `cmake -S runtime -B ...` on
# a fresh clone reaches a build without a separate bootstrap step. Nothing is
# compiled by this -- the archives are downloaded, checksum-verified, unpacked
# and (for tvOS) retagged.
# The header stamp holds the commit fetch.mjs checked out. Comparing it with the
# manifest is what makes a ref bump -- or a cache from before headers were pinned
# -- refetch, instead of compiling against whatever include/ happens to be there.
set(_angle_headers_stamp "${ANGLE_ROOT}/include-src/.verified")
set(_angle_headers_current FALSE)
if(EXISTS "${_angle_headers_stamp}")
  file(READ "${_angle_headers_stamp}" _angle_headers_have)
  string(STRIP "${_angle_headers_have}" _angle_headers_have)
  if(_angle_headers_have STREQUAL ANGLE_HEADERS_REF)
    set(_angle_headers_current TRUE)
  endif()
endif()

if(NOT EXISTS "${ANGLE_INCLUDE}/EGL/egl.h" OR NOT _angle_headers_current
   OR NOT EXISTS "${ANGLE_LIBDIR}/.verified")
  find_program(NODE_EXECUTABLE node)
  if(NODE_EXECUTABLE)
    message(STATUS "ANGLE ${ANGLE_VERSION} missing, or headers not at ${ANGLE_HEADERS_REF} -- fetching")
    execute_process(
      COMMAND "${NODE_EXECUTABLE}" "${_angle_fetch_script}" --dep angle "${SCREENKIT_TARGET}"
      RESULT_VARIABLE _angle_fetch_result)
    if(NOT _angle_fetch_result EQUAL 0)
      message(FATAL_ERROR "fetching ANGLE failed (exit ${_angle_fetch_result}).\n  Run: ${_angle_fetch_hint}")
    endif()
  endif()
endif()

if(NOT EXISTS "${ANGLE_INCLUDE}/EGL/eglext_angle.h")
  message(FATAL_ERROR
    "ANGLE headers missing at ${ANGLE_INCLUDE}.\n  Run: ${_angle_fetch_hint}")
endif()
file(READ "${_angle_headers_stamp}" _angle_headers_have)
string(STRIP "${_angle_headers_have}" _angle_headers_have)
if(NOT _angle_headers_have STREQUAL ANGLE_HEADERS_REF)
  message(FATAL_ERROR
    "ANGLE headers are at ${_angle_headers_have}, the manifest pins ${ANGLE_HEADERS_REF}.\n"
    "  Run: ${_angle_fetch_hint}")
endif()

# Godot names its archives per slice (libANGLE.macos.arm64.a,
# libANGLE.ios.arm64.simulator.a, ...), so glob rather than spell them out --
# the name is the publisher's, not something we should encode per target.
set(ANGLE_LIBS "")
foreach(_lib EGL GLES ANGLE)
  file(GLOB _found "${ANGLE_LIBDIR}/lib${_lib}.*.a")
  if(NOT _found)
    message(FATAL_ERROR
      "lib${_lib} missing from ${ANGLE_LIBDIR}.\n  Run: ${_angle_fetch_hint}")
  endif()
  list(GET _found 0 _one)
  # Link order matters for static archives and this loop is that order: the
  # loaders in libEGL/libGLES reference libANGLE, so libANGLE must come last.
  list(APPEND ANGLE_LIBS "${_one}")
  if(_lib STREQUAL "ANGLE")
    set(_angle_core_lib "${_one}")
  endif()
endforeach()

message(STATUS "ANGLE ${ANGLE_VERSION} / ${SCREENKIT_TARGET}")
message(STATUS "  libs:    ${ANGLE_LIBDIR}")
message(STATUS "  headers: ${ANGLE_INCLUDE}")

# ---- which symbols Godot's archives are missing ------------------------------
# Godot links some symbols from its own tree, so its published archives have
# holes -- and the holes differ per slice: the macOS archive carries
# angle::GetCurrentSystemTime and angle::SetCurrentThreadName, the iOS/tvOS one
# does not. Supplying them unconditionally would be a duplicate-symbol error on
# macOS, so probe rather than assume. astcenc is absent from every slice
# measured, and is probed on the same pass rather than trusted.
set(ANGLE_STUB_DEFINES "")
find_program(NM_EXECUTABLE nm)
if(NM_EXECUTABLE AND _angle_core_lib)
  execute_process(
    COMMAND "${NM_EXECUTABLE}" -g "${_angle_core_lib}"
    OUTPUT_VARIABLE _angle_symbols
    ERROR_QUIET
    RESULT_VARIABLE _angle_nm_result)
  if(NOT _angle_nm_result EQUAL 0)
    message(WARNING "nm failed on ${_angle_core_lib}; assuming ANGLE needs every stub")
    set(_angle_symbols "")
  endif()

  # A definition is "<address> T <name>"; an undefined reference is "U <name>".
  # Matching on the address plus the T is what distinguishes the two.
  if(NOT _angle_symbols MATCHES "[0-9a-f]+ T __ZN5angle20GetCurrentSystemTimeEv")
    list(APPEND ANGLE_STUB_DEFINES SCREENKIT_ANGLE_STUB_PLATFORM_UTILS)
  endif()
  if(NOT _angle_symbols MATCHES "[0-9a-f]+ T __Z24astcenc_get_error_string13astcenc_error")
    list(APPEND ANGLE_STUB_DEFINES SCREENKIT_ANGLE_STUB_ASTCENC)
  endif()
else()
  message(WARNING
    "nm not found -- cannot tell which symbols ${SCREENKIT_TARGET}'s ANGLE archives omit. "
    "Assuming the iOS/tvOS shape (both stub sets). A duplicate-symbol link error here "
    "means this target's archives are complete.")
  set(ANGLE_STUB_DEFINES SCREENKIT_ANGLE_STUB_PLATFORM_UTILS SCREENKIT_ANGLE_STUB_ASTCENC)
endif()

if(ANGLE_STUB_DEFINES)
  message(STATUS "  stubs:   ${ANGLE_STUB_DEFINES}")
else()
  message(STATUS "  stubs:   none needed")
endif()

# ---- the target --------------------------------------------------------------
# INTERFACE rather than a STATIC IMPORTED target: there are three archives whose
# order is load-bearing, and an imported target carries exactly one location.
add_library(angle::angle INTERFACE IMPORTED GLOBAL)
set_target_properties(angle::angle PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${ANGLE_INCLUDE}")

set(_angle_frameworks
  "-framework Metal"
  "-framework QuartzCore"
  "-framework IOSurface"
  "-framework CoreGraphics"
  "-framework Foundation"
  "-framework CoreFoundation")
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  # The macOS slice reaches AppKit (NSView/NSScreen) and IOKit (display
  # enumeration) where the UIKit slices reach UIKit instead.
  list(APPEND _angle_frameworks "-framework Cocoa" "-framework IOKit")
else()
  list(APPEND _angle_frameworks "-framework UIKit")
endif()

set_target_properties(angle::angle PROPERTIES
  INTERFACE_LINK_LIBRARIES "${ANGLE_LIBS};${_angle_frameworks}")
