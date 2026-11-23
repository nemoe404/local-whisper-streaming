find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# gtest prints "[  SKIPPED ]" for GTEST_SKIP and still exits 0, so without this property ctest
# reports a skipped test as passed. Every gtest_discover_tests call in this tree passes it.
set(LWS_GTEST_SKIP_REGEX "\\[  SKIPPED \\]")

# Globbed on purpose: a new *_test.cpp is picked up without editing CMake.
file(GLOB LWS_SMOKE_TESTS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/smoke/*_test.cpp)
foreach(_src IN LISTS LWS_SMOKE_TESTS)
    get_filename_component(_stem ${_src} NAME_WE)
    add_executable(smoke_${_stem} ${_src})
    target_link_libraries(smoke_${_stem} PRIVATE lws_smoke lws_warnings lws_sanitizers GTest::gtest_main)
    if(_stem STREQUAL "silero_smoke_test")
        # The test calls whisper_vad_* directly and loads the model scripts/fetch-models.sh writes (D-05).
        target_link_libraries(smoke_${_stem} PRIVATE whisper)
        target_compile_definitions(smoke_${_stem} PRIVATE
            LWS_SILERO_MODEL_PATH="${PROJECT_SOURCE_DIR}/models/ggml-silero-v5.1.2.bin")
        # SileroSmoke is the success-criterion-3 smoke test (label smoke, never filtered). SileroSmokeMt
        # runs ggml's thread pool and carries label ggml-mt, which the tsan scope rule filters. Label
        # real-model is reserved for multi-frame model-backed tests; no Phase 1 test carries it.
        gtest_discover_tests(smoke_${_stem}
            DISCOVERY_MODE PRE_TEST
            TEST_FILTER "SileroSmoke.*"
            TEST_LIST lws_silero_tests
            PROPERTIES LABELS smoke TIMEOUT 120
            SKIP_REGULAR_EXPRESSION ${LWS_GTEST_SKIP_REGEX})
        gtest_discover_tests(smoke_${_stem}
            DISCOVERY_MODE PRE_TEST
            TEST_FILTER "SileroSmokeMt.*"
            TEST_LIST lws_silero_mt_tests
            PROPERTIES LABELS ggml-mt TIMEOUT 120
            SKIP_REGULAR_EXPRESSION ${LWS_GTEST_SKIP_REGEX})
    else()
        gtest_discover_tests(smoke_${_stem}
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS smoke TIMEOUT 120
            SKIP_REGULAR_EXPRESSION ${LWS_GTEST_SKIP_REGEX})
    endif()
endforeach()

# Pure-core tests: one executable per file, linked against lws_core only (no whisper, no model).
file(GLOB LWS_CORE_TESTS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/core/*_test.cpp)
foreach(_src IN LISTS LWS_CORE_TESTS)
    get_filename_component(_stem ${_src} NAME_WE)
    add_executable(core_${_stem} ${_src})
    target_link_libraries(core_${_stem} PRIVATE lws_core lws_warnings lws_sanitizers GTest::gtest_main)
    gtest_discover_tests(core_${_stem}
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS core TIMEOUT 120
        SKIP_REGULAR_EXPRESSION ${LWS_GTEST_SKIP_REGEX})
endforeach()

# Shell tests: one executable per file, linked against lws_core. One executable per file is required
# because a later test file replaces the global allocation functions, which must not touch the others.
# No Threads::Threads link: std::jthread links without it (tests/gates/seeds/tsan_race.cpp).
file(GLOB LWS_SHELL_TESTS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/shell/*_test.cpp)
foreach(_src IN LISTS LWS_SHELL_TESTS)
    get_filename_component(_stem ${_src} NAME_WE)
    add_executable(shell_${_stem} ${_src})
    target_link_libraries(shell_${_stem} PRIVATE lws_core lws_warnings lws_sanitizers GTest::gtest_main)
    gtest_discover_tests(shell_${_stem}
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS shell TIMEOUT 120
        SKIP_REGULAR_EXPRESSION ${LWS_GTEST_SKIP_REGEX})
endforeach()

# Link closure of lws_core (SC5, T-02-10). A breadth-first walk over LINK_LIBRARIES and
# INTERFACE_LINK_LIBRARIES writes every name it meets, sorted, one per line, to the build tree;
# scripts/core-isolation-gate.cmake reads it in the core_link_closure test and refuses a whisper, ggml,
# spdlog or thread name. The walk is deferred to the end of this directory's configure step, so a
# target_link_libraries line appended below (the seeded red proof) is part of what it reads.
# If fmt::fmt's own closure ever holds a thread library the check reddens. Do not weaken the gate,
# report it (research Assumption A7).
function(lws_write_core_closure)
    set(_queue lws_core)
    set(_seen "")
    while(_queue)
        list(POP_FRONT _queue _current)
        if(_current IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_current}")
        if(NOT TARGET ${_current})
            continue()
        endif()
        foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(_deps ${_current} ${_prop})
            if(NOT _deps OR _deps MATCHES "NOTFOUND$")
                continue()
            endif()
            # Drop $<LINK_ONLY:...> and any other generator expression wrapper, keeping what it wraps.
            string(REGEX REPLACE "\\$<[^:>]*:" "" _deps "${_deps}")
            string(REPLACE ">" "" _deps "${_deps}")
            list(APPEND _queue ${_deps})
        endforeach()
    endwhile()
    list(REMOVE_ITEM _seen lws_core "")
    list(SORT _seen)
    list(JOIN _seen "\n" _text)
    file(WRITE "${CMAKE_BINARY_DIR}/lws_core_closure.txt" "${_text}\n")
endfunction()
cmake_language(DEFER CALL lws_write_core_closure)

add_test(NAME core_link_closure
    COMMAND ${CMAKE_COMMAND} -DLWS_ROOT=${PROJECT_SOURCE_DIR}
            -DLWS_CLOSURE_FILE=${CMAKE_BINARY_DIR}/lws_core_closure.txt
            -P ${PROJECT_SOURCE_DIR}/scripts/core-isolation-gate.cmake)
set_tests_properties(core_link_closure PROPERTIES LABELS gate TIMEOUT 120)

# Gate verdict rows, run in script mode through the same cmake binary that drives the build. The
# driver includes cmake/Clang21Toolchain.cmake, so the row verdicts and configure share one owner.
add_test(NAME toolchain_verdict_cases
    COMMAND ${CMAKE_COMMAND} -DLWS_ROOT=${PROJECT_SOURCE_DIR}
            -P ${PROJECT_SOURCE_DIR}/tests/gates/toolchain_verdict_cases.cmake)
set_tests_properties(toolchain_verdict_cases PROPERTIES LABELS gate TIMEOUT 120)

add_test(NAME isa_verdict_cases
    COMMAND ${CMAKE_COMMAND} -DLWS_ROOT=${PROJECT_SOURCE_DIR}
            -P ${PROJECT_SOURCE_DIR}/tests/gates/isa_verdict_cases.cmake)
set_tests_properties(isa_verdict_cases PROPERTIES LABELS gate TIMEOUT 120)
