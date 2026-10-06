# JSI's own conformance suite (runtime/third_party/jsi/jsi/test/testlib.cpp -- the
# one Hermes and JSC are held to) run against the SpiderMonkey runtime.
#
#   sh runtime/tests/jsi/run.sh          build and run it in tools/spidermonkey's container
#
# A standalone project: it needs only JSI, the SpiderMonkey runtime and gtest,
# not the rest of the runtime or any of its platform dependencies.
cmake_minimum_required(VERSION 3.20)
project(screenkit_jsi_conformance LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/../..")
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOZJS REQUIRED IMPORTED_TARGET mozjs-128)
find_package(GTest REQUIRED)

add_executable(jsi-conformance
  SpiderMonkeyJsiTests.cpp
  ${RUNTIME}/third_party/jsi/jsi/jsi.cpp
  ${RUNTIME}/third_party/jsi/jsi/test/testlib.cpp
  ${RUNTIME}/core/src/spidermonkey/SpiderMonkeyRuntime.cpp)
target_include_directories(jsi-conformance PRIVATE ${RUNTIME}/third_party/jsi ${RUNTIME}/core/src)
target_compile_options(jsi-conformance PRIVATE -Wall -Wno-unused-parameter)
# libmozjs is built without RTTI; the runtime subclasses one of its classes.
set_source_files_properties(${RUNTIME}/core/src/spidermonkey/SpiderMonkeyRuntime.cpp PROPERTIES COMPILE_OPTIONS -fno-rtti)
target_link_libraries(jsi-conformance PRIVATE PkgConfig::MOZJS GTest::gtest GTest::gtest_main)
