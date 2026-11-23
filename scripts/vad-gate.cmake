# scripts/vad-gate.cmake: the VAD API text gate.
#
#   cmake -DLWS_ROOT={root} -P scripts/vad-gate.cmake
#
# Silero is the only voice-activity detector, and it runs through whisper's no-reset detection call so the
# model state carries across calls. The gate reads every .cpp and .hpp under src/ and every .hpp under
# include/ as text and refuses a line that holds one of three things:
#   reset-variant         whisper's detection call that resets the model state on every call
#   whisper-segmentation  whisper's own segmenter (it resets state and duplicates ours)
#   substitute-detector   the vocabulary of a signal-level stand-in for Silero (level, energy, fallback)
# Comment lines are scanned too, so the tree describes the failure contract in other words. Nothing is
# compiled. A refusal prints "[vad] <path>:<line>: refused <name>: <line>".
#
# Floor (non-vacuity): at least five files are scanned, and src/shell/vad_engine.cpp is among them and holds
# a line calling whisper_vad_detect_speech_no_reset(, otherwise the scan proves nothing.
#
# Exit: 0 holds; 1 refusal; 2 missing input or a failed floor. A refusal outranks missing input.
cmake_minimum_required(VERSION 3.29)

if(NOT DEFINED LWS_ROOT OR "${LWS_ROOT}" STREQUAL "")
    message(FATAL_ERROR "vad-gate: -DLWS_ROOT={root} is required")
endif()

# GLOB_RECURSE: a plain GLOB does not descend, so it would see nothing under src/shell/ or include/lws/.
file(GLOB_RECURSE LWS_VG_FILES
    "${LWS_ROOT}/src/*.cpp" "${LWS_ROOT}/src/*.hpp" "${LWS_ROOT}/include/*.hpp")

set(LWS_VG_REFUSED 0)
set(LWS_VG_MISSING "")
set(LWS_VG_SCANNED 0)
set(LWS_VG_ENGINE_FILE "${LWS_ROOT}/src/shell/vad_engine.cpp")
set(LWS_VG_ENGINE_CALL FALSE)

foreach(_vg_file IN LISTS LWS_VG_FILES)
    math(EXPR LWS_VG_SCANNED "${LWS_VG_SCANNED} + 1")
    file(RELATIVE_PATH _vg_rel "${LWS_ROOT}" "${_vg_file}")
    file(READ "${_vg_file}" _vg_text)
    # A semicolon or bracket in the text would split or merge list elements, so they are swapped for
    # markers first. Every line is wrapped in a leading ">" and a trailing "<": no line is an empty list
    # element (which would shift the line numbers), and both ends are non-word characters for the
    # patterns below.
    string(REPLACE ";" "@SEMI@" _vg_text "${_vg_text}")
    string(REPLACE "[" "@LB@" _vg_text "${_vg_text}")
    string(REPLACE "]" "@RB@" _vg_text "${_vg_text}")
    string(REPLACE "\n" "<\n>" _vg_text ">${_vg_text}")
    string(REPLACE "\n" ";" _vg_lines "${_vg_text}")

    set(_vg_number 0)
    foreach(_vg_line IN LISTS _vg_lines)
        math(EXPR _vg_number "${_vg_number} + 1")
        string(TOLOWER "${_vg_line}" _vg_lower)
        set(_vg_name "")
        if(_vg_line MATCHES "whisper_vad_detect_speech[^_A-Za-z0-9]")
            set(_vg_name "reset-variant")
        elseif(_vg_line MATCHES "whisper_vad_segments_from_(samples|probs)")
            set(_vg_name "whisper-segmentation")
        elseif(_vg_lower MATCHES "[^a-z0-9](rms|energy|fallback|fail_open)[^a-z0-9]")
            set(_vg_name "substitute-detector")
        endif()
        if(NOT _vg_name STREQUAL "")
            string(LENGTH "${_vg_line}" _vg_len)
            math(EXPR _vg_inner "${_vg_len} - 2")
            string(SUBSTRING "${_vg_line}" 1 ${_vg_inner} _vg_shown)
            string(REPLACE "@SEMI@" ";" _vg_shown "${_vg_shown}")
            string(REPLACE "@LB@" "[" _vg_shown "${_vg_shown}")
            string(REPLACE "@RB@" "]" _vg_shown "${_vg_shown}")
            # Printed here, not collected: a list would split a line at its semicolons.
            message(STATUS "[vad] ${_vg_rel}:${_vg_number}: refused ${_vg_name}: ${_vg_shown}")
            math(EXPR LWS_VG_REFUSED "${LWS_VG_REFUSED} + 1")
        endif()
        if("${_vg_file}" STREQUAL "${LWS_VG_ENGINE_FILE}")
            string(FIND "${_vg_line}" "whisper_vad_detect_speech_no_reset(" _vg_call)
            if(NOT _vg_call EQUAL -1)
                set(LWS_VG_ENGINE_CALL TRUE)
            endif()
        endif()
    endforeach()
endforeach()

if(LWS_VG_SCANNED LESS 5)
    list(APPEND LWS_VG_MISSING "only ${LWS_VG_SCANNED} files scanned, at least 5 expected (vacuous)")
endif()
if(NOT LWS_VG_ENGINE_FILE IN_LIST LWS_VG_FILES)
    list(APPEND LWS_VG_MISSING "src/shell/vad_engine.cpp is not among the scanned files")
elseif(NOT LWS_VG_ENGINE_CALL)
    list(APPEND LWS_VG_MISSING "src/shell/vad_engine.cpp holds no whisper_vad_detect_speech_no_reset( call (vacuous)")
endif()

if(LWS_VG_REFUSED GREATER 0)
    message(STATUS "VAD-REFUSED refusals=${LWS_VG_REFUSED}")
    cmake_language(EXIT 1)
endif()
if(LWS_VG_MISSING)
    foreach(_vg_msg IN LISTS LWS_VG_MISSING)
        message(STATUS "[vad] missing input: ${_vg_msg}")
    endforeach()
    cmake_language(EXIT 2)
endif()
message(STATUS "VAD-OK files=${LWS_VG_SCANNED}")
cmake_language(EXIT 0)
