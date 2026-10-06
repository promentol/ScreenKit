cmake_minimum_required(VERSION 3.24)
project(sdlhello LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- SDL3 -------------------------------------------------------------------
# Static only: simulator apps can't load an unsigned dylib from the bundle
# without extra signing setup, and static keeps the .app self-contained.
set(SDL_SHARED       OFF CACHE BOOL "" FORCE)
set(SDL_STATIC       ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS        OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL      OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG        release-3.4.16
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(SDL3)

# ---- app --------------------------------------------------------------------
add_executable(sdlhello MACOSX_BUNDLE src/main.cpp)
target_link_libraries(sdlhello PRIVATE SDL3::SDL3-static)

set_target_properties(sdlhello PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
  MACOSX_BUNDLE_BUNDLE_NAME           "SDL Hello"
  MACOSX_BUNDLE_GUI_IDENTIFIER        "com.example.sdlhello"
  MACOSX_BUNDLE_BUNDLE_VERSION        "1"
  MACOSX_BUNDLE_SHORT_VERSION_STRING  "1.0"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.example.sdlhello"
  # Simulator only -- no team, no provisioning profile needed.
  XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED  "NO"
  XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
  XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY    ""
)
