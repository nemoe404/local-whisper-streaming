# scripts/readme-gate.cmake: the README numbers gate.
#
#   cmake -DLWS_ROOT={root} -P scripts/readme-gate.cmake
#
# Every number with a unit in README.md is either a row of the Measured table, with the machine, the models, the
# commits, the command and the date it was taken, or a design constant that says so. The gate checks:
#
#   - the "## Measured" heading is followed by a table whose header is exactly
#     "Id | What | Value | Machine | Model | lws commit | fork commit | Command | Date";
#   - every row has nine non-empty cells and a unique Id of the form M<number>;
#   - the lws commit resolves to a commit in this repository and is an ancestor of HEAD;
#   - the fork commit is a prefix of the commit the third_party/whisper.cpp gitlink holds at HEAD;
#   - the date is YYYY-MM-DD;
#   - outside the table and outside fenced code blocks, a line holding a number followed by a unit (ms, s, min,
#     minutes, seconds, MB, MiB, GB, GiB, KB, KiB, % or x) names a row of the table as M<number> or carries the
#     marker <!-- const -->, which says the number is a design constant and not a measurement.
#
# What it cannot prove: that a cell's value is true. The row is the trace; a reader checks it against the command.
#
# Floor (non-vacuity): the README exists, has the Measured heading and the table has at least five rows. A
# shallow clone cannot resolve the commits, so it is reported as missing input at once and never as a pass.
# Missing input prints "[readme] missing input: <reason>". A refusal prints "[readme] README.md:<line>: <reason>".
#
# Exit: 0 holds (prints README-OK rows=<n>); 1 refused (prints README-REFUSED n=<n>); 2 missing input. A refusal
# outranks missing input.
cmake_minimum_required(VERSION 3.29)

if(NOT DEFINED LWS_ROOT OR "${LWS_ROOT}" STREQUAL "")
    message(FATAL_ERROR "readme-gate: -DLWS_ROOT={root} is required")
endif()

set(LWS_README_FILE "${LWS_ROOT}/README.md")
set(LWS_README_HEADER "| Id | What | Value | Machine | Model | lws commit | fork commit | Command | Date |")
set(LWS_README_MARKER "<!-- const -->")
set(LWS_README_MIN_ROWS 5)
set(LWS_README_UNIT_REGEX
    "(^|[^A-Za-z0-9_.])[0-9]+([.][0-9]+)? ?(ms|s|min|minutes|seconds|MB|MiB|GB|GiB|KB|KiB|%|x)([^A-Za-z0-9_]|$)")

# A shallow clone first: with the history cut, no commit cell can be resolved, and every row would be refused
# for a reason that is not the row's.
execute_process(COMMAND git -C "${LWS_ROOT}" rev-parse --is-shallow-repository
    OUTPUT_VARIABLE LWS_README_SHALLOW RESULT_VARIABLE LWS_README_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT LWS_README_GIT_STATUS EQUAL 0)
    message(STATUS "[readme] missing input: ${LWS_ROOT} is not a git work tree")
    cmake_language(EXIT 2)
endif()
if("${LWS_README_SHALLOW}" STREQUAL "true")
    message(STATUS "[readme] missing input: shallow clone, the measured commits cannot be resolved")
    cmake_language(EXIT 2)
endif()
if(NOT EXISTS "${LWS_README_FILE}")
    message(STATUS "[readme] missing input: README.md does not exist")
    cmake_language(EXIT 2)
endif()

# Lines are cut by hand into numbered variables. A list split would drop empty lines, which would shift the
# printed line numbers, and would break on the semicolons and brackets the table holds.
file(READ "${LWS_README_FILE}" LWS_README_TEXT)
set(LWS_README_COUNT 0)
set(_readme_rest "${LWS_README_TEXT}")
while(NOT "${_readme_rest}" STREQUAL "")
    string(FIND "${_readme_rest}" "\n" _readme_nl)
    if(_readme_nl EQUAL -1)
        set(_readme_line "${_readme_rest}")
        set(_readme_rest "")
    else()
        string(SUBSTRING "${_readme_rest}" 0 ${_readme_nl} _readme_line)
        math(EXPR _readme_next "${_readme_nl} + 1")
        string(SUBSTRING "${_readme_rest}" ${_readme_next} -1 _readme_rest)
    endif()
    math(EXPR LWS_README_COUNT "${LWS_README_COUNT} + 1")
    set(LWS_README_LINE_${LWS_README_COUNT} "${_readme_line}")
endwhile()

set(LWS_README_REFUSED 0)
set(LWS_README_MISSING "")

# refuse(line, reason, text): print one refusal and count it.
function(lws_readme_refuse number reason text)
    message(STATUS "[readme] README.md:${number}: ${reason}: ${text}")
    math(EXPR _count "${LWS_README_REFUSED} + 1")
    set(LWS_README_REFUSED ${_count} PARENT_SCOPE)
endfunction()

# split_row(row): LWS_CELL_COUNT and LWS_CELL_1 ... of a "| a | b |" row, each cell trimmed. A row that does not
# begin and end with a bar has no cells.
function(lws_readme_split_row row)
    string(STRIP "${row}" _text)
    string(LENGTH "${_text}" _len)
    set(LWS_CELL_COUNT 0 PARENT_SCOPE)
    if(_len LESS 2)
        return()
    endif()
    math(EXPR _last_index "${_len} - 1")
    string(SUBSTRING "${_text}" 0 1 _first)
    string(SUBSTRING "${_text}" ${_last_index} 1 _last)
    if(NOT "${_first}" STREQUAL "|" OR NOT "${_last}" STREQUAL "|")
        return()
    endif()
    math(EXPR _inner_len "${_len} - 2")
    string(SUBSTRING "${_text}" 1 ${_inner_len} _inner)
    set(_cells_rest "${_inner}|")
    set(_cells_count 0)
    while(NOT "${_cells_rest}" STREQUAL "")
        string(FIND "${_cells_rest}" "|" _bar)
        string(SUBSTRING "${_cells_rest}" 0 ${_bar} _cell)
        math(EXPR _after "${_bar} + 1")
        string(SUBSTRING "${_cells_rest}" ${_after} -1 _cells_rest)
        string(STRIP "${_cell}" _cell)
        math(EXPR _cells_count "${_cells_count} + 1")
        set(LWS_CELL_${_cells_count} "${_cell}" PARENT_SCOPE)
    endwhile()
    set(LWS_CELL_COUNT ${_cells_count} PARENT_SCOPE)
endfunction()

# The Measured heading, then the first table line after it.
set(LWS_README_HEADING 0)
foreach(_i RANGE 1 ${LWS_README_COUNT})
    if("${LWS_README_LINE_${_i}}" STREQUAL "## Measured")
        set(LWS_README_HEADING ${_i})
        break()
    endif()
endforeach()

set(LWS_README_TABLE_FIRST 0)
set(LWS_README_TABLE_LAST 0)
set(LWS_README_IDS "")
set(LWS_README_ROWS 0)
if(LWS_README_HEADING EQUAL 0)
    list(APPEND LWS_README_MISSING "README.md has no \"## Measured\" heading")
else()
    math(EXPR _readme_from "${LWS_README_HEADING} + 1")
    if(_readme_from LESS_EQUAL LWS_README_COUNT)
        foreach(_i RANGE ${_readme_from} ${LWS_README_COUNT})
            if("${LWS_README_LINE_${_i}}" MATCHES "^\\|")
                set(LWS_README_TABLE_FIRST ${_i})
                break()
            endif()
        endforeach()
    endif()
    if(LWS_README_TABLE_FIRST EQUAL 0)
        list(APPEND LWS_README_MISSING "the Measured section has no table")
    endif()
endif()

if(NOT LWS_README_TABLE_FIRST EQUAL 0)
    if(NOT "${LWS_README_LINE_${LWS_README_TABLE_FIRST}}" STREQUAL "${LWS_README_HEADER}")
        lws_readme_refuse(${LWS_README_TABLE_FIRST} "header" "${LWS_README_LINE_${LWS_README_TABLE_FIRST}}")
    endif()
    math(EXPR _readme_sep "${LWS_README_TABLE_FIRST} + 1")
    if(NOT "${LWS_README_LINE_${_readme_sep}}" MATCHES "^\\|[-| :]+\\|$")
        lws_readme_refuse(${_readme_sep} "separator" "${LWS_README_LINE_${_readme_sep}}")
    endif()
    set(LWS_README_TABLE_LAST ${_readme_sep})

    # The gitlink's commit, once: every fork commit cell is a prefix of it.
    execute_process(COMMAND git -C "${LWS_ROOT}" ls-tree HEAD third_party/whisper.cpp
        OUTPUT_VARIABLE LWS_README_GITLINK_LINE RESULT_VARIABLE LWS_README_LSTREE_STATUS ERROR_QUIET)
    set(LWS_README_GITLINK "")
    if(LWS_README_LSTREE_STATUS EQUAL 0 AND "${LWS_README_GITLINK_LINE}" MATCHES "commit ([0-9a-f]+)")
        set(LWS_README_GITLINK "${CMAKE_MATCH_1}")
    else()
        list(APPEND LWS_README_MISSING "git ls-tree HEAD third_party/whisper.cpp holds no gitlink")
    endif()

    math(EXPR _readme_data "${_readme_sep} + 1")
    if(_readme_data GREATER LWS_README_COUNT)
        set(_readme_data ${LWS_README_COUNT})
    endif()
    foreach(_i RANGE ${_readme_data} ${LWS_README_COUNT})
        set(_row "${LWS_README_LINE_${_i}}")
        if(NOT "${_row}" MATCHES "^\\|")
            break()
        endif()
        set(LWS_README_TABLE_LAST ${_i})
        math(EXPR LWS_README_ROWS "${LWS_README_ROWS} + 1")
        lws_readme_split_row("${_row}")
        set(_cells_ok TRUE)
        if(NOT LWS_CELL_COUNT EQUAL 9)
            set(_cells_ok FALSE)
        else()
            foreach(_c RANGE 1 9)
                if("${LWS_CELL_${_c}}" STREQUAL "")
                    set(_cells_ok FALSE)
                endif()
            endforeach()
        endif()
        if(NOT _cells_ok)
            lws_readme_refuse(${_i} "cells" "a row needs nine non-empty cells: ${_row}")
            continue()
        endif()

        set(_id "${LWS_CELL_1}")
        if(NOT "${_id}" MATCHES "^M[0-9]+$")
            lws_readme_refuse(${_i} "id" "${_id} is not M<number>")
        elseif("${_id}" IN_LIST LWS_README_IDS)
            lws_readme_refuse(${_i} "id" "${_id} appears twice")
        else()
            list(APPEND LWS_README_IDS "${_id}")
        endif()

        string(REPLACE "`" "" _lws "${LWS_CELL_6}")
        string(REPLACE "`" "" _fork "${LWS_CELL_7}")
        string(LENGTH "${_lws}" _lws_len)
        if(NOT "${_lws}" MATCHES "^[0-9a-f]+$" OR _lws_len LESS 7 OR _lws_len GREATER 40)
            lws_readme_refuse(${_i} "lws commit" "${_lws} is not 7 to 40 hex characters")
        else()
            execute_process(COMMAND git -C "${LWS_ROOT}" cat-file -e "${_lws}^{commit}"
                RESULT_VARIABLE _resolved OUTPUT_QUIET ERROR_QUIET)
            if(NOT _resolved EQUAL 0)
                lws_readme_refuse(${_i} "lws commit does not resolve" "${_lws}")
            else()
                execute_process(COMMAND git -C "${LWS_ROOT}" merge-base --is-ancestor "${_lws}" HEAD
                    RESULT_VARIABLE _ancestor OUTPUT_QUIET ERROR_QUIET)
                if(NOT _ancestor EQUAL 0)
                    lws_readme_refuse(${_i} "lws commit is not an ancestor of HEAD" "${_lws}")
                endif()
            endif()
        endif()

        string(LENGTH "${_fork}" _fork_len)
        if(NOT "${_fork}" MATCHES "^[0-9a-f]+$" OR _fork_len LESS 7)
            lws_readme_refuse(${_i} "fork commit" "${_fork} is not at least 7 hex characters")
        elseif(NOT "${LWS_README_GITLINK}" STREQUAL "")
            string(FIND "${LWS_README_GITLINK}" "${_fork}" _fork_at)
            if(NOT _fork_at EQUAL 0)
                lws_readme_refuse(${_i} "fork commit is not the gitlink's" "${_fork} against ${LWS_README_GITLINK}")
            endif()
        endif()

        if(NOT "${LWS_CELL_9}" MATCHES "^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$")
            lws_readme_refuse(${_i} "date" "${LWS_CELL_9} is not YYYY-MM-DD")
        endif()
    endforeach()

    if(LWS_README_ROWS LESS LWS_README_MIN_ROWS)
        list(APPEND LWS_README_MISSING
            "the table has ${LWS_README_ROWS} rows, at least ${LWS_README_MIN_ROWS} expected (vacuous)")
    endif()
endif()

# Prose: every line outside the table and outside fenced code blocks.
set(LWS_README_IN_FENCE FALSE)
foreach(_i RANGE 1 ${LWS_README_COUNT})
    set(_line "${LWS_README_LINE_${_i}}")
    if("${_line}" MATCHES "^```")
        if(LWS_README_IN_FENCE)
            set(LWS_README_IN_FENCE FALSE)
        else()
            set(LWS_README_IN_FENCE TRUE)
        endif()
        continue()
    endif()
    if(LWS_README_IN_FENCE)
        continue()
    endif()
    if(LWS_README_TABLE_FIRST GREATER 0 AND NOT _i LESS LWS_README_TABLE_FIRST
       AND NOT _i GREATER LWS_README_TABLE_LAST)
        continue()
    endif()
    if(NOT "${_line}" MATCHES "${LWS_README_UNIT_REGEX}")
        continue()
    endif()
    string(FIND "${_line}" "${LWS_README_MARKER}" _marked)
    if(NOT _marked EQUAL -1)
        continue()
    endif()
    set(_named FALSE)
    foreach(_id IN LISTS LWS_README_IDS)
        if("${_line}" MATCHES "(^|[^A-Za-z0-9_])${_id}([^A-Za-z0-9_]|$)")
            set(_named TRUE)
        endif()
    endforeach()
    if(NOT _named)
        lws_readme_refuse(${_i} "unmarked number" "${_line}")
    endif()
endforeach()

if(LWS_README_REFUSED GREATER 0)
    message(STATUS "README-REFUSED n=${LWS_README_REFUSED}")
    cmake_language(EXIT 1)
endif()
if(LWS_README_MISSING)
    foreach(_msg IN LISTS LWS_README_MISSING)
        message(STATUS "[readme] missing input: ${_msg}")
    endforeach()
    cmake_language(EXIT 2)
endif()
message(STATUS "README-OK rows=${LWS_README_ROWS}")
cmake_language(EXIT 0)
