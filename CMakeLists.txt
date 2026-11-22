cmake_minimum_required(VERSION 3.29)
project(lws VERSION 0.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# `import std` is not used; skip module scanning under Ninja.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(LWS_BUILD_TESTS "Build the tests" OFF)

# Linked only by our own targets. whisper.cpp and ggml never see these flags.
add_library(lws_warnings INTERFACE)
target_compile_options(lws_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast
    -Wnon-virtual-dtor -Wimplicit-fallthrough -Wdouble-promotion -Werror)

# whisper.cpp is built as static archives from the submodule (D-01). The policy and the plain
# set must both come before add_subdirectory: the fork's root declares cmake_minimum_required(3.5),
# so without CMP0077 its option(BUILD_SHARED_LIBS) would ignore the plain set and build shared.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_SHARED_LIBS OFF)
set(GGML_NATIVE OFF CACHE BOOL "ggml: optimise for the build host (never ON in this tree)" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "ggml: use OpenMP (libomp is absent; ggml has its own thread pool)" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "whisper: build tests" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "whisper: build examples" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "whisper: build the server example" FORCE)
set(WHISPER_CURL OFF CACHE BOOL "whisper: libcurl model download" FORCE)
set(WHISPER_SDL2 OFF CACHE BOOL "whisper: SDL2 support" FORCE)
# SYSTEM keeps whisper.h and ggml headers out of our -Werror set.
add_subdirectory(third_party/whisper.cpp whisper EXCLUDE_FROM_ALL SYSTEM)

add_library(lws_smoke STATIC src/smoke/lws_smoke.cpp)
target_include_directories(lws_smoke PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(lws_smoke PRIVATE whisper lws_warnings)

if(LWS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
