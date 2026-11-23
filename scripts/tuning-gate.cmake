# scripts/tuning-gate.cmake: the tuning-values gate (GATE-02, success criterion 4).
#
#   cmake -DLWS_ROOT={root} -P scripts/tuning-gate.cmake
#
# Tuning values stay out of constexpr until a measured protocol commits them (D-14). The gate reads
# every header under include/lws as text and refuses a line that holds both the constexpr keyword and
# a tuning identifier. Each line is lowercased before the match, so a camel-case name such as the
# seeded ones is caught as well; the refusal prints the line as written. Nothing is compiled.
#
# A line without constexpr may use these words freely.
#
# Floor (non-vacuity): include/lws/options.hpp must exist and at least one header must be scanned,
# otherwise the scan proves nothing.
#
# Exit: 0 holds; 1 refusal; 2 missing input or a failed floor. A refusal outranks missing input.
cmake_minimum_required(VERSION 3.29)

if(NOT DEFINED LWS_ROOT OR "${LWS_ROOT}" STREQUAL "")
    message(FATAL_ERROR "tuning-gate: -DLWS_ROOT={root} is required")
endif()

# Lowercase on purpose: the match runs against the lowercased line.
set(LWS_TG_IDENTIFIERS
    threshold speech silence pre_roll overlap ceiling hangover onset max_speech pad)

set(LWS_TG_REFUSED 0)
set(LWS_TG_MISSING "")

set(LWS_TG_OPTIONS "${LWS_ROOT}/include/lws/options.hpp")
if(NOT EXISTS "${LWS_TG_OPTIONS}")
    list(APPEND LWS_TG_MISSING "${LWS_TG_OPTIONS} is absent")
endif()

file(GLOB_RECURSE LWS_TG_HEADERS "${LWS_ROOT}/include/lws/*.hpp")
list(LENGTH LWS_TG_HEADERS LWS_TG_COUNT)
if(LWS_TG_COUNT EQUAL 0)
    list(APPEND LWS_TG_MISSING "no header under ${LWS_ROOT}/include/lws was scanned (vacuous)")
endif()

foreach(_tg_header IN LISTS LWS_TG_HEADERS)
    file(RELATIVE_PATH _tg_rel "${LWS_ROOT}" "${_tg_header}")
    file(STRINGS "${_tg_header}" _tg_lines)
    set(_tg_n 0)
    foreach(_tg_line IN LISTS _tg_lines)
        math(EXPR _tg_n "${_tg_n} + 1")
        string(TOLOWER "${_tg_line}" _tg_lower)
        string(FIND "${_tg_lower}" "constexpr" _tg_cx)
        if(_tg_cx EQUAL -1)
            continue()
        endif()
        foreach(_tg_id IN LISTS LWS_TG_IDENTIFIERS)
            string(FIND "${_tg_lower}" "${_tg_id}" _tg_hit)
            if(NOT _tg_hit EQUAL -1)
                # Printed here, not collected: a list would split a line that holds a semicolon.
                message(STATUS "[tuning] ${_tg_rel}:${_tg_n}: constexpr tuning value: ${_tg_line}")
                math(EXPR LWS_TG_REFUSED "${LWS_TG_REFUSED} + 1")
                break()
            endif()
        endforeach()
    endforeach()
endforeach()

if(LWS_TG_REFUSED GREATER 0)
    message(STATUS "TUNING-REFUSED headers=${LWS_TG_COUNT} refusals=${LWS_TG_REFUSED}")
    cmake_language(EXIT 1)
endif()
if(LWS_TG_MISSING)
    foreach(_tg_msg IN LISTS LWS_TG_MISSING)
        message(STATUS "[tuning] missing input: ${_tg_msg}")
    endforeach()
    cmake_language(EXIT 2)
endif()
message(STATUS "TUNING-OK headers=${LWS_TG_COUNT} constexpr_tuning=0")
cmake_language(EXIT 0)
