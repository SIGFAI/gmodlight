cmake_minimum_required(VERSION 3.20)
project(GModLight CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # static CRT: nothing extra to install
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# The version both halves log at startup (VERSION; tools/release.ps1 bumps it).
file(READ "${CMAKE_SOURCE_DIR}/VERSION" GMODLIGHT_VERSION)
string(STRIP "${GMODLIGHT_VERSION}" GMODLIGHT_VERSION)
add_compile_definitions(GMODLIGHT_VERSION="${GMODLIGHT_VERSION}")
message(STATUS "GModLight ${GMODLIGHT_VERSION}")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Both Dying Light and 64-bit Garry's Mod are x64; configure with -A x64")
endif()

add_library(minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# Dying Light plugin, loaded by the ASI loader.
add_library(GModLight SHARED
  dl_plugin/src/main.cpp
  dl_plugin/src/engine.cpp
  dl_plugin/src/overlay.cpp)
set_target_properties(GModLight PROPERTIES SUFFIX ".asi")
target_link_libraries(GModLight PRIVATE minhook d3d11 dxgi d3dcompiler)

# Garry's Mod binary module (64-bit branch). Copied as both gmcl_ and gmsv_.
add_library(gmodlight_module SHARED gmod_module/src/main.cpp)
set_target_properties(gmodlight_module PROPERTIES OUTPUT_NAME "gmcl_gmodlight_win64")
target_include_directories(gmodlight_module PRIVATE third_party/gmod-module-base/include)
target_link_libraries(gmodlight_module PRIVATE minhook d3d9)

# Test harness: plays Dying Light's side of the bridge so GMod can be tested alone.
add_executable(fakehost tools/fakehost.cpp)
