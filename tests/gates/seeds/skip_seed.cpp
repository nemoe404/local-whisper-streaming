// The skip seed (tests/gates/skip_red.sh). Valid C++ under lws_warnings; its only effect is one
// skipped test. GTEST_SKIP exits 0, so only SKIP_REGULAR_EXPRESSION and the skip count in
// scripts/check.sh turn it into a failure.
#include <gtest/gtest.h>

TEST(SeedSkip, AlwaysSkips) { GTEST_SKIP() << "seeded skip (tests/gates/skip_red.sh)"; }
