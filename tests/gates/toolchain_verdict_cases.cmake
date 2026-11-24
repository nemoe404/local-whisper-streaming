# tests/gates/toolchain_verdict_cases.cmake: script-mode rows for lws_clang21_verdict.
#
#   cmake -DLWS_ROOT={root} -P tests/gates/toolchain_verdict_cases.cmake
#
# Each row gives the compiler identities and the [toolchain] tags the verdict must return. A row
# passes when the verdict returns exactly as many reasons as the row expects and every expected tag
# appears in one of them; an empty expectation means no refusal. Any mismatch is a FATAL_ERROR that
# names the row, so the script exits 1. ctest registers it as toolchain_verdict_cases (label gate).
cmake_minimum_required(VERSION 3.29)

if(NOT DEFINED LWS_ROOT OR "${LWS_ROOT}" STREQUAL "")
    message(FATAL_ERROR "toolchain_verdict_cases: -DLWS_ROOT={root} is required")
endif()
include(${LWS_ROOT}/cmake/Clang21Toolchain.cmake)

# lws_vc_row(<name> <expected_tags> <c_id> <c_version> <cxx_id> <cxx_version> <cuda_id> <cuda_version>)
# <expected_tags> is a list of [toolchain] tags, "" for none. An empty cuda_id skips the CUDA leg.
function(lws_vc_row name expected c_id c_version cxx_id cxx_version cuda_id cuda_version)
    lws_clang21_verdict(_got
        C_ID "${c_id}" C_VERSION "${c_version}"
        CXX_ID "${cxx_id}" CXX_VERSION "${cxx_version}"
        CUDA_HOST_ID "${cuda_id}" CUDA_HOST_VERSION "${cuda_version}"
        EXPECTED_MAJOR 21)

    list(LENGTH _got _got_n)
    list(LENGTH expected _want_n)
    if(NOT _got_n EQUAL _want_n)
        message(FATAL_ERROR
            "toolchain verdict row '${name}': expected ${_want_n} refusal(s), got ${_got_n}: [${_got}]")
    endif()
    foreach(_tag IN LISTS expected)
        set(_hit FALSE)
        foreach(_reason IN LISTS _got)
            string(FIND "${_reason}" "${_tag}" _pos)
            if(NOT _pos EQUAL -1)
                set(_hit TRUE)
            endif()
        endforeach()
        if(NOT _hit)
            message(FATAL_ERROR
                "toolchain verdict row '${name}': no refusal carries ${_tag}; got [${_got}]")
        endif()
    endforeach()
    set_property(GLOBAL APPEND PROPERTY LWS_VC_ROW_NAMES "${name}")
endfunction()

# The holds rows: clang 21 for C and CXX, with and without a CUDA host.
lws_vc_row("clang 21 holds for C and CXX" "" Clang 21.1.8 Clang 21.1.8 "" "")
lws_vc_row("clang 21 CUDA host holds" "" Clang 21.1.8 Clang 21.1.8 Clang 21.1.8)

# The refusal rows, one reason per named leg.
lws_vc_row("GNU C refused" "[toolchain][c]" GNU 15.2.0 Clang 21.1.8 "" "")
lws_vc_row("clang 20 CXX refused" "[toolchain][cxx]" Clang 21.1.8 Clang 20.1.8 "" "")
lws_vc_row("empty C id refused" "[toolchain][c]" "" 21.1.8 Clang 21.1.8 "" "")
lws_vc_row("empty CXX version refused" "[toolchain][cxx]" Clang 21.1.8 Clang "" "" "")
lws_vc_row("version 21 without a dot refused" "[toolchain][c]" Clang 21 Clang 21.1.8 "" "")
lws_vc_row("major 210 refused (exact major match)" "[toolchain][c]" Clang 210.0.0 Clang 21.1.8 "" "")
lws_vc_row("GNU C and GNU CXX both refused" "[toolchain][c];[toolchain][cxx]" GNU 15.2.0 GNU 15.2.0 "" "")

# lws_vc_cuda_row(<name> <expected_tags> <cuda_id> <cuda_version> <host_id> <host_version>)
# A row with the CUDA legs mandatory: clang 21 for C and CXX, nvcc major 13 expected.
function(lws_vc_cuda_row name expected cuda_id cuda_version host_id host_version)
    lws_clang21_verdict(_got
        C_ID Clang C_VERSION 21.1.8
        CXX_ID Clang CXX_VERSION 21.1.8
        CUDA CUDA_ID "${cuda_id}" CUDA_VERSION "${cuda_version}"
        CUDA_HOST_ID "${host_id}" CUDA_HOST_VERSION "${host_version}"
        EXPECTED_MAJOR 21 EXPECTED_CUDA_MAJOR 13)

    list(LENGTH _got _got_n)
    list(LENGTH expected _want_n)
    if(NOT _got_n EQUAL _want_n)
        message(FATAL_ERROR
            "toolchain verdict row '${name}': expected ${_want_n} refusal(s), got ${_got_n}: [${_got}]")
    endif()
    foreach(_tag IN LISTS expected)
        set(_hit FALSE)
        foreach(_reason IN LISTS _got)
            string(FIND "${_reason}" "${_tag}" _pos)
            if(NOT _pos EQUAL -1)
                set(_hit TRUE)
            endif()
        endforeach()
        if(NOT _hit)
            message(FATAL_ERROR
                "toolchain verdict row '${name}': no refusal carries ${_tag}; got [${_got}]")
        endif()
    endforeach()
    set_property(GLOBAL APPEND PROPERTY LWS_VC_ROW_NAMES "${name}")
endfunction()

# The CUDA host leg (CUDA configured with a GCC host).
lws_vc_row("CUDA host GNU 13.4.0 refused" "[toolchain][cuda-host]" Clang 21.1.8 Clang 21.1.8 GNU 13.4.0)
lws_vc_row("CUDA host clang 20 refused" "[toolchain][cuda-host]" Clang 21.1.8 Clang 21.1.8 Clang 20.1.8)

# The CUDA legs when CUDA is on: the compiler must be nvcc 13 and the host clang 21, and an empty identity refuses.
lws_vc_cuda_row("nvcc 13 with a clang 21 host holds" "" NVIDIA 13.4.92 Clang 21.1.8)
lws_vc_cuda_row("nvcc 13 with a GNU host refused" "[toolchain][cuda-host]" NVIDIA 13.4.92 GNU 15.2.0)
lws_vc_cuda_row("nvcc 13 with an empty host id refused" "[toolchain][cuda-host]" NVIDIA 13.4.92 "" "")
lws_vc_cuda_row("nvcc 12 refused" "[toolchain][cuda]" NVIDIA 12.4.131 Clang 21.1.8)
lws_vc_cuda_row("clang as the CUDA compiler refused" "[toolchain][cuda]" Clang 21.1.8 Clang 21.1.8)
lws_vc_cuda_row("empty CUDA compiler id refused" "[toolchain][cuda]" "" "" Clang 21.1.8)
lws_vc_cuda_row("nvcc 12 with a GNU host refused twice" "[toolchain][cuda];[toolchain][cuda-host]" NVIDIA 12.4.131 GNU 13.3.0)

get_property(_names GLOBAL PROPERTY LWS_VC_ROW_NAMES)
list(LENGTH _names _rows)
if(_rows EQUAL 0)
    message(FATAL_ERROR "toolchain_verdict_cases: no row ran (vacuous)")
endif()
message(STATUS "TOOLCHAIN-VERDICT-CASES-OK rows=${_rows}")
