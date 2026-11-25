# scripts/api-gate.cmake: the public header gate.
#
#   cmake -DLWS_ROOT={root} -P scripts/api-gate.cmake
#
# Every file under include/lws is byte-identical to the list in scripts/api-baseline.sha256, which holds the
# sha256 of each public header as it was at commit 2cb3149, and no file is added or removed. The gate hashes
# the files and compares; it needs no git history, so it holds in a shallow clone. It compares bytes and not
# declarations on purpose: a comment-only edit to a public header must be a conscious change too, made together
# with a new baseline and a CHANGELOG line.
# A refusal prints "[api] <path>: changed", "[api] <path>: added" or "[api] <path>: removed", one line per file.
#
# Floor (non-vacuity): the baseline exists, lists at least five files and lists include/lws/streamer.hpp,
# otherwise the comparison proves nothing about the library's entry point. A floor failure prints
# "[api] missing input: <reason>".
#
# Exit: 0 holds; 1 a header changed, was added or was removed; 2 missing input or a failed floor. A change
# outranks missing input.
cmake_minimum_required(VERSION 3.29)

if(NOT DEFINED LWS_ROOT OR "${LWS_ROOT}" STREQUAL "")
    message(FATAL_ERROR "api-gate: -DLWS_ROOT={root} is required")
endif()

set(LWS_API_BASELINE "${LWS_ROOT}/scripts/api-baseline.sha256")
set(LWS_API_MISSING "")
set(LWS_API_CHANGED 0)
set(LWS_API_PATHS "")

# Baseline: "<sha256>  <path>" lines; "#" lines and blank lines are skipped.
if(NOT EXISTS "${LWS_API_BASELINE}")
    list(APPEND LWS_API_MISSING "scripts/api-baseline.sha256 does not exist")
else()
    file(STRINGS "${LWS_API_BASELINE}" LWS_API_LINES)
    foreach(_api_line IN LISTS LWS_API_LINES)
        if(_api_line STREQUAL "" OR _api_line MATCHES "^#")
            continue()
        endif()
        if(NOT _api_line MATCHES "^([0-9a-f]+)  (include/lws/.+)$")
            list(APPEND LWS_API_MISSING "malformed baseline line: ${_api_line}")
            continue()
        endif()
        list(APPEND LWS_API_PATHS "${CMAKE_MATCH_2}")
        set(LWS_API_HASH_${CMAKE_MATCH_2} "${CMAKE_MATCH_1}")
    endforeach()
endif()

list(LENGTH LWS_API_PATHS LWS_API_COUNT)
if(LWS_API_COUNT LESS 5)
    list(APPEND LWS_API_MISSING "baseline lists ${LWS_API_COUNT} files, at least 5 expected (vacuous)")
endif()
if(NOT "include/lws/streamer.hpp" IN_LIST LWS_API_PATHS)
    list(APPEND LWS_API_MISSING "baseline has no include/lws/streamer.hpp entry (vacuous)")
endif()

# GLOB_RECURSE: every file under include/lws, of any type, at any depth.
file(GLOB_RECURSE LWS_API_FILES LIST_DIRECTORIES false "${LWS_ROOT}/include/lws/*")
set(LWS_API_SEEN "")
foreach(_api_file IN LISTS LWS_API_FILES)
    file(RELATIVE_PATH _api_rel "${LWS_ROOT}" "${_api_file}")
    list(APPEND LWS_API_SEEN "${_api_rel}")
    if(NOT _api_rel IN_LIST LWS_API_PATHS)
        message(STATUS "[api] ${_api_rel}: added")
        math(EXPR LWS_API_CHANGED "${LWS_API_CHANGED} + 1")
        continue()
    endif()
    file(SHA256 "${_api_file}" _api_hash)
    if(NOT _api_hash STREQUAL "${LWS_API_HASH_${_api_rel}}")
        message(STATUS "[api] ${_api_rel}: changed")
        math(EXPR LWS_API_CHANGED "${LWS_API_CHANGED} + 1")
    endif()
endforeach()

foreach(_api_path IN LISTS LWS_API_PATHS)
    if(NOT _api_path IN_LIST LWS_API_SEEN)
        message(STATUS "[api] ${_api_path}: removed")
        math(EXPR LWS_API_CHANGED "${LWS_API_CHANGED} + 1")
    endif()
endforeach()

if(LWS_API_CHANGED GREATER 0)
    message(STATUS "API-CHANGED n=${LWS_API_CHANGED}")
    cmake_language(EXIT 1)
endif()
if(LWS_API_MISSING)
    foreach(_api_msg IN LISTS LWS_API_MISSING)
        message(STATUS "[api] missing input: ${_api_msg}")
    endforeach()
    cmake_language(EXIT 2)
endif()
message(STATUS "API-OK headers=${LWS_API_COUNT}")
cmake_language(EXIT 0)
