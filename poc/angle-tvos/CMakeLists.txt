cmake_minimum_required(VERSION 3.24)
project(angle_tvos LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prebuilt ANGLE from tools/prebuilts. The version pin lives in manifest.json
# only -- read it here so the two cannot drift.
set(ANGLE_TARGET "apple-tvos-simulator-arm64" CACHE STRING "prebuilts target key")

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/prebuilts/manifest.json" _manifest)
string(JSON ANGLE_VERSION GET "${_manifest}" angle version)
string(REPLACE "/" "-" _version_dir "${ANGLE_VERSION}")

if(NOT DEFINED ANGLE_PREBUILTS)
  set(ANGLE_PREBUILTS "$ENV{HOME}/.screenkit/prebuilts")
endif()
set(_base        "${ANGLE_PREBUILTS}/angle/${_version_dir}")
set(ANGLE_INCLUDE "${_base}/include-src/include")
set(ANGLE_LIBDIR  "${_base}/${ANGLE_TARGET}")

message(STATUS "ANGLE ${ANGLE_VERSION} / ${ANGLE_TARGET}")

if(NOT EXISTS "${ANGLE_INCLUDE}/EGL/egl.h")
  message(FATAL_ERROR
    "ANGLE headers missing.\n  Run: node tools/prebuilts/fetch.mjs ${ANGLE_TARGET}")
endif()
foreach(lib EGL GLES ANGLE)
  file(GLOB _found "${ANGLE_LIBDIR}/lib${lib}.*.a")
  if(NOT _found)
    message(FATAL_ERROR
      "lib${lib} missing from ${ANGLE_LIBDIR}.\n  Run: node tools/prebuilts/fetch.mjs ${ANGLE_TARGET}")
  endif()
  list(GET _found 0 _lib)
  list(APPEND ANGLE_LIBS "${_lib}")
endforeach()

add_executable(angle_hello MACOSX_BUNDLE src/main.mm src/angle_stubs.mm)
target_include_directories(angle_hello PRIVATE "${ANGLE_INCLUDE}")

# Link order matters for static archives: the loaders reference libANGLE, so it
# must come last.
target_link_libraries(angle_hello PRIVATE
  ${ANGLE_LIBS}
  "-framework UIKit" "-framework QuartzCore" "-framework Metal"
  "-framework Foundation" "-framework CoreGraphics" "-framework IOSurface"
  "-framework CoreFoundation")

set_target_properties(angle_hello PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
  MACOSX_BUNDLE_BUNDLE_NAME           "ANGLE Hello"
  MACOSX_BUNDLE_GUI_IDENTIFIER        "com.example.anglehello"
  MACOSX_BUNDLE_BUNDLE_VERSION        "1"
  MACOSX_BUNDLE_SHORT_VERSION_STRING  "1.0")
