# scripts/isa-gate.cmake: the native-ISA sweep over every leg's compile database (BLD-01, success
# criterion 2).
#
#   cmake -DLWS_ROOT={root} -DLWS_BUILD_ID={id} -DLWS_LEGS={leg;leg} -P scripts/isa-gate.cmake
#
# Per leg:
#   - build/{id}/{leg}/CMakeCache.txt must hold GGML_NATIVE:BOOL=OFF, and LWS_CPU_BASELINE must be a
#     name lws_select_cpu_baseline accepts;
#   - every argument of every compile-database entry goes through lws_isa_flag_refusal (the rule
#     configure uses), so a -march=, -mtune=, -mcpu= or above-baseline flag anywhere is a refusal;
#   - non-vacuity floor: at least one ggml-cpu entry carries the whole baseline, otherwise the sweep
#     did not read ggml-cpu's real flags.
#
# Exit: 0 holds; 1 refusal; 2 missing input or a failed floor. A refusal outranks missing input.
cmake_minimum_required(VERSION 3.29)

if(NOT DEFINED LWS_ROOT OR "${LWS_ROOT}" STREQUAL "")
    message(FATAL_ERROR "isa-gate: -DLWS_ROOT={root} is required")
endif()
if(NOT DEFINED LWS_BUILD_ID OR "${LWS_BUILD_ID}" STREQUAL "")
    message(FATAL_ERROR "isa-gate: -DLWS_BUILD_ID={id} is required")
endif()
if(NOT DEFINED LWS_LEGS OR "${LWS_LEGS}" STREQUAL "")
    message(FATAL_ERROR "isa-gate: -DLWS_LEGS={leg;leg} is required")
endif()
include(${LWS_ROOT}/cmake/CpuBaseline.cmake)

set(LWS_IG_REFUSALS "")
set(LWS_IG_MISSING "")
set(LWS_IG_LEGS 0)
set(LWS_IG_ENTRIES 0)
set(LWS_IG_GGML 0)

# The ggml-cpu floor: a ggml-cpu entry must carry every one of these (the baseline, as ggml emits it).
set(LWS_IG_FLOOR -msse4.2 -mavx -mavx2 -mfma -mf16c -mbmi2)
set(LWS_IG_GGML_DIR "third_party/whisper.cpp/ggml/src/ggml-cpu/")

foreach(_ig_leg IN LISTS LWS_LEGS)
    math(EXPR LWS_IG_LEGS "${LWS_IG_LEGS} + 1")
    set(_ig_bdir "${LWS_ROOT}/build/${LWS_BUILD_ID}/${_ig_leg}")
    set(_ig_cache "${_ig_bdir}/CMakeCache.txt")
    set(_ig_db "${_ig_bdir}/compile_commands.json")
    set(_ig_lp "[isa][leg ${_ig_leg}]")

    if(NOT EXISTS "${_ig_cache}")
        list(APPEND LWS_IG_MISSING "leg ${_ig_leg}: no ${_ig_cache}")
        continue()
    endif()

    file(STRINGS "${_ig_cache}" _ig_native REGEX "^GGML_NATIVE:")
    if(NOT "${_ig_native}" STREQUAL "GGML_NATIVE:BOOL=OFF")
        list(APPEND LWS_IG_REFUSALS
            "${_ig_lp} CMakeCache.txt has '${_ig_native}', not GGML_NATIVE:BOOL=OFF")
    endif()

    file(STRINGS "${_ig_cache}" _ig_base REGEX "^LWS_CPU_BASELINE:")
    string(REGEX REPLACE "^LWS_CPU_BASELINE:[A-Za-z]*=" "" _ig_base_name "${_ig_base}")
    lws_select_cpu_baseline(_ig_isas _ig_base_reason "${_ig_base_name}")
    if(NOT "${_ig_base_reason}" STREQUAL "")
        list(APPEND LWS_IG_REFUSALS "${_ig_lp} ${_ig_base_reason}")
    endif()

    if(NOT EXISTS "${_ig_db}")
        list(APPEND LWS_IG_MISSING "leg ${_ig_leg}: no compile database at ${_ig_db}")
        continue()
    endif()
    file(READ "${_ig_db}" _ig_text)
    string(JSON _ig_n ERROR_VARIABLE _ig_jerr LENGTH "${_ig_text}")
    if(_ig_jerr)
        list(APPEND LWS_IG_REFUSALS "${_ig_lp} ${_ig_db} is not valid JSON: refused")
        continue()
    endif()
    if(_ig_n EQUAL 0)
        list(APPEND LWS_IG_MISSING "leg ${_ig_leg}: compile database holds no entries (vacuous)")
        continue()
    endif()

    set(_ig_leg_ggml 0)
    math(EXPR _ig_last "${_ig_n} - 1")
    foreach(_ig_e RANGE ${_ig_last})
        math(EXPR LWS_IG_ENTRIES "${LWS_IG_ENTRIES} + 1")
        string(JSON _ig_file ERROR_VARIABLE _ig_ferr GET "${_ig_text}" ${_ig_e} file)
        if(_ig_ferr)
            set(_ig_file "<entry ${_ig_e}>")
        endif()

        set(_ig_args "")
        string(JSON _ig_at ERROR_VARIABLE _ig_aterr TYPE "${_ig_text}" ${_ig_e} arguments)
        if(NOT _ig_aterr AND "${_ig_at}" STREQUAL "ARRAY")
            string(JSON _ig_an LENGTH "${_ig_text}" ${_ig_e} arguments)
            if(_ig_an GREATER 0)
                math(EXPR _ig_alast "${_ig_an} - 1")
                foreach(_ig_k RANGE ${_ig_alast})
                    string(JSON _ig_a GET "${_ig_text}" ${_ig_e} arguments ${_ig_k})
                    list(APPEND _ig_args "${_ig_a}")
                endforeach()
            endif()
        else()
            string(JSON _ig_cmd ERROR_VARIABLE _ig_cmderr GET "${_ig_text}" ${_ig_e} command)
            if(_ig_cmderr)
                set(_ig_cmd "")
            endif()
            separate_arguments(_ig_args UNIX_COMMAND "${_ig_cmd}")
        endif()

        foreach(_ig_a IN LISTS _ig_args)
            lws_isa_flag_refusal(_ig_reason "${_ig_a}")
            if(NOT "${_ig_reason}" STREQUAL "")
                list(APPEND LWS_IG_REFUSALS "${_ig_lp} ${_ig_file}: ${_ig_reason}")
            endif()
        endforeach()

        string(FIND "${_ig_file}" "${LWS_IG_GGML_DIR}" _ig_in_ggml)
        if(NOT _ig_in_ggml EQUAL -1)
            set(_ig_all TRUE)
            foreach(_ig_f IN LISTS LWS_IG_FLOOR)
                if(NOT _ig_f IN_LIST _ig_args)
                    set(_ig_all FALSE)
                endif()
            endforeach()
            if(_ig_all)
                math(EXPR _ig_leg_ggml "${_ig_leg_ggml} + 1")
            endif()
        endif()
    endforeach()

    if(_ig_leg_ggml EQUAL 0)
        list(APPEND LWS_IG_MISSING
            "leg ${_ig_leg}: no ggml-cpu entry carries the baseline (${LWS_IG_FLOOR}): the sweep did not read ggml-cpu's flags")
    endif()
    math(EXPR LWS_IG_GGML "${LWS_IG_GGML} + ${_ig_leg_ggml}")
endforeach()

if(LWS_IG_REFUSALS)
    foreach(_ig_line IN LISTS LWS_IG_REFUSALS)
        message(STATUS "${_ig_line}")
    endforeach()
    message(STATUS "ISA-REFUSED legs=${LWS_IG_LEGS} entries=${LWS_IG_ENTRIES} ggml_cpu_entries=${LWS_IG_GGML}")
    cmake_language(EXIT 1)
endif()
if(LWS_IG_MISSING)
    foreach(_ig_line IN LISTS LWS_IG_MISSING)
        message(STATUS "[isa] missing input: ${_ig_line}")
    endforeach()
    cmake_language(EXIT 2)
endif()
message(STATUS "ISA-OK legs=${LWS_IG_LEGS} entries=${LWS_IG_ENTRIES} ggml_cpu_entries=${LWS_IG_GGML}")
cmake_language(EXIT 0)
