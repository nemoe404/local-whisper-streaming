// Seed for the release leg (tests/gates/leg_matrix.sh). A local const used only inside assert():
// under NDEBUG the assert vanishes, the variable is unused, and -Wunused-variable with -Werror
// (lws_warnings) breaks the build. The Debug-type legs keep the assert, so they compile. This is
// the class of break the release leg exists for.
#include <cassert>
#include <cstdint>

#include <gtest/gtest.h>

TEST(SeedRelease, ValueUsedOnlyInAssert) {
    const std::int32_t expected = 4;
    assert(expected == 4);
}
