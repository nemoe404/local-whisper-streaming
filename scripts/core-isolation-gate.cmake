# scripts/core-isolation-gate.cmake: the pure-core isolation gate (ROADMAP success criterion 5, GATE-02
# companion, T-02-10).
#
#   cmake -DLWS_ROOT={root} [-DLWS_CLOSURE_FILE={path}] -P scripts/core-isolation-gate.cmake
#
# The core stays free of whisper, ggml, spdlog and threads. Two checks:
#
#   include scan (always)  Every header under include/lws and every file under src/core is read as text.
#                          A line that includes whisper, ggml, spdlog, pthread or a standard thread header
#                          (thread, mutex, condition_variable, future, stop_token, semaphore, latch, barrier)
#                          is refused.
#   link closure           Only with -DLWS_CLOSURE_FILE. The file holds one name per line: every name in
#                          lws_core's link closure, written at configure time by tests/CMakeLists.txt. A
#                          name that matches whisper, ggml, spdlog, Threads::Threads or pthread is refused.
#
# Floor (non-vacuity): fewer than three scanned files, or a closure file that is missing or empty, is a
# missing input and not a pass.
#
# Exit: 0 holds; 1 refusal; 2 missing input or a failed floor. A refusal outranks missing input.
cmake_minimum_required(VERSION 3.29)

if(NOT DEFINED LWS_ROOT OR "${LWS_ROOT}" STREQUAL "")
    message(FATAL_ERROR "core-isolation-gate: -DLWS_ROOT={root} is required")
endif()

set(LWS_ISO_REFUSED 0)
set(LWS_ISO_MISSING "")

file(GLOB_RECURSE LWS_ISO_FILES
    "${LWS_ROOT}/include/lws/*.hpp"
    "${LWS_ROOT}/src/core/*.cpp"
    "${LWS_ROOT}/src/core/*.hpp")
list(LENGTH LWS_ISO_FILES LWS_ISO_COUNT)
if(LWS_ISO_COUNT LESS 3)
    list(APPEND LWS_ISO_MISSING "only ${LWS_ISO_COUNT} file(s) scanned, at least 3 expected (vacuous)")
endif()

set(LWS_ISO_INCLUDE_PATTERN
    "^[ \t]*#[ \t]*include[ \t]*[<\"](whisper[^>\"]*|ggml[^>\"]*|spdlog[^>\"]*|thread|mutex|condition_variable|future|stop_token|semaphore|latch|barrier|pthread[^>\"]*)[>\"]")

foreach(_iso_file IN LISTS LWS_ISO_FILES)
    file(RELATIVE_PATH _iso_rel "${LWS_ROOT}" "${_iso_file}")
    file(STRINGS "${_iso_file}" _iso_lines)
    set(_iso_n 0)
    foreach(_iso_line IN LISTS _iso_lines)
        math(EXPR _iso_n "${_iso_n} + 1")
        if(_iso_line MATCHES "${LWS_ISO_INCLUDE_PATTERN}")
            message(STATUS "[isolation] include ${_iso_rel}:${_iso_n}: forbidden include")
            math(EXPR LWS_ISO_REFUSED "${LWS_ISO_REFUSED} + 1")
        endif()
    endforeach()
endforeach()

set(LWS_ISO_CLOSURE_COUNT 0)
if(DEFINED LWS_CLOSURE_FILE AND NOT "${LWS_CLOSURE_FILE}" STREQUAL "")
    if(NOT EXISTS "${LWS_CLOSURE_FILE}")
        list(APPEND LWS_ISO_MISSING "closure file ${LWS_CLOSURE_FILE} is absent")
    else()
        file(STRINGS "${LWS_CLOSURE_FILE}" _iso_names)
        list(LENGTH _iso_names LWS_ISO_CLOSURE_COUNT)
        if(LWS_ISO_CLOSURE_COUNT EQUAL 0)
            list(APPEND LWS_ISO_MISSING "closure file ${LWS_CLOSURE_FILE} is empty (vacuous)")
        endif()
        foreach(_iso_name IN LISTS _iso_names)
            if(_iso_name MATCHES "whisper|ggml|spdlog|Threads::Threads|pthread")
                message(STATUS "[isolation] link closure of lws_core contains ${_iso_name}")
                math(EXPR LWS_ISO_REFUSED "${LWS_ISO_REFUSED} + 1")
            endif()
        endforeach()
    endif()
endif()

if(LWS_ISO_REFUSED GREATER 0)
    message(STATUS "ISOLATION-REFUSED headers_and_sources=${LWS_ISO_COUNT} refusals=${LWS_ISO_REFUSED}")
    cmake_language(EXIT 1)
endif()
if(LWS_ISO_MISSING)
    foreach(_iso_msg IN LISTS LWS_ISO_MISSING)
        message(STATUS "[isolation] missing input: ${_iso_msg}")
    endforeach()
    cmake_language(EXIT 2)
endif()
message(STATUS "ISOLATION-OK headers_and_sources=${LWS_ISO_COUNT} closure=${LWS_ISO_CLOSURE_COUNT}")
cmake_language(EXIT 0)
