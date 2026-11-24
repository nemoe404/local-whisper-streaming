# cmake/Clang21Toolchain.cmake: the one owner of the pinned LLVM major.
#
# The compiler is pinned in two places that must agree: CMakePresets.json (CMAKE_C_COMPILER,
# CMAKE_CXX_COMPILER) and profiles/clang-21 (compiler_executables). This module owns the major
# version and refuses any other compiler at configure time, right after project(). Consumers
# fail closed when they disagree with it.
#
# Why a verdict module and not a second toolchain file: CMake takes exactly one toolchainFile per
# configure, and Conan's generated conan_toolchain.cmake owns that slot (it carries the profile's
# settings and the CMakeDeps paths).
#
# Script-mode safe: including this file only defines variables and functions, so `cmake -P` on it
# is a no-op that exits 0.

set(LWS_LLVM_MAJOR 21)
# The CUDA toolkit major. 13 is the first toolkit on this machine whose host-compiler check accepts
# clang 21; the older 12 toolkit refuses clang 18 and above, and a toolkit left to choose its own
# host picks GCC without a word.
set(LWS_CUDA_MAJOR 13)

# _lws_toolchain_leg_reason(<out_var> <tag> <id> <version> <expected_major>)
# Sets <out_var> in the caller to "" when the compiler holds, or to one `<tag> ...` reason.
# An empty id or version refuses (fail-closed). Pure: reads only its arguments.
function(_lws_toolchain_leg_reason out_var tag id version expected)
    if("${id}" STREQUAL "" OR "${version}" STREQUAL "")
        set(${out_var} "${tag} compiler id or version is empty: refused (fail-closed)" PARENT_SCOPE)
        return()
    endif()
    if(NOT "${id}" STREQUAL "Clang")
        set(${out_var} "${tag} compiler is ${id} ${version}, not Clang ${expected}" PARENT_SCOPE)
        return()
    endif()
    if(NOT "${version}" MATCHES "^([0-9]+)\\.")
        set(${out_var} "${tag} Clang version '${version}' has no leading major number: refused" PARENT_SCOPE)
        return()
    endif()
    set(_major "${CMAKE_MATCH_1}")
    if(NOT _major STREQUAL "${expected}")
        set(${out_var} "${tag} Clang ${version} has major ${_major}, not ${expected}" PARENT_SCOPE)
        return()
    endif()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

# _lws_cuda_compiler_reason(<out_var> <id> <version> <expected_major>)
# Sets <out_var> in the caller to "" when the CUDA compiler is NVIDIA's nvcc of the expected major,
# or to one `[toolchain][cuda]` reason. An empty id or version refuses (fail-closed). Pure.
function(_lws_cuda_compiler_reason out_var id version expected)
    set(_tag "[toolchain][cuda]")
    if("${id}" STREQUAL "" OR "${version}" STREQUAL "")
        set(${out_var} "${_tag} compiler id or version is empty: refused (fail-closed)" PARENT_SCOPE)
        return()
    endif()
    if(NOT "${id}" STREQUAL "NVIDIA")
        set(${out_var} "${_tag} compiler is ${id} ${version}, not NVIDIA nvcc ${expected}" PARENT_SCOPE)
        return()
    endif()
    if(NOT "${version}" MATCHES "^([0-9]+)\\.")
        set(${out_var} "${_tag} NVIDIA version '${version}' has no leading major number: refused" PARENT_SCOPE)
        return()
    endif()
    set(_major "${CMAKE_MATCH_1}")
    if(NOT _major STREQUAL "${expected}")
        set(${out_var} "${_tag} NVIDIA ${version} has major ${_major}, not ${expected}" PARENT_SCOPE)
        return()
    endif()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

# lws_clang21_verdict(<out_var> C_ID <id> C_VERSION <v> CXX_ID <id> CXX_VERSION <v>
#                     [CUDA CUDA_ID <id> CUDA_VERSION <v> EXPECTED_CUDA_MAJOR <n>]
#                     [CUDA_HOST_ID <id> CUDA_HOST_VERSION <v>] EXPECTED_MAJOR <n>)
#
# Sets <out_var> in the caller to a list of `[toolchain]` reasons, or to "" when every checked leg
# holds. With CUDA given, the CUDA legs are mandatory: the compiler must be NVIDIA nvcc of
# EXPECTED_CUDA_MAJOR and the host must be Clang of EXPECTED_MAJOR, and an empty identity for either
# is refused. The root passes CUDA when GGML_CUDA is on. Without CUDA the old host-only form still
# works: the host leg runs only when CUDA_HOST_ID is non-empty, which the rows that check a host alone use.
# A non-numeric EXPECTED_MAJOR, a non-numeric EXPECTED_CUDA_MAJOR given with CUDA, or unparsed arguments
# are misuse: FATAL_ERROR.
function(lws_clang21_verdict out_var)
    cmake_parse_arguments(_lws_arg "CUDA"
        "C_ID;C_VERSION;CXX_ID;CXX_VERSION;CUDA_ID;CUDA_VERSION;CUDA_HOST_ID;CUDA_HOST_VERSION;EXPECTED_MAJOR;EXPECTED_CUDA_MAJOR"
        "" ${ARGN})
    if(NOT "${_lws_arg_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR
            "lws: [toolchain] lws_clang21_verdict: unexpected arguments '${_lws_arg_UNPARSED_ARGUMENTS}' (misuse)")
    endif()
    if(NOT "${_lws_arg_EXPECTED_MAJOR}" MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "lws: [toolchain] lws_clang21_verdict: EXPECTED_MAJOR '${_lws_arg_EXPECTED_MAJOR}' is not a number (misuse)")
    endif()

    if(_lws_arg_CUDA AND NOT "${_lws_arg_EXPECTED_CUDA_MAJOR}" MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "lws: [toolchain] lws_clang21_verdict: EXPECTED_CUDA_MAJOR '${_lws_arg_EXPECTED_CUDA_MAJOR}' is not a number (misuse)")
    endif()

    set(_reasons "")
    _lws_toolchain_leg_reason(_reason "[toolchain][c]"
        "${_lws_arg_C_ID}" "${_lws_arg_C_VERSION}" "${_lws_arg_EXPECTED_MAJOR}")
    if(NOT "${_reason}" STREQUAL "")
        list(APPEND _reasons "${_reason}")
    endif()
    _lws_toolchain_leg_reason(_reason "[toolchain][cxx]"
        "${_lws_arg_CXX_ID}" "${_lws_arg_CXX_VERSION}" "${_lws_arg_EXPECTED_MAJOR}")
    if(NOT "${_reason}" STREQUAL "")
        list(APPEND _reasons "${_reason}")
    endif()
    if(_lws_arg_CUDA)
        _lws_cuda_compiler_reason(_reason
            "${_lws_arg_CUDA_ID}" "${_lws_arg_CUDA_VERSION}" "${_lws_arg_EXPECTED_CUDA_MAJOR}")
        if(NOT "${_reason}" STREQUAL "")
            list(APPEND _reasons "${_reason}")
        endif()
    endif()
    if(_lws_arg_CUDA OR NOT "${_lws_arg_CUDA_HOST_ID}" STREQUAL "")
        _lws_toolchain_leg_reason(_reason "[toolchain][cuda-host]"
            "${_lws_arg_CUDA_HOST_ID}" "${_lws_arg_CUDA_HOST_VERSION}" "${_lws_arg_EXPECTED_MAJOR}")
        if(NOT "${_reason}" STREQUAL "")
            list(APPEND _reasons "${_reason}")
        endif()
    endif()
    set(${out_var} "${_reasons}" PARENT_SCOPE)
endfunction()
