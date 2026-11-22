# tests/gates/isa_verdict_cases.cmake: script-mode rows for the ISA verdict (BLD-01, success criterion 2).
#
#   cmake -DLWS_ROOT={root} -P tests/gates/isa_verdict_cases.cmake
#
# Three families, each row proven red or green on its own: lws_isa_flag_refusal (one option),
# lws_ggml_isa_verdict (a whole option list, with the non-vacuity rule) and lws_select_cpu_baseline.
# Any mismatch is a FATAL_ERROR naming the row, so the script exits 1. ctest registers it as
# isa_verdict_cases (label gate).
cmake_minimum_required(VERSION 3.29)

if(NOT DEFINED LWS_ROOT OR "${LWS_ROOT}" STREQUAL "")
    message(FATAL_ERROR "isa_verdict_cases: -DLWS_ROOT={root} is required")
endif()
include(${LWS_ROOT}/cmake/CpuBaseline.cmake)

# lws_ic_option(<option> <refused: TRUE|FALSE>): one option, refused or accepted by the single-flag rule.
function(lws_ic_option option refused)
    lws_isa_flag_refusal(_reason "${option}")
    if(refused)
        if(NOT "${_reason}" MATCHES "^\\[isa\\] ")
            message(FATAL_ERROR "isa verdict row '${option}': expected a refusal, got accepted")
        endif()
    else()
        if(NOT "${_reason}" STREQUAL "")
            message(FATAL_ERROR "isa verdict row '${option}': expected accepted, got [${_reason}]")
        endif()
    endif()
    set_property(GLOBAL APPEND PROPERTY LWS_IC_ROW_NAMES "option ${option}")
endfunction()

# lws_ic_list(<name> <expected-substring or "">) : a whole option list through lws_ggml_isa_verdict.
# "" expects the baseline to hold; otherwise the reason text must contain the substring.
function(lws_ic_list name expected)
    lws_ggml_isa_verdict(_reasons ${ARGN})
    if("${expected}" STREQUAL "")
        if(NOT "${_reasons}" STREQUAL "")
            message(FATAL_ERROR "isa verdict row '${name}': expected the baseline to hold, got [${_reasons}]")
        endif()
    else()
        string(FIND "${_reasons}" "${expected}" _pos)
        if(_pos EQUAL -1)
            message(FATAL_ERROR "isa verdict row '${name}': expected a reason with '${expected}', got [${_reasons}]")
        endif()
    endif()
    set_property(GLOBAL APPEND PROPERTY LWS_IC_ROW_NAMES "list ${name}")
endfunction()

# lws_ic_baseline(<name> <expected-list> <expected-reason-substring> <name-arg>)
function(lws_ic_baseline name expected_list expected_reason selected)
    lws_select_cpu_baseline(_list _reason "${selected}")
    if(NOT "${_list}" STREQUAL "${expected_list}")
        message(FATAL_ERROR "isa baseline row '${name}': expected list [${expected_list}], got [${_list}]")
    endif()
    if("${expected_reason}" STREQUAL "")
        if(NOT "${_reason}" STREQUAL "")
            message(FATAL_ERROR "isa baseline row '${name}': expected no reason, got '${_reason}'")
        endif()
    else()
        string(FIND "${_reason}" "${expected_reason}" _pos)
        if(_pos EQUAL -1)
            message(FATAL_ERROR "isa baseline row '${name}': expected reason '${expected_reason}', got '${_reason}'")
        endif()
    endif()
    set_property(GLOBAL APPEND PROPERTY LWS_IC_ROW_NAMES "baseline ${name}")
endfunction()

# --- one option: refused -------------------------------------------------------------------------
lws_ic_option("-march=native" TRUE)
lws_ic_option("-march=x86-64-v3" TRUE)
lws_ic_option("-mtune=native" TRUE)
lws_ic_option("-mcpu=haswell" TRUE)
lws_ic_option("-mavx512f" TRUE)
lws_ic_option("-mavxvnni" TRUE)
lws_ic_option("-mamx-tile" TRUE)
lws_ic_option("$<$<COMPILE_LANGUAGE:C>:-march=native>" TRUE)

# --- one option: accepted ------------------------------------------------------------------------
lws_ic_option("-mavx2" FALSE)
lws_ic_option("-mfma" FALSE)
lws_ic_option("-msse4.2" FALSE)
lws_ic_option("-mf16c" FALSE)
lws_ic_option("-DX-march=native" FALSE)

# --- a whole option list -------------------------------------------------------------------------
set(_baseline -msse4.2 -mavx -mavx2 -mfma -mf16c -mbmi2)
lws_ic_list("the six baseline flags hold" "" ${_baseline})
lws_ic_list("the baseline without -mavx2 refuses (non-vacuity)" "no -mavx2" -msse4.2 -mavx -mfma -mf16c -mbmi2)
lws_ic_list("the baseline plus -march=native refuses" "host/arch flag -march=native" ${_baseline} -march=native)
lws_ic_list("the baseline plus -mavx512f refuses" "above-baseline flag -mavx512f" ${_baseline} -mavx512f)

# --- the baseline selector -----------------------------------------------------------------------
lws_ic_baseline("x86-64-v3 yields the six ISA names" "SSE42;AVX;AVX2;FMA;F16C;BMI2" "" "x86-64-v3")
lws_ic_baseline("an empty name is unknown" "" "[isa] unknown LWS_CPU_BASELINE" "")
lws_ic_baseline("native is unknown" "" "[isa] unknown LWS_CPU_BASELINE" "native")

get_property(_names GLOBAL PROPERTY LWS_IC_ROW_NAMES)
list(LENGTH _names _rows)
if(_rows EQUAL 0)
    message(FATAL_ERROR "isa_verdict_cases: no row ran (vacuous)")
endif()
message(STATUS "ISA-VERDICT-CASES-OK rows=${_rows}")
