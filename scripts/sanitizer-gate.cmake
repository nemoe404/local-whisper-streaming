# scripts/sanitizer-gate.cmake: the instrumented-leg gate (BLD-04, success criterion 3).
#
#   cmake -DLWS_ROOT={root} -DLWS_BUILD_ID={id} -DLWS_LEGS={leg;leg} -P scripts/sanitizer-gate.cmake
#
# Per leg, LWS_SANITIZER and LWS_SANITIZER_SCOPE come from build/{id}/{leg}/CMakeCache.txt:
#   - asan-ubsan or tsan: the scope must be `all`, and every compile-database entry (ours and third-party,
#     C and C++) must carry the leg's option (the first compile element of lws_sanitizer_flags, so the
#     expected token has the same owner as the build). Floor: at least one C and one C++ entry under
#     third_party/whisper.cpp/ggml/src/ were checked, otherwise the sweep did not read ggml.
#   - off: no entry may carry any -fsanitize option (an instrumented release leg is also wrong).
#
# Exit: 0 holds; 1 refusal; 2 missing input or a failed floor. A refusal outranks missing input.
cmake_minimum_required(VERSION 3.29)

if(NOT DEFINED LWS_ROOT OR "${LWS_ROOT}" STREQUAL "")
    message(FATAL_ERROR "sanitizer-gate: -DLWS_ROOT={root} is required")
endif()
if(NOT DEFINED LWS_BUILD_ID OR "${LWS_BUILD_ID}" STREQUAL "")
    message(FATAL_ERROR "sanitizer-gate: -DLWS_BUILD_ID={id} is required")
endif()
if(NOT DEFINED LWS_LEGS OR "${LWS_LEGS}" STREQUAL "")
    message(FATAL_ERROR "sanitizer-gate: -DLWS_LEGS={leg;leg} is required")
endif()
include(${LWS_ROOT}/cmake/SanitizerFlags.cmake)

set(LWS_SG_REFUSALS "")
set(LWS_SG_MISSING "")
set(LWS_SG_GGML_DIR "/third_party/whisper.cpp/ggml/src/")

# Cache values, read with file(STRINGS) so a missing entry is an empty string.
function(lws_sg_cache_value out_var cache name)
    file(STRINGS "${cache}" _line REGEX "^${name}:")
    string(REGEX REPLACE "^${name}:[A-Za-z]*=" "" _value "${_line}")
    set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

foreach(_sg_leg IN LISTS LWS_LEGS)
    set(_sg_bdir "${LWS_ROOT}/build/${LWS_BUILD_ID}/${_sg_leg}")
    set(_sg_cache "${_sg_bdir}/CMakeCache.txt")
    set(_sg_db "${_sg_bdir}/compile_commands.json")
    set(_sg_lp "[sanitizer][leg ${_sg_leg}]")

    if(NOT EXISTS "${_sg_cache}")
        list(APPEND LWS_SG_MISSING "leg ${_sg_leg}: no ${_sg_cache}")
        continue()
    endif()
    lws_sg_cache_value(_sg_mode "${_sg_cache}" LWS_SANITIZER)
    lws_sg_cache_value(_sg_scope "${_sg_cache}" LWS_SANITIZER_SCOPE)

    if(NOT "${_sg_mode}" STREQUAL "off" AND NOT "${_sg_mode}" STREQUAL "asan-ubsan"
       AND NOT "${_sg_mode}" STREQUAL "tsan")
        list(APPEND LWS_SG_REFUSALS "${_sg_lp} CMakeCache.txt has LWS_SANITIZER='${_sg_mode}', not off, asan-ubsan or tsan")
        continue()
    endif()
    set(_sg_expected "")
    if(NOT "${_sg_mode}" STREQUAL "off")
        lws_sanitizer_flags("${_sg_mode}" _sg_compile _sg_link)
        list(GET _sg_compile 0 _sg_expected)
        if(NOT "${_sg_scope}" STREQUAL "all")
            list(APPEND LWS_SG_REFUSALS
                "${_sg_lp} LWS_SANITIZER_SCOPE is '${_sg_scope}', not all: the whisper.cpp subtree is not instrumented")
        endif()
    endif()

    if(NOT EXISTS "${_sg_db}")
        list(APPEND LWS_SG_MISSING "leg ${_sg_leg}: no compile database at ${_sg_db}")
        continue()
    endif()
    file(READ "${_sg_db}" _sg_text)
    string(JSON _sg_n ERROR_VARIABLE _sg_jerr LENGTH "${_sg_text}")
    if(_sg_jerr)
        list(APPEND LWS_SG_REFUSALS "${_sg_lp} ${_sg_db} is not valid JSON: refused")
        continue()
    endif()
    if(_sg_n EQUAL 0)
        list(APPEND LWS_SG_MISSING "leg ${_sg_leg}: compile database holds no entries (vacuous)")
        continue()
    endif()

    set(_sg_entries 0)
    set(_sg_ggml_c 0)
    set(_sg_ggml_cxx 0)
    math(EXPR _sg_last "${_sg_n} - 1")
    foreach(_sg_e RANGE ${_sg_last})
        math(EXPR _sg_entries "${_sg_entries} + 1")
        string(JSON _sg_file ERROR_VARIABLE _sg_ferr GET "${_sg_text}" ${_sg_e} file)
        if(_sg_ferr)
            set(_sg_file "<entry ${_sg_e}>")
        endif()

        set(_sg_args "")
        string(JSON _sg_at ERROR_VARIABLE _sg_aterr TYPE "${_sg_text}" ${_sg_e} arguments)
        if(NOT _sg_aterr AND "${_sg_at}" STREQUAL "ARRAY")
            string(JSON _sg_an LENGTH "${_sg_text}" ${_sg_e} arguments)
            if(_sg_an GREATER 0)
                math(EXPR _sg_alast "${_sg_an} - 1")
                foreach(_sg_k RANGE ${_sg_alast})
                    string(JSON _sg_a GET "${_sg_text}" ${_sg_e} arguments ${_sg_k})
                    list(APPEND _sg_args "${_sg_a}")
                endforeach()
            endif()
        else()
            string(JSON _sg_cmd ERROR_VARIABLE _sg_cmderr GET "${_sg_text}" ${_sg_e} command)
            if(_sg_cmderr)
                set(_sg_cmd "")
            endif()
            separate_arguments(_sg_args UNIX_COMMAND "${_sg_cmd}")
        endif()

        if("${_sg_mode}" STREQUAL "off")
            foreach(_sg_a IN LISTS _sg_args)
                if(_sg_a MATCHES "^-fsanitize")
                    list(APPEND LWS_SG_REFUSALS
                        "${_sg_lp} ${_sg_file}: carries ${_sg_a} on a leg whose LWS_SANITIZER is off")
                endif()
            endforeach()
        else()
            if(NOT "${_sg_expected}" IN_LIST _sg_args)
                list(APPEND LWS_SG_REFUSALS
                    "${_sg_lp} ${_sg_file}: lacks ${_sg_expected} (uninstrumented entry on the ${_sg_mode} leg)")
            endif()
            string(FIND "${_sg_file}" "${LWS_SG_GGML_DIR}" _sg_in_ggml)
            if(NOT _sg_in_ggml EQUAL -1)
                if(_sg_file MATCHES "\\.c$")
                    math(EXPR _sg_ggml_c "${_sg_ggml_c} + 1")
                elseif(_sg_file MATCHES "\\.cpp$")
                    math(EXPR _sg_ggml_cxx "${_sg_ggml_cxx} + 1")
                endif()
            endif()
        endif()
    endforeach()

    if(NOT "${_sg_mode}" STREQUAL "off" AND (_sg_ggml_c EQUAL 0 OR _sg_ggml_cxx EQUAL 0))
        list(APPEND LWS_SG_MISSING
            "leg ${_sg_leg}: the sweep saw ggml_c=${_sg_ggml_c} ggml_cxx=${_sg_ggml_cxx} entries under ${LWS_SG_GGML_DIR}: it did not read ggml")
    endif()

    if("${_sg_mode}" STREQUAL "off")
        set(_sg_ok_line "SANITIZER-OK leg=${_sg_leg} mode=off entries=${_sg_entries}")
    else()
        set(_sg_ok_line
            "SANITIZER-OK leg=${_sg_leg} mode=${_sg_mode} entries=${_sg_entries} ggml_c=${_sg_ggml_c} ggml_cxx=${_sg_ggml_cxx}")
    endif()
    list(APPEND LWS_SG_OK_LINES "${_sg_ok_line}")
endforeach()

if(LWS_SG_REFUSALS)
    foreach(_sg_line IN LISTS LWS_SG_REFUSALS)
        message(STATUS "${_sg_line}")
    endforeach()
    message(STATUS "SANITIZER-REFUSED legs=${LWS_LEGS}")
    cmake_language(EXIT 1)
endif()
if(LWS_SG_MISSING)
    foreach(_sg_line IN LISTS LWS_SG_MISSING)
        message(STATUS "[sanitizer] missing input: ${_sg_line}")
    endforeach()
    cmake_language(EXIT 2)
endif()
foreach(_sg_line IN LISTS LWS_SG_OK_LINES)
    message(STATUS "${_sg_line}")
endforeach()
cmake_language(EXIT 0)
