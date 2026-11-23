# scripts/ring-gate.cmake: the ring wait-free gate (RING-03, success criterion 3).
#
#   cmake -DLWS_ROOT={root} -P scripts/ring-gate.cmake
#
# The ring's push never loops on a failed update and never waits: no compare-and-swap, no fetch-style
# read-modify-write, plain slots, no blocking primitive. The gate reads include/lws/spsc_ring.hpp as text
# and refuses a line that holds one of the refused tokens (plain, case-sensitive substrings). The refusal
# prints the matched text and the token. Comment lines are scanned too, so the header describes the design
# in other words. Nothing is compiled. exchange alone is allowed: the consumer clears the flag with it.
#
# Floor (non-vacuity): the header must exist and hold a class SpscRing line, otherwise the scan proves
# nothing.
#
# Exit: 0 holds; 1 refusal; 2 missing input or a failed floor. A refusal outranks missing input.
cmake_minimum_required(VERSION 3.29)

if(NOT DEFINED LWS_ROOT OR "${LWS_ROOT}" STREQUAL "")
    message(FATAL_ERROR "ring-gate: -DLWS_ROOT={root} is required")
endif()

# Each token is quoted: an unquoted ( would end the set() call early.
set(LWS_RG_TOKENS
    "compare_exchange"
    "fetch_"
    "atomic<float>"
    "mutex"
    "lock_guard"
    "unique_lock"
    "scoped_lock"
    "condition_variable"
    ".wait("
    "notify_one"
    "notify_all")

set(LWS_RG_REFUSED 0)
set(LWS_RG_MISSING "")
set(LWS_RG_PIECES 0)
set(LWS_RG_HAS_CLASS FALSE)

set(LWS_RG_HEADER "${LWS_ROOT}/include/lws/spsc_ring.hpp")
if(NOT EXISTS "${LWS_RG_HEADER}")
    list(APPEND LWS_RG_MISSING "include/lws/spsc_ring.hpp is absent")
else()
    file(STRINGS "${LWS_RG_HEADER}" _rg_pieces)
    foreach(_rg_piece IN LISTS _rg_pieces)
        math(EXPR LWS_RG_PIECES "${LWS_RG_PIECES} + 1")
        string(FIND "${_rg_piece}" "class SpscRing" _rg_cls)
        if(NOT _rg_cls EQUAL -1)
            set(LWS_RG_HAS_CLASS TRUE)
        endif()
        foreach(_rg_token IN LISTS LWS_RG_TOKENS)
            string(FIND "${_rg_piece}" "${_rg_token}" _rg_hit)
            if(NOT _rg_hit EQUAL -1)
                # Printed here, not collected: file(STRINGS) splits at semicolons and a list would split again.
                message(STATUS "[ring] include/lws/spsc_ring.hpp: refused token '${_rg_token}': ${_rg_piece}")
                math(EXPR LWS_RG_REFUSED "${LWS_RG_REFUSED} + 1")
                break()
            endif()
        endforeach()
    endforeach()
    if(NOT LWS_RG_HAS_CLASS)
        list(APPEND LWS_RG_MISSING "include/lws/spsc_ring.hpp holds no 'class SpscRing' line (vacuous)")
    endif()
endif()

if(LWS_RG_REFUSED GREATER 0)
    message(STATUS "RING-REFUSED refusals=${LWS_RG_REFUSED}")
    cmake_language(EXIT 1)
endif()
if(LWS_RG_MISSING)
    foreach(_rg_msg IN LISTS LWS_RG_MISSING)
        message(STATUS "[ring] missing input: ${_rg_msg}")
    endforeach()
    cmake_language(EXIT 2)
endif()
list(LENGTH LWS_RG_TOKENS _rg_ntokens)
message(STATUS "RING-OK tokens=${_rg_ntokens} pieces=${LWS_RG_PIECES}")
cmake_language(EXIT 0)
