// Seed for the cppcheck proof (tests/gates/static_gates_red.sh). Valid C++ under lws_warnings, so
// the compiler accepts it; cppcheck reports exactly one finding, redundantAssignment: `value` is
// assigned 1 and overwritten before anything reads it. The file has no _test suffix, so
// tests/CMakeLists.txt never compiles it in place; the proof copies it to
// tests/smoke/zz_seed_cppcheck_test.cpp for its red run.
#include <cstdint>

#include <gtest/gtest.h>

TEST(SeedCppcheck, RedundantAssignment) {
    std::int32_t value = 0;
    value = 1;
    value = 2;
    EXPECT_EQ(value, 2);
}
