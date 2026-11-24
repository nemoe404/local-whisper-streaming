# scripts/threads-gate.cmake: the thread text gate.
#
#   cmake -DLWS_ROOT={root} -P scripts/threads-gate.cmake
#
# No recursive mutex anywhere in the project's C++ files, and the library starts threads only as jthreads,
# which join on destruction and stop through a stop token. The gate reads every .cpp, .hpp and .h under
# src/, include/ and tests/ as text and applies two rules:
#   recursive mutex  every scanned file: a line naming a recursive mutex type or a recursive timed mutex
#                    type is refused (plain, case-sensitive substrings)
#   raw thread       files under src/ and include/ only: a line constructing a plain std::thread is refused.
#                    The this_thread namespace and the hardware_concurrency query are allowed.
# Comment lines are scanned too, so source describes the rule in other words. Nothing is compiled.
# tests/gates/ and scripts/ are not scanned: the seeded proofs and the gate itself must name the tokens.
# A refusal prints "[threads] <path>:<line>: refused <rule>: <line>".
#
# Floor (non-vacuity): at least five files are scanned, and src/shell/streamer_impl.cpp is among them and
# holds a std::jthread line, otherwise the scan proves nothing about the streamer.
#
# Exit: 0 holds; 1 refusal; 2 missing input or a failed floor. A refusal outranks missing input.
cmake_minimum_required(VERSION 3.29)

if(NOT DEFINED LWS_ROOT OR "${LWS_ROOT}" STREQUAL "")
    message(FATAL_ERROR "threads-gate: -DLWS_ROOT={root} is required")
endif()

# Each token is quoted, as in the other text gates.
set(LWS_TG_RECURSIVE_TOKENS
    "recursive_mutex"
    "recursive_timed_mutex")

# GLOB_RECURSE: a plain GLOB does not descend into src/shell/ or include/lws/.
file(GLOB_RECURSE LWS_TG_ALL
    "${LWS_ROOT}/src/*.cpp" "${LWS_ROOT}/src/*.hpp" "${LWS_ROOT}/src/*.h"
    "${LWS_ROOT}/include/*.cpp" "${LWS_ROOT}/include/*.hpp" "${LWS_ROOT}/include/*.h"
    "${LWS_ROOT}/tests/*.cpp" "${LWS_ROOT}/tests/*.hpp" "${LWS_ROOT}/tests/*.h")

set(LWS_TG_FILES "")
foreach(_tg_candidate IN LISTS LWS_TG_ALL)
    file(RELATIVE_PATH _tg_cand_rel "${LWS_ROOT}" "${_tg_candidate}")
    if(NOT _tg_cand_rel MATCHES "^tests/gates/")
        list(APPEND LWS_TG_FILES "${_tg_candidate}")
    endif()
endforeach()

set(LWS_TG_REFUSED 0)
set(LWS_TG_MISSING "")
set(LWS_TG_SCANNED 0)
set(LWS_TG_STREAMER_FILE "${LWS_ROOT}/src/shell/streamer_impl.cpp")
set(LWS_TG_HAS_JTHREAD FALSE)

foreach(_tg_file IN LISTS LWS_TG_FILES)
    math(EXPR LWS_TG_SCANNED "${LWS_TG_SCANNED} + 1")
    file(RELATIVE_PATH _tg_rel "${LWS_ROOT}" "${_tg_file}")
    set(_tg_library FALSE)
    if(_tg_rel MATCHES "^(src|include)/")
        set(_tg_library TRUE)
    endif()
    file(READ "${_tg_file}" _tg_text)
    # A semicolon or bracket in the text would split or merge list elements, so they are swapped for
    # markers first. Every line is wrapped in a leading ">" and a trailing "<": no line is an empty list
    # element (which would shift the line numbers), and both ends are non-word characters for the
    # patterns below.
    string(REPLACE ";" "@SEMI@" _tg_text "${_tg_text}")
    string(REPLACE "[" "@LB@" _tg_text "${_tg_text}")
    string(REPLACE "]" "@RB@" _tg_text "${_tg_text}")
    string(REPLACE "\n" "<\n>" _tg_text ">${_tg_text}")
    string(REPLACE "\n" ";" _tg_lines "${_tg_text}")

    set(_tg_number 0)
    foreach(_tg_line IN LISTS _tg_lines)
        math(EXPR _tg_number "${_tg_number} + 1")
        set(_tg_rule "")
        foreach(_tg_token IN LISTS LWS_TG_RECURSIVE_TOKENS)
            string(FIND "${_tg_line}" "${_tg_token}" _tg_hit)
            if(NOT _tg_hit EQUAL -1)
                set(_tg_rule "recursive mutex '${_tg_token}'")
                break()
            endif()
        endforeach()
        if(_tg_rule STREQUAL "" AND _tg_library AND _tg_line MATCHES "std::thread[^_A-Za-z0-9:]")
            set(_tg_rule "raw std::thread")
        endif()
        if(NOT _tg_rule STREQUAL "")
            string(LENGTH "${_tg_line}" _tg_len)
            math(EXPR _tg_inner "${_tg_len} - 2")
            string(SUBSTRING "${_tg_line}" 1 ${_tg_inner} _tg_shown)
            string(REPLACE "@SEMI@" ";" _tg_shown "${_tg_shown}")
            string(REPLACE "@LB@" "[" _tg_shown "${_tg_shown}")
            string(REPLACE "@RB@" "]" _tg_shown "${_tg_shown}")
            # Printed here, not collected: a list would split a line at its semicolons.
            message(STATUS "[threads] ${_tg_rel}:${_tg_number}: refused ${_tg_rule}: ${_tg_shown}")
            math(EXPR LWS_TG_REFUSED "${LWS_TG_REFUSED} + 1")
        endif()
        if("${_tg_file}" STREQUAL "${LWS_TG_STREAMER_FILE}")
            string(FIND "${_tg_line}" "std::jthread" _tg_jt)
            if(NOT _tg_jt EQUAL -1)
                set(LWS_TG_HAS_JTHREAD TRUE)
            endif()
        endif()
    endforeach()
endforeach()

if(LWS_TG_SCANNED LESS 5)
    list(APPEND LWS_TG_MISSING "only ${LWS_TG_SCANNED} files scanned, at least 5 expected (vacuous)")
endif()
if(NOT LWS_TG_STREAMER_FILE IN_LIST LWS_TG_FILES)
    list(APPEND LWS_TG_MISSING "src/shell/streamer_impl.cpp is not among the scanned files")
elseif(NOT LWS_TG_HAS_JTHREAD)
    list(APPEND LWS_TG_MISSING "src/shell/streamer_impl.cpp holds no std::jthread line (vacuous)")
endif()

if(LWS_TG_REFUSED GREATER 0)
    message(STATUS "THREADS-REFUSED refusals=${LWS_TG_REFUSED}")
    cmake_language(EXIT 1)
endif()
if(LWS_TG_MISSING)
    foreach(_tg_msg IN LISTS LWS_TG_MISSING)
        message(STATUS "[threads] missing input: ${_tg_msg}")
    endforeach()
    cmake_language(EXIT 2)
endif()
message(STATUS "THREADS-OK files=${LWS_TG_SCANNED} rules=2")
cmake_language(EXIT 0)
