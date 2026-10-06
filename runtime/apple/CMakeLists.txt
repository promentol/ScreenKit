# screenkit-host: the platform shell. On macOS it is a command-line tool that
# runs a bundle and exits; on tvOS it is an .app, because the simulator will not
# launch anything else.

set(_host_sources HostMain.mm PlatformLog.mm ../host/Host.cpp ../host/FramePacing.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
  list(APPEND _host_sources RemotePresses.mm)
  add_executable(screenkit-host MACOSX_BUNDLE ${_host_sources})
else()
  add_executable(screenkit-host ${_host_sources})
endif()

target_link_libraries(screenkit-host PRIVATE screenkit-core "-framework Foundation")

# The package gate's newest supported runtimeVersion (runtime/VERSION).
target_compile_definitions(screenkit-host PRIVATE
  SCREENKIT_RUNTIME_VERSION=${SCREENKIT_RUNTIME_VERSION})

# The prelude is not optional: the host evaluates it after the graphics
# bootstrap and refuses to run an app bundle without it. It also has to exist
# before the tvOS POST_BUILD copy below runs.
add_dependencies(screenkit-host screenkit-dom-shim)
# ARC: the .mm sources are manual retain/release otherwise, which leaks the
# window and the root view controller on every launch.
target_compile_options(screenkit-host PRIVATE -Wall -Wextra $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

# The spec's verification commands expect the binary at the top of the build
# tree, not under apple/.
set_target_properties(screenkit-host PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
  target_link_libraries(screenkit-host PRIVATE "-framework UIKit")

  set_target_properties(screenkit-host PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST           "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME          "ScreenKit Host"
    MACOSX_BUNDLE_GUI_IDENTIFIER       "dev.screenkit.host"
    MACOSX_BUNDLE_BUNDLE_VERSION       "1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    # Only the app's own Frameworks/: without this CMake also bakes in the
    # prebuilts cache's absolute framework directories, which let a simulator
    # run load SDL3 from the Mac's disk and hid that the app never carried it --
    # a device has no such directory.
    BUILD_WITH_INSTALL_RPATH           TRUE
    BUILD_RPATH                        "@executable_path/Frameworks"
    INSTALL_RPATH                      "@executable_path/Frameworks")

  # hermesvm, SDL3 and SDL3_ttf are dynamic frameworks with @rpath install names,
  # so the .app has to carry all three. Networking needs nothing embedded: it is
  # Network.framework, part of the OS. Ad-hoc signing is enough for the simulator.
  add_custom_command(TARGET screenkit-host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:screenkit-host>/Frameworks"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${HERMES_FRAMEWORK}"
            "$<TARGET_BUNDLE_CONTENT_DIR:screenkit-host>/Frameworks/hermesvm.framework"
    COMMAND codesign --force --sign - --timestamp=none
            "$<TARGET_BUNDLE_CONTENT_DIR:screenkit-host>/Frameworks/hermesvm.framework"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_BUNDLE_CONTENT_DIR:screenkit-host>/Frameworks/SDL3.framework"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${SDL3_FRAMEWORK}"
            "$<TARGET_BUNDLE_CONTENT_DIR:screenkit-host>/Frameworks/SDL3.framework"
    COMMAND codesign --force --sign - --timestamp=none
            "$<TARGET_BUNDLE_CONTENT_DIR:screenkit-host>/Frameworks/SDL3.framework"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_BUNDLE_CONTENT_DIR:screenkit-host>/Frameworks/SDL3_ttf.framework"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${SDL3_TTF_FRAMEWORK}"
            "$<TARGET_BUNDLE_CONTENT_DIR:screenkit-host>/Frameworks/SDL3_ttf.framework"
    COMMAND codesign --force --sign - --timestamp=none
            "$<TARGET_BUNDLE_CONTENT_DIR:screenkit-host>/Frameworks/SDL3_ttf.framework"
    COMMENT "embedding hermesvm.framework, SDL3.framework and SDL3_ttf.framework")

  # The bundle travels inside the app: tvOS has no argv to point at one. Without
  # it the app builds and launches but can only report a missing bundle, which
  # is a broken artifact dressed up as a successful build.
  if(NOT HERMESC)
    message(FATAL_ERROR
      "hermesc not in the prebuilts cache -- the tvOS app cannot be built without hello.hbc.\n"
      "  Run: node tools/prebuilts/fetch.mjs --hermesc")
  endif()
  if(NOT NODE_EXECUTABLE)
    message(FATAL_ERROR "node not found -- needed to compile the tvOS app's hello.hbc")
  endif()
  add_custom_command(TARGET screenkit-host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/fixtures"
    COMMAND "${NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/make-fixtures.mjs"
            "${HERMESC}" "${CMAKE_BINARY_DIR}/fixtures"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/fixtures/hello.hbc"
            "$<TARGET_BUNDLE_CONTENT_DIR:screenkit-host>/hello.hbc"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/fixtures/timers.hbc"
            "$<TARGET_BUNDLE_CONTENT_DIR:screenkit-host>/timers.hbc"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/fixtures/triangle.hbc"
            "$<TARGET_BUNDLE_CONTENT_DIR:screenkit-host>/triangle.hbc"
    # The prelude travels with the app the same way the bundle does; NSBundle
    # finds it by name. Without it the app launches and dies on the first
    # document.createElement, which is a worse failure than not building.
    COMMAND ${CMAKE_COMMAND} -E copy "${SCREENKIT_DOM_SHIM_HBC}"
            "$<TARGET_BUNDLE_CONTENT_DIR:screenkit-host>/dom-shim.hbc"
    COMMENT "compiling the app bundle's .hbc fixtures and copying the DOM shim prelude")

  # An app to ship in place of the built-in fixtures: a .skpkg directory, the
  # output of `screenkit bundle`. It is embedded as app.skpkg/, which the host
  # prefers over every fixture and gates on its manifest before running; the
  # package directory is the app's asset root, so its `/fonts/...` URLs resolve
  # inside it. Unset, the fixtures run and the M3/M4 gates behave exactly as
  # before.
  if(DEFINED CACHE{SCREENKIT_APP_DIR})
    if(SCREENKIT_APP_DIR)
      message(WARNING
        "SCREENKIT_APP_DIR is gone and is ignored: embed a package instead, "
        "-DSCREENKIT_APP_PKG=<app.skpkg> (see examples/lightning3-blits/README.md)")
    endif()
    unset(SCREENKIT_APP_DIR CACHE)
  endif()
  set(SCREENKIT_APP_PKG "" CACHE PATH ".skpkg directory (screenkit bundle output) to embed in the tvOS app")
  set(_app_pkg_dest "$<TARGET_BUNDLE_CONTENT_DIR:screenkit-host>/app.skpkg")
  if(SCREENKIT_APP_PKG)
    if(NOT EXISTS "${SCREENKIT_APP_PKG}/manifest.json")
      message(FATAL_ERROR
        "SCREENKIT_APP_PKG=${SCREENKIT_APP_PKG} has no manifest.json -- it is not a .skpkg. "
        "Build one with `npm run build:screenkit` (vite build, then screenkit bundle).")
    endif()
    message(STATUS "tvOS app embeds ${SCREENKIT_APP_PKG} as app.skpkg")
    # Removed first, so a file dropped from the package does not linger in the
    # .app from an earlier build.
    add_custom_command(TARGET screenkit-host POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -rf "${_app_pkg_dest}"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${SCREENKIT_APP_PKG}" "${_app_pkg_dest}"
      COMMENT "embedding ${SCREENKIT_APP_PKG} as app.skpkg")
  else()
    # And a package embedded by an earlier configure must not outlive the
    # setting: the host would still find it and run it instead of the fixtures.
    add_custom_command(TARGET screenkit-host POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -rf "${_app_pkg_dest}"
      COMMENT "no SCREENKIT_APP_PKG: removing any embedded app.skpkg")
  endif()

  # POST_BUILD steps only run when screenkit-host itself relinks. Everything above
  # copies a *generated* file into the bundle, so a change to only that file --
  # the DOM shim, or a package rebuilt by `screenkit bundle` -- rebuilt it outside
  # the .app and then shipped the stale copy, because nothing relinked. That
  # shipped a tvOS app still running an old prelude while the macOS host, which
  # reads the build tree directly, looked fixed. LINK_DEPENDS makes those files
  # force the relink, so the copies and the signature below always follow them.
  # For a package that means every file in it, manifest included: `screenkit
  # bundle` rewrites them all, and the glob is re-checked on every build, so a
  # file added to or dropped from the package reconfigures too.
  set_property(TARGET screenkit-host APPEND PROPERTY LINK_DEPENDS "${SCREENKIT_DOM_SHIM_HBC}")
  if(SCREENKIT_APP_PKG)
    file(GLOB_RECURSE _app_pkg_files CONFIGURE_DEPENDS "${SCREENKIT_APP_PKG}/*")
    set_property(TARGET screenkit-host APPEND PROPERTY LINK_DEPENDS ${_app_pkg_files})
  endif()

  # Sign the app last, after the framework and resources are in place.
  add_custom_command(TARGET screenkit-host POST_BUILD
    COMMAND codesign --force --sign - --timestamp=none
            "$<TARGET_BUNDLE_DIR:screenkit-host>"
    COMMENT "ad-hoc signing screenkit-host.app")
else()
  # macOS links straight out of the prebuilts cache; nothing is copied, so the
  # binary is only runnable on a machine that has fetched Hermes.
  set_target_properties(screenkit-host PROPERTIES
    BUILD_RPATH   "${HERMES_RPATH}"
    INSTALL_RPATH "${HERMES_RPATH}")
endif()
