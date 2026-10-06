# Locate the prebuilt SDL3_ttf from tools/prebuilts and expose it as an imported
# target, the same way Sdl3Prebuilt.cmake does SDL3 (which runs first and
# resolves SCREENKIT_PREBUILTS). SDL3_ttf is never compiled here either: the
# official release ships one SDL3_ttf.xcframework with FreeType and HarfBuzz
# linked inside, and each slice links SDL3 through @rpath -- so it loads the
# SDL3.framework the host already carries.
#
# The 2D canvas draws text through it (core/src/text).

file(READ "${SCREENKIT_MANIFEST}" _ttf_manifest)
string(JSON SDL3_TTF_VERSION GET "${_ttf_manifest}" sdl3_ttf version)

string(JSON _ttf_framework_rel ERROR_VARIABLE _ttf_target_err
       GET "${_ttf_manifest}" sdl3_ttf targets "${SCREENKIT_TARGET}" framework)
if(_ttf_target_err)
  string(JSON _ttf_why ERROR_VARIABLE _ttf_ignored
         GET "${_ttf_manifest}" sdl3_ttf unavailable "${SCREENKIT_TARGET}")
  if(_ttf_why AND NOT _ttf_why STREQUAL "NOTFOUND")
    message(FATAL_ERROR "SDL3_ttf has no prebuilt for \"${SCREENKIT_TARGET}\":\n  ${_ttf_why}")
  endif()
  message(FATAL_ERROR
    "unknown prebuilts target \"${SCREENKIT_TARGET}\".\n"
    "  Run: node tools/prebuilts/fetch.mjs --list")
endif()

set(SDL3_TTF_ROOT      "${SCREENKIT_PREBUILTS}/sdl3_ttf/${SDL3_TTF_VERSION}/apple")
set(SDL3_TTF_FRAMEWORK "${SDL3_TTF_ROOT}/${_ttf_framework_rel}")
get_filename_component(SDL3_TTF_FRAMEWORK_DIR "${SDL3_TTF_FRAMEWORK}" DIRECTORY)

set(_ttf_fetch_script "${CMAKE_CURRENT_LIST_DIR}/../../tools/prebuilts/fetch.mjs")
set(_ttf_fetch_hint "node tools/prebuilts/fetch.mjs --dep sdl3_ttf ${SCREENKIT_TARGET}")

# A cold cache fetches on demand, exactly like SDL3: downloaded, checksum-verified
# and unpacked, never built.
if(NOT EXISTS "${SDL3_TTF_FRAMEWORK}/SDL3_ttf")
  find_program(NODE_EXECUTABLE node)
  if(NODE_EXECUTABLE)
    message(STATUS "SDL3_ttf ${SDL3_TTF_VERSION} not in the prebuilts cache -- fetching")
    execute_process(
      COMMAND "${NODE_EXECUTABLE}" "${_ttf_fetch_script}" --dep sdl3_ttf "${SCREENKIT_TARGET}"
      RESULT_VARIABLE _ttf_fetch_result)
    if(NOT _ttf_fetch_result EQUAL 0)
      message(FATAL_ERROR "fetching SDL3_ttf failed (exit ${_ttf_fetch_result}).\n  Run: ${_ttf_fetch_hint}")
    endif()
  endif()
endif()

if(NOT EXISTS "${SDL3_TTF_FRAMEWORK}/SDL3_ttf")
  message(FATAL_ERROR "SDL3_ttf.framework missing at ${SDL3_TTF_FRAMEWORK}.\n  Run: ${_ttf_fetch_hint}")
endif()

message(STATUS "SDL3_ttf ${SDL3_TTF_VERSION} / ${SCREENKIT_TARGET}")
message(STATUS "  framework: ${SDL3_TTF_FRAMEWORK}")

add_library(sdl3::ttf SHARED IMPORTED GLOBAL)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set_target_properties(sdl3::ttf PROPERTIES
    IMPORTED_LOCATION "${SDL3_TTF_FRAMEWORK}/Versions/A/SDL3_ttf")
else()
  set_target_properties(sdl3::ttf PROPERTIES IMPORTED_LOCATION "${SDL3_TTF_FRAMEWORK}/SDL3_ttf")
endif()
set_target_properties(sdl3::ttf PROPERTIES FRAMEWORK TRUE)
# <SDL3_ttf/SDL_ttf.h> resolves through the framework search path, as SDL3's does.
target_compile_options(sdl3::ttf INTERFACE "SHELL:-F ${SDL3_TTF_FRAMEWORK_DIR}")
target_link_libraries(sdl3::ttf INTERFACE sdl3::sdl3)
