// Seed for the clang-tidy proof (tests/gates/static_gates_red.sh). Valid C++ under lws_warnings, so
// the compiler accepts it; clang-tidy reports exactly one defect, readability-container-size-empty:
// the emptiness of a container is asked through size() == 0 instead of empty(). The file has no
// _test suffix, so tests/CMakeLists.txt never compiles it in place; the proof copies it to
// tests/smoke/zz_seed_tidy_test.cpp for its red run.
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

TEST(SeedTidy, SizeComparedToZero) {
    const std::vector<std::int32_t> values;
    EXPECT_TRUE(values.size() == 0);
}
