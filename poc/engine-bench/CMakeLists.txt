# Two hosts for one workload: bench-hermes and bench-spidermonkey. Each is built in its own
# container against its own engine (bench.sh), so each is an option here.
#
#   cmake -B build/hermes -DBENCH_HERMES=ON -DHERMES_DIR=<extracted Linux Hermes prebuilt>
#   cmake -B build/spidermonkey -DBENCH_SPIDERMONKEY=ON        # mozjs-128 through pkg-config
cmake_minimum_required(VERSION 3.20)
project(engine_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(BENCH_HERMES "Build bench-hermes (needs HERMES_DIR)" OFF)
option(BENCH_SPIDERMONKEY "Build bench-spidermonkey (needs SpiderMonkey through pkg-config)" OFF)
set(HERMES_DIR "" CACHE PATH "An extracted Linux Hermes prebuilt: lib/libhermesvm.so and include/")
set(MOZJS_PKG "mozjs-128" CACHE STRING "SpiderMonkey's pkg-config name")

add_library(bench_core STATIC src/Bench.cpp src/Metrics.cpp)
target_include_directories(bench_core PUBLIC src)
target_compile_options(bench_core PRIVATE -Wall -Wextra)
target_link_libraries(bench_core PUBLIC ${CMAKE_DL_LIBS})

# The engine's library ships in lib/ beside bin/, as the ScreenKit host lays it out.
set(BENCH_RPATH "\$ORIGIN/../lib")

if(BENCH_HERMES)
  if(NOT EXISTS "${HERMES_DIR}/lib/libhermesvm.so")
    message(FATAL_ERROR "HERMES_DIR=${HERMES_DIR} holds no lib/libhermesvm.so")
  endif()
  add_executable(bench-hermes src/HermesMain.cpp)
  target_include_directories(bench-hermes PRIVATE "${HERMES_DIR}/include")
  target_link_directories(bench-hermes PRIVATE "${HERMES_DIR}/lib")
  # -rpath-link: the ICU that libhermesvm.so needs sits beside it, not on the system.
  target_link_options(bench-hermes PRIVATE "-Wl,-rpath-link,${HERMES_DIR}/lib")
  target_link_libraries(bench-hermes PRIVATE bench_core hermesvm)
  set_target_properties(bench-hermes PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "${BENCH_RPATH}")
endif()

if(BENCH_SPIDERMONKEY)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(MOZJS REQUIRED IMPORTED_TARGET ${MOZJS_PKG})
  add_executable(bench-spidermonkey src/SpiderMonkeyMain.cpp)
  target_link_libraries(bench-spidermonkey PRIVATE bench_core PkgConfig::MOZJS)
  set_target_properties(bench-spidermonkey PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "${BENCH_RPATH}")
endif()
