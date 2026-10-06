# screenkit-smc (SmcMain.cpp), built against the system libmozjs-128 of
# tools/spidermonkey/Dockerfile -- the library the device loads.
cmake_minimum_required(VERSION 3.20)
project(screenkit_smc LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/../../../runtime")
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOZJS REQUIRED IMPORTED_TARGET mozjs-128)
add_executable(screenkit-smc SmcMain.cpp
  ${RUNTIME}/third_party/jsi/jsi/jsi.cpp
  ${RUNTIME}/core/src/spidermonkey/SpiderMonkeyRuntime.cpp)
target_include_directories(screenkit-smc PRIVATE ${RUNTIME}/third_party/jsi ${RUNTIME}/core/src)
# libmozjs is built without RTTI; the runtime subclasses one of its classes.
set_source_files_properties(${RUNTIME}/core/src/spidermonkey/SpiderMonkeyRuntime.cpp PROPERTIES COMPILE_OPTIONS -fno-rtti)
target_link_libraries(screenkit-smc PRIVATE PkgConfig::MOZJS)
