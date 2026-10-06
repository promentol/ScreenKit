# Locate the prebuilt Hermes from tools/prebuilts and expose it as an imported
# target. Hermes is never compiled here: `hermesvm` ships the engine AND the JSI
# implementation (466 facebook::jsi::* symbols), so an embedder links that one
# framework and never vendors ReactCommon or compiles jsi.cpp. Android is the
# exception to the JSI half: its hermesvm links a prebuilt libjsi.so instead.
#
# The version pin lives in tools/prebuilts/manifest.json and nowhere else --
# read it here so the build and the manifest cannot drift.

set(SCREENKIT_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../../tools/prebuilts/manifest.json")
if(NOT EXISTS "${SCREENKIT_MANIFEST}")
  message(FATAL_ERROR "prebuilts manifest not found at ${SCREENKIT_MANIFEST}")
endif()

file(READ "${SCREENKIT_MANIFEST}" _manifest)
string(JSON HERMES_VERSION          GET "${_manifest}" hermes version)
string(JSON HERMES_BYTECODE_VERSION GET "${_manifest}" hermes bytecodeVersion)
string(JSON _hermes_include         GET "${_manifest}" hermes include)

if(NOT DEFINED SCREENKIT_PREBUILTS)
  if(DEFINED ENV{SCREENKIT_PREBUILTS})
    set(SCREENKIT_PREBUILTS "$ENV{SCREENKIT_PREBUILTS}")
  else()
    set(SCREENKIT_PREBUILTS "$ENV{HOME}/.screenkit/prebuilts")
  endif()
endif()

set(_fetch_script "${CMAKE_CURRENT_LIST_DIR}/../../tools/prebuilts/fetch.mjs")
set(_fetch_hint "node tools/prebuilts/fetch.mjs --dep hermes ${SCREENKIT_TARGET}")
find_program(NODE_EXECUTABLE node)

# An Android target is the official hermes-android AAR, consumed as a prefab
# package through Gradle (runtime/android/app/build.gradle), so CMake finds it
# with find_package rather than by path. Two things the AAR does not carry come
# from the prebuilts cache, where fetch.mjs puts them: the JSI headers (the
# prefab excludes jsi/**; they are the Apple destroot's, the same Hermes) and
# libjsi.so. Unlike Apple's hermesvm, Android's does not export the JSI
# implementation -- it links libjsi.so, which React Native ships, and so must we.
string(JSON _hermes_aar ERROR_VARIABLE _aar_err
       GET "${_manifest}" hermes targets "${SCREENKIT_TARGET}" aar)
if(NOT _aar_err)
  string(JSON _hermes_jsi_rel GET "${_manifest}" hermes targets "${SCREENKIT_TARGET}" jsi)
  string(JSON _hermes_android_include GET "${_manifest}" hermes targets "${SCREENKIT_TARGET}" include)
  set(HERMES_ROOT    "${SCREENKIT_PREBUILTS}/hermes/${HERMES_VERSION}/android")
  set(HERMES_INCLUDE "${HERMES_ROOT}/${_hermes_android_include}")
  set(HERMES_JSI     "${HERMES_ROOT}/${_hermes_jsi_rel}")
  if((NOT EXISTS "${HERMES_JSI}" OR NOT EXISTS "${HERMES_INCLUDE}/jsi/jsi.h") AND NODE_EXECUTABLE)
    message(STATUS "Hermes ${HERMES_VERSION} ${SCREENKIT_TARGET} not in the prebuilts cache -- fetching")
    execute_process(
      COMMAND "${NODE_EXECUTABLE}" "${_fetch_script}" --dep hermes "${SCREENKIT_TARGET}"
      RESULT_VARIABLE _fetch_result)
  endif()
  if(NOT EXISTS "${HERMES_JSI}" OR NOT EXISTS "${HERMES_INCLUDE}/jsi/jsi.h")
    message(FATAL_ERROR "libjsi.so or the JSI headers for ${SCREENKIT_TARGET} missing at ${HERMES_ROOT}.\n"
                        "  Run: ${_fetch_hint}")
  endif()

  # The prefab package Gradle generated from the AAR: hermes-engine::hermesvm.
  find_package(hermes-engine CONFIG)
  if(NOT TARGET hermes-engine::hermesvm)
    message(FATAL_ERROR
      "the hermes-engine prefab package was not found. An Android build runs through Gradle, which "
      "unpacks ${_hermes_aar} as a prefab package: sh tools/android/android.sh build")
  endif()
  message(STATUS "Hermes ${HERMES_VERSION} (hbc ${HERMES_BYTECODE_VERSION}) / ${SCREENKIT_TARGET}")
  message(STATUS "  prefab: hermes-engine::hermesvm from ${_hermes_aar}")
  message(STATUS "  jsi:    ${HERMES_JSI}")

  add_library(hermes::jsi SHARED IMPORTED GLOBAL)
  set_target_properties(hermes::jsi PROPERTIES
    IMPORTED_LOCATION "${HERMES_JSI}"
    INTERFACE_INCLUDE_DIRECTORIES "${HERMES_INCLUDE}")
  add_library(hermes::vm INTERFACE IMPORTED GLOBAL)
  target_link_libraries(hermes::vm INTERFACE hermes-engine::hermesvm hermes::jsi)

  # hermesc for the machine doing the build, as for Apple below: the same
  # fetch.mjs --hermesc that Gradle asks for the APK's own copy of the prelude,
  # so both are compiled by the pinned binary. -DHERMESC overrides it.
  if(NOT HERMESC AND NODE_EXECUTABLE)
    execute_process(
      COMMAND "${NODE_EXECUTABLE}" "${_fetch_script}" --hermesc
      OUTPUT_VARIABLE _hermesc_path OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
      RESULT_VARIABLE _hermesc_result)
    if(_hermesc_result EQUAL 0 AND EXISTS "${_hermesc_path}")
      set(HERMESC "${_hermesc_path}")
    endif()
  endif()
  message(STATUS "  hermesc: ${HERMESC}")
  return()
endif()

# A Linux target is an archive of our own (tools/prebuilts/recipes/hermes-linux):
# its manifest entry names a library, where an Apple one names a framework.
string(JSON _hermes_lib ERROR_VARIABLE _lib_err
       GET "${_manifest}" hermes targets "${SCREENKIT_TARGET}" lib)
if(_lib_err)
  string(JSON _framework_rel ERROR_VARIABLE _target_err
         GET "${_manifest}" hermes targets "${SCREENKIT_TARGET}" framework)
  if(_target_err)
    string(JSON _why ERROR_VARIABLE _ignored
           GET "${_manifest}" hermes unavailable "${SCREENKIT_TARGET}")
    if(_why AND NOT _why STREQUAL "NOTFOUND")
      message(FATAL_ERROR "Hermes has no runtime for \"${SCREENKIT_TARGET}\":\n  ${_why}")
    endif()
    message(FATAL_ERROR
      "unknown prebuilts target \"${SCREENKIT_TARGET}\".\n"
      "  Run: node tools/prebuilts/fetch.mjs --list")
  endif()
endif()

if(NOT _lib_err)
  string(JSON _hermes_lib_include GET "${_manifest}" hermes targets "${SCREENKIT_TARGET}" include)
  set(HERMES_ROOT    "${SCREENKIT_PREBUILTS}/hermes/${HERMES_VERSION}/${SCREENKIT_TARGET}")
  set(HERMES_INCLUDE "${HERMES_ROOT}/${_hermes_lib_include}")
  set(HERMES_LIBRARY "${HERMES_ROOT}/${_hermes_lib}")
  if(NOT EXISTS "${HERMES_LIBRARY}" AND NODE_EXECUTABLE)
    message(STATUS "Hermes ${HERMES_VERSION} ${SCREENKIT_TARGET} not in the prebuilts cache -- fetching")
    execute_process(
      COMMAND "${NODE_EXECUTABLE}" "${_fetch_script}" --dep hermes "${SCREENKIT_TARGET}"
      RESULT_VARIABLE _fetch_result)
  endif()
  if(NOT EXISTS "${HERMES_LIBRARY}" OR NOT EXISTS "${HERMES_INCLUDE}/hermes/hermes.h")
    message(FATAL_ERROR "Hermes for ${SCREENKIT_TARGET} missing at ${HERMES_ROOT}.\n  Run: ${_fetch_hint}")
  endif()
  message(STATUS "Hermes ${HERMES_VERSION} (hbc ${HERMES_BYTECODE_VERSION}) / ${SCREENKIT_TARGET}")
  message(STATUS "  library: ${HERMES_LIBRARY}")

  add_library(hermes::vm SHARED IMPORTED GLOBAL)
  set_target_properties(hermes::vm PROPERTIES
    IMPORTED_LOCATION "${HERMES_LIBRARY}"
    IMPORTED_NO_SONAME TRUE
    INTERFACE_INCLUDE_DIRECTORIES "${HERMES_INCLUDE}")
  get_filename_component(HERMES_RPATH "${HERMES_LIBRARY}" DIRECTORY)
  # libhermesvm.so needs the ICU it carries beside it; the linker finds it there too.
  set_property(TARGET hermes::vm PROPERTY INTERFACE_LINK_OPTIONS "LINKER:-rpath-link,${HERMES_RPATH}")

  # The archive carries a hermesc built for the target. On a build machine of
  # that architecture -- the Docker image tools/batocera builds in -- it compiles
  # the prelude; elsewhere the pinned npm hermesc for the host does.
  set(HERMESC "")
  set(_hermes_target_arch "${SCREENKIT_TARGET}")
  string(REPLACE "linux-" "" _hermes_target_arch "${_hermes_target_arch}")
  set(_host_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
  if(_host_arch STREQUAL "aarch64")
    set(_host_arch "arm64")
  endif()
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND _host_arch STREQUAL _hermes_target_arch
     AND EXISTS "${HERMES_ROOT}/bin/hermesc")
    set(HERMESC "${HERMES_ROOT}/bin/hermesc")
  elseif(NODE_EXECUTABLE)
    execute_process(
      COMMAND "${NODE_EXECUTABLE}" "${_fetch_script}" --hermesc
      OUTPUT_VARIABLE _hermesc_path OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
      RESULT_VARIABLE _hermesc_result)
    if(_hermesc_result EQUAL 0 AND EXISTS "${_hermesc_path}")
      set(HERMESC "${_hermesc_path}")
    endif()
  endif()
  message(STATUS "  hermesc: ${HERMESC}")
  return()
endif()

# Every Apple slice comes out of one archive, so the cache holds a single
# unpacked destroot and the target key only picks the framework inside it.
set(HERMES_ROOT      "${SCREENKIT_PREBUILTS}/hermes/${HERMES_VERSION}/apple")
set(HERMES_INCLUDE   "${HERMES_ROOT}/${_hermes_include}")
set(HERMES_FRAMEWORK "${HERMES_ROOT}/${_framework_rel}")
get_filename_component(HERMES_FRAMEWORK_DIR "${HERMES_FRAMEWORK}" DIRECTORY)


# A cold cache is not an error: fetch on demand, so `cmake -S runtime -B ...` on
# a fresh clone reaches a build without a separate bootstrap step. Nothing is
# compiled by this -- the archive is downloaded, checksum-verified and unpacked.
if(NOT EXISTS "${HERMES_INCLUDE}/hermes/hermes.h" OR NOT EXISTS "${HERMES_FRAMEWORK}/hermesvm")
  if(NODE_EXECUTABLE)
    message(STATUS "Hermes ${HERMES_VERSION} not in the prebuilts cache -- fetching")
    execute_process(
      COMMAND "${NODE_EXECUTABLE}" "${_fetch_script}" --dep hermes "${SCREENKIT_TARGET}"
      RESULT_VARIABLE _fetch_result)
    if(NOT _fetch_result EQUAL 0)
      message(FATAL_ERROR "fetching Hermes failed (exit ${_fetch_result}).\n  Run: ${_fetch_hint}")
    endif()
  endif()
endif()

if(NOT EXISTS "${HERMES_INCLUDE}/hermes/hermes.h")
  message(FATAL_ERROR "Hermes headers missing at ${HERMES_INCLUDE}.\n  Run: ${_fetch_hint}")
endif()
if(NOT EXISTS "${HERMES_FRAMEWORK}/hermesvm")
  message(FATAL_ERROR "hermesvm.framework missing at ${HERMES_FRAMEWORK}.\n  Run: ${_fetch_hint}")
endif()

message(STATUS "Hermes ${HERMES_VERSION} (hbc ${HERMES_BYTECODE_VERSION}) / ${SCREENKIT_TARGET}")
message(STATUS "  framework: ${HERMES_FRAMEWORK}")

add_library(hermes::vm SHARED IMPORTED GLOBAL)
set_target_properties(hermes::vm PROPERTIES
  IMPORTED_LOCATION "${HERMES_FRAMEWORK}/hermesvm"
  FRAMEWORK TRUE
  INTERFACE_INCLUDE_DIRECTORIES "${HERMES_INCLUDE}")

# The framework's install name is @rpath/hermesvm.framework/... so consumers need
# an rpath pointing at the directory that holds it. Bundled apps get a second one
# (@executable_path/Frameworks) where the framework is copied in.
set(HERMES_RPATH "${HERMES_FRAMEWORK_DIR}")

# hermesc, for compiling test fixtures and .skpkg bundles. Same version as the
# engine by construction -- that identity is the point, since the loader refuses
# bytecode from any other hermesc.
#
# The path is not re-derived here. `fetch.mjs --hermesc` fetches if needed and
# prints the resolved path on stdout (everything else goes to stderr), so it
# stays the single authority on where the binary lives and which host binary is
# right -- including win32, which a CMAKE_HOST_APPLE/else split would get wrong.
set(HERMESC "")
if(NODE_EXECUTABLE)
  execute_process(
    COMMAND "${NODE_EXECUTABLE}" "${_fetch_script}" --hermesc
    OUTPUT_VARIABLE _hermesc_path
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _hermesc_result)
  if(_hermesc_result EQUAL 0 AND EXISTS "${_hermesc_path}")
    set(HERMESC "${_hermesc_path}")
    message(STATUS "  hermesc:   ${HERMESC}")
  endif()
endif()
