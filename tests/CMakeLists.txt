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
