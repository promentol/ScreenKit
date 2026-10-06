# The libraries the runtime links on Android, as imported targets named the way
# the Apple prebuilts and LinuxSystem.cmake name theirs: sdl3::sdl3, sdl3::ttf,
# and gles::gles (the NDK's EGL and GLES 3 stubs, which bind to the device's
# driver -- there is no ANGLE to link on Android). android::log is logcat.
#
# SDL3 and SDL3_ttf are the official release AARs, pinned in
# tools/prebuilts/manifest.json and fetched into the prebuilts cache by fetch.mjs.
# Gradle hands them to CMake as prefab packages (runtime/android/app/build.gradle,
# `buildFeatures.prefab`), so they are found with find_package rather than by
# path -- which is also how their .so files reach the APK. hermes::vm comes from
# HermesPrebuilt.cmake, the same way.
#
# A configure outside Gradle has no prefab packages and stops here with that
# explanation; `sh tools/android/android.sh build` is the way in.

find_package(SDL3 CONFIG)
find_package(SDL3_ttf CONFIG)
if(NOT TARGET SDL3::SDL3-shared OR NOT TARGET SDL3_ttf::SDL3_ttf-shared)
  message(FATAL_ERROR
    "the SDL3 / SDL3_ttf prefab packages were not found. An Android build runs through Gradle, which "
    "unpacks the AARs from the prebuilts cache as prefab packages: sh tools/android/android.sh build")
endif()

add_library(sdl3::sdl3 INTERFACE IMPORTED GLOBAL)
target_link_libraries(sdl3::sdl3 INTERFACE SDL3::SDL3-shared)
add_library(sdl3::ttf INTERFACE IMPORTED GLOBAL)
target_link_libraries(sdl3::ttf INTERFACE SDL3_ttf::SDL3_ttf-shared sdl3::sdl3)

# From the NDK sysroot for the platform level Gradle configures (minSdk 24).
# libGLESv3 exports the GLES 2 entry points as well, so one library serves an
# ES 2 context and an ES 3 one alike.
find_library(ANDROID_GLESV3_LIBRARY GLESv3 REQUIRED)
find_library(ANDROID_EGL_LIBRARY EGL REQUIRED)
find_library(ANDROID_LOG_LIBRARY log REQUIRED)
find_library(ANDROID_ANDROID_LIBRARY android REQUIRED)

add_library(gles::gles INTERFACE IMPORTED GLOBAL)
set_target_properties(gles::gles PROPERTIES
  INTERFACE_LINK_LIBRARIES "${ANDROID_GLESV3_LIBRARY};${ANDROID_EGL_LIBRARY}")
add_library(android::log INTERFACE IMPORTED GLOBAL)
set_target_properties(android::log PROPERTIES
  INTERFACE_LINK_LIBRARIES "${ANDROID_LOG_LIBRARY};${ANDROID_ANDROID_LIBRARY}")

message(STATUS "SDL3 and SDL3_ttf from prefab / ${SCREENKIT_TARGET} (${CMAKE_ANDROID_ARCH_ABI}, API ${ANDROID_PLATFORM_LEVEL})")
message(STATUS "GLES: ${ANDROID_GLESV3_LIBRARY}")
