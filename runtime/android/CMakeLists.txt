# libscreenkit.so: the Android shell's native half. SDL's Java activity
# (runtime/android/app, ScreenKitActivity) loads it after SDL3, SDL3_ttf, fbjni
# and hermesvm, and runs its SDL_main -- the SDL_MAIN_USE_CALLBACKS host in
# jni/HostMain.cpp. The rest of the shell is host/Host.cpp, as on every platform.
#
# The prelude (dom-shim.hbc) and the bundled package are not built here: Gradle
# compiles them into the APK's assets, and the activity copies them out to files
# the host can open by path.

add_library(screenkit SHARED jni/HostMain.cpp ../host/Host.cpp ../host/FramePacing.cpp)
target_link_libraries(screenkit PRIVATE screenkit-core)
target_compile_definitions(screenkit PRIVATE SCREENKIT_RUNTIME_VERSION=${SCREENKIT_RUNTIME_VERSION})
target_compile_options(screenkit PRIVATE -Wall -Wextra)
# The prelude still compiles with the build, so a syntax error in dom-shim.js
# fails an Android build too.
add_dependencies(screenkit screenkit-dom-shim)
