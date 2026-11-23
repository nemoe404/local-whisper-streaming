// Seed for the tsan leg (tests/gates/leg_matrix.sh). Two threads increment one plain integer with
// no synchronisation: a data race that only ThreadSanitizer observes. The other legs stay green
// because the test never asserts the counter value, and neither ASan, UBSan nor an uninstrumented
// build checks for races. With halt_on_error=1 TSan fails the test.
#include <algorithm>
#include <cstdint>
#include <functional>
#include <ranges>
#include <thread>

#include <gtest/gtest.h>

namespace {

constexpr std::int32_t kIncrements = 100000;

} // namespace

TEST(SeedTsan, UnsynchronisedIncrement) {
    std::int32_t counter = 0;
    bool first_ran = false;
    bool second_ran = false;
    {
        const auto body = [&counter](bool& ran) {
            std::ranges::for_each(std::views::iota(std::int32_t{0}, kIncrements),
                                  [&counter](std::int32_t) { ++counter; });
            ran = true;
        };
        const std::jthread first{body, std::ref(first_ran)};
        const std::jthread second{body, std::ref(second_ran)};
    }
    EXPECT_TRUE(first_ran);
    EXPECT_TRUE(second_ran);
}
