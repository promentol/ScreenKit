# SpiderMonkey behind JSI: SCREENKIT_ENGINE=spidermonkey (Linux).
#
# Defines screenkit-spidermonkey -- JSI's own implementation (vendored at the
# Hermes commit the prebuilts use, third_party/jsi) and the SpiderMonkey runtime
# (core/src/spidermonkey) -- which stands where hermes::vm stands in a Hermes
# build. No Hermes is linked.
#
# SCREENKIT_MOZJS_DIR is an unpacked libmozjs-128: include/mozjs-128/ and
# lib/libmozjs-128.so.0, as tools/spidermonkey/fetch.sh writes it from Debian's
# pinned packages. The library is linked by that soname and ships in lib/ beside
# the host (tools/batocera/pi.sh, ENGINE=spidermonkey).
set(SCREENKIT_MOZJS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../tools/spidermonkey/dist/${SCREENKIT_TARGET}"
    CACHE PATH "An unpacked libmozjs-128: include/mozjs-128 and lib/libmozjs-128.so.0")
set(_mozjs_include "${SCREENKIT_MOZJS_DIR}/include/mozjs-128")
set(_mozjs_library "${SCREENKIT_MOZJS_DIR}/lib/libmozjs-128.so.0")
if(NOT EXISTS "${_mozjs_include}/jsapi.h" OR NOT EXISTS "${_mozjs_library}")
  message(FATAL_ERROR "SpiderMonkey not found at ${SCREENKIT_MOZJS_DIR}.\n"
                      "  Run: sh tools/spidermonkey/fetch.sh ${SCREENKIT_TARGET}")
endif()

add_library(mozjs::mozjs SHARED IMPORTED)
set_target_properties(mozjs::mozjs PROPERTIES
  IMPORTED_LOCATION "${_mozjs_library}"
  IMPORTED_SONAME "libmozjs-128.so.0"
  INTERFACE_INCLUDE_DIRECTORIES "${_mozjs_include}"
  # Debian 13 built it against glibc 2.38; the Debian 12 toolchain that builds
  # the host has 2.36. Those symbols resolve on the device (Batocera 42: 2.40).
  INTERFACE_LINK_OPTIONS "LINKER:--unresolved-symbols=ignore-in-shared-libs")

set(_jsi_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jsi")
add_library(screenkit-spidermonkey STATIC
  "${_jsi_dir}/jsi/jsi.cpp"
  core/src/spidermonkey/SpiderMonkeyRuntime.cpp)
target_include_directories(screenkit-spidermonkey PUBLIC "${_jsi_dir}")
target_link_libraries(screenkit-spidermonkey PUBLIC mozjs::mozjs)
# libmozjs is built without RTTI, and the runtime subclasses one of its classes
# (a proxy handler), whose typeinfo the library does not export.
set_source_files_properties(core/src/spidermonkey/SpiderMonkeyRuntime.cpp PROPERTIES COMPILE_OPTIONS -fno-rtti)
set_target_properties(screenkit-spidermonkey PROPERTIES POSITION_INDEPENDENT_CODE ON)
message(STATUS "engine: SpiderMonkey from ${SCREENKIT_MOZJS_DIR}")
