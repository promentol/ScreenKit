# screenkit-host on Linux: a command-line tool that runs a bundle headless, or in
# a window, or fullscreen on a TV. It links the system's SDL3, SDL3_ttf, EGL and
# GLES (cmake/LinuxSystem.cmake) and the prebuilt libhermesvm.so, which it finds
# in ../lib at run time -- the layout tools/batocera deploys.

add_executable(screenkit-host HostMain.cpp ../host/Host.cpp ../host/FramePacing.cpp)
target_link_libraries(screenkit-host PRIVATE screenkit-core)
target_compile_definitions(screenkit-host PRIVATE SCREENKIT_RUNTIME_VERSION=${SCREENKIT_RUNTIME_VERSION})
target_compile_options(screenkit-host PRIVATE -Wall -Wextra)
add_dependencies(screenkit-host screenkit-dom-shim)

set_target_properties(screenkit-host PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "$ORIGIN/../lib:$ORIGIN")
