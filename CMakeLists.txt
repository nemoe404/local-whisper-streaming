cmake_minimum_required(VERSION 3.29)
project(lws VERSION 0.0.0 LANGUAGES C CXX)

# The compiler verdict runs first, before anything else: a wrong compiler stops for this reason only
# cmake/Clang21Toolchain.cmake owns the pinned LLVM major.
include(${PROJECT_SOURCE_DIR}/cmake/Clang21Toolchain.cmake)
lws_clang21_verdict(_lws_toolchain_refusals
    C_ID "${CMAKE_C_COMPILER_ID}" C_VERSION "${CMAKE_C_COMPILER_VERSION}"
    CXX_ID "${CMAKE_CXX_COMPILER_ID}" CXX_VERSION "${CMAKE_CXX_COMPILER_VERSION}"
    EXPECTED_MAJOR "${LWS_LLVM_MAJOR}")
if(NOT "${_lws_toolchain_refusals}" STREQUAL "")
    list(JOIN _lws_toolchain_refusals "\n  " _lws_toolchain_text)
    message(FATAL_ERROR
        "lws: compiler refused (cmake/Clang21Toolchain.cmake):\n  ${_lws_toolchain_text}")
endif()
message(STATUS "lws: [toolchain] holds")

# CUDA is enabled here, before the whisper.cpp subdirectory, because the compiler identity cannot be
# read from this directory once ggml enables CUDA inside its own. The host must be clang 21; CMake
# left alone picks GCC without a word, so the verdict refuses anything else at configure time.
if(GGML_CUDA)
    enable_language(CUDA)
    lws_clang21_verdict(_lws_cuda_refusals
        C_ID "${CMAKE_C_COMPILER_ID}" C_VERSION "${CMAKE_C_COMPILER_VERSION}"
        CXX_ID "${CMAKE_CXX_COMPILER_ID}" CXX_VERSION "${CMAKE_CXX_COMPILER_VERSION}"
        CUDA
        CUDA_ID "${CMAKE_CUDA_COMPILER_ID}" CUDA_VERSION "${CMAKE_CUDA_COMPILER_VERSION}"
        CUDA_HOST_ID "${CMAKE_CUDA_HOST_COMPILER_ID}"
        CUDA_HOST_VERSION "${CMAKE_CUDA_HOST_COMPILER_VERSION}"
        EXPECTED_MAJOR "${LWS_LLVM_MAJOR}"
        EXPECTED_CUDA_MAJOR "${LWS_CUDA_MAJOR}")
    if(NOT "${_lws_cuda_refusals}" STREQUAL "")
        list(JOIN _lws_cuda_refusals "\n  " _lws_cuda_text)
        message(FATAL_ERROR
            "lws: CUDA compiler refused (cmake/Clang21Toolchain.cmake):\n  ${_lws_cuda_text}")
    endif()
    message(STATUS
        "lws: [toolchain][cuda] holds: NVIDIA ${CMAKE_CUDA_COMPILER_VERSION}, host ${CMAKE_CUDA_HOST_COMPILER_ID} ${CMAKE_CUDA_HOST_COMPILER_VERSION}")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# `import std` is not used; skip module scanning under Ninja.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(LWS_BUILD_TESTS "Build the tests" OFF)

# Sanitizer leg. CMakePresets.json selects it; an unknown value stops here, before the
# whisper.cpp subdirectory, so a typo cannot build an uninstrumented leg that still calls itself asan-ubsan.
set(LWS_SANITIZER off CACHE STRING "asan-ubsan, tsan or off")
include(${PROJECT_SOURCE_DIR}/cmake/SanitizerFlags.cmake)
lws_sanitizer_flags("${LWS_SANITIZER}" LWS_SANITIZER_COMPILE LWS_SANITIZER_LINK)
# Measurement-only switch that runs uninstrumented ggml under TSan: `lws-only` leaves the
# whisper.cpp subtree uninstrumented while lws targets stay instrumented. Both sanitizer presets keep
# `all`, as scripts/sanitizer-gate.cmake requires.
set(LWS_SANITIZER_SCOPE all CACHE STRING "all or lws-only (measurement-only; presets keep all)")
set_property(CACHE LWS_SANITIZER_SCOPE PROPERTY STRINGS all lws-only)
if(NOT LWS_SANITIZER_SCOPE STREQUAL "all" AND NOT LWS_SANITIZER_SCOPE STREQUAL "lws-only")
    message(FATAL_ERROR "lws: unknown LWS_SANITIZER_SCOPE='${LWS_SANITIZER_SCOPE}' (expected all or lws-only)")
endif()
if(LWS_SANITIZER_SCOPE STREQUAL "lws-only" AND "${LWS_SANITIZER_COMPILE}" STREQUAL "")
    message(FATAL_ERROR "lws: LWS_SANITIZER_SCOPE=lws-only needs LWS_SANITIZER=asan-ubsan or tsan")
endif()

# Linked only by our own targets. whisper.cpp and ggml never see these flags.
add_library(lws_warnings INTERFACE)
target_compile_options(lws_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast
    -Wnon-virtual-dtor -Wimplicit-fallthrough -Wdouble-promotion -Werror)

# Instrumentation must cover every linked translation unit: ours through this target, whisper.cpp's
# through the project hook below. It is deliberately separate from lws_warnings, which must never reach
# whisper.cpp.
add_library(lws_sanitizers INTERFACE)
target_compile_options(lws_sanitizers INTERFACE ${LWS_SANITIZER_COMPILE})
target_link_options(lws_sanitizers INTERFACE ${LWS_SANITIZER_LINK})

# whisper.cpp is built as static archives from the submodule. The policy and the plain
# set must both come before add_subdirectory: the fork's root declares cmake_minimum_required(3.5),
# so without CMP0077 its option(BUILD_SHARED_LIBS) would ignore the plain set and build shared.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_SHARED_LIBS OFF)
# The ggml CPU baseline is pinned from cmake/CpuBaseline.cmake: GGML_NATIVE OFF plus an
# explicit ISA list, every entry CACHE FORCE, so a -DGGML_NATIVE=ON cannot reach ggml-cpu.
# LWS_CPU_BASELINE has no default here; CMakePresets.json selects it, and an empty value stops.
set(LWS_CPU_BASELINE "" CACHE STRING "ggml CPU baseline (cmake/CpuBaseline.cmake), set by CMakePresets.json")
include(${PROJECT_SOURCE_DIR}/cmake/CpuBaseline.cmake)
lws_select_cpu_baseline(LWS_SELECTED_CPU_ISAS _lws_baseline_reason "${LWS_CPU_BASELINE}")
if(NOT "${_lws_baseline_reason}" STREQUAL "")
    message(FATAL_ERROR "lws: ${_lws_baseline_reason}")
endif()
lws_pin_ggml_cpu_baseline()
# The one owner of the whisper.cpp project hook: cmake/EnableSanitizers.cmake runs as the last
# step of project("whisper.cpp") and instruments the whisper.cpp subtree (ggml included) from inside its
# own directory scope. No preset names the hook, so `cmake --preset tsan` and a preset-less configure with
# -DLWS_SANITIZER=tsan take the same route. A normal variable shadows any cache entry of that name.
# With LWS_SANITIZER off the variable stays unset and whisper.cpp's project() includes nothing.
if(NOT "${LWS_SANITIZER_COMPILE}" STREQUAL "")
    set(CMAKE_PROJECT_whisper.cpp_INCLUDE "${PROJECT_SOURCE_DIR}/cmake/EnableSanitizers.cmake")
endif()
set(GGML_OPENMP OFF CACHE BOOL "ggml: use OpenMP (libomp is absent; ggml has its own thread pool)" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "whisper: build tests" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "whisper: build examples" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "whisper: build the server example" FORCE)
set(WHISPER_CURL OFF CACHE BOOL "whisper: libcurl model download" FORCE)
set(WHISPER_SDL2 OFF CACHE BOOL "whisper: SDL2 support" FORCE)
# SYSTEM keeps whisper.h and ggml headers out of our -Werror set.
add_subdirectory(third_party/whisper.cpp whisper EXCLUDE_FROM_ALL SYSTEM)
lws_check_ggml_cpu_baseline()

add_library(lws_smoke STATIC src/smoke/lws_smoke.cpp)
target_include_directories(lws_smoke PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(lws_smoke PRIVATE whisper lws_warnings lws_sanitizers)

# lws_core is the pure core: public headers under include/lws, sources under src/core. It links
# fmt only: no whisper, spdlog or threads, so the core stays testable without a model. Globbed on
# purpose, a new src/core/*.cpp is picked up without editing CMake. The preset's clang-21 rule applies
# unchanged; the target adds no compiler or flag override.
find_package(fmt CONFIG REQUIRED)
file(GLOB LWS_CORE_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/core/*.cpp)
add_library(lws_core STATIC ${LWS_CORE_SOURCES})
target_include_directories(lws_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(lws_core PUBLIC fmt::fmt)
target_link_libraries(lws_core PRIVATE lws_warnings lws_sanitizers)

# lws_shell is the imperative shell: the place whisper.cpp is allowed. lws_core never links it, and its
# headers stay under src/shell, internal until a public streaming type exists. Globbed like lws_core.
file(GLOB LWS_SHELL_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/shell/*.cpp)
add_library(lws_shell STATIC ${LWS_SHELL_SOURCES})
target_include_directories(lws_shell PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(lws_shell PUBLIC lws_core)
target_link_libraries(lws_shell PRIVATE whisper lws_warnings lws_sanitizers)

# lws_cli_lib is the command-line tool's logic library: every src/cli source except main.cpp, linked by
# the tests and by the executable. Globbed like lws_core, so a new src/cli/*.cpp joins it without a CMake
# edit. It consumes only the public headers and is never installed.
file(GLOB LWS_CLI_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/cli/*.cpp)
list(FILTER LWS_CLI_SOURCES EXCLUDE REGEX "/main\\.cpp$")
add_library(lws_cli_lib STATIC ${LWS_CLI_SOURCES})
target_include_directories(lws_cli_lib PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(lws_cli_lib PUBLIC lws_shell)
target_link_libraries(lws_cli_lib PRIVATE lws_warnings lws_sanitizers)

if(LWS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
