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
    target_link_libraries(smoke_${_stem} PRIVATE lws_smoke lws_warnings GTest::gtest_main)
    gtest_discover_tests(smoke_${_stem}
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS smoke TIMEOUT 120
        SKIP_REGULAR_EXPRESSION ${LWS_GTEST_SKIP_REGEX})
endforeach()
