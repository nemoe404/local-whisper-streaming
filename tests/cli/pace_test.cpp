#include "cli/pace.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace {

using std::chrono::milliseconds;
using std::chrono::nanoseconds;
using std::chrono::seconds;

TEST(CliPace, UnpacedIsAlwaysDue) {
    constexpr std::array<std::uint64_t, 3> kSamples{0, 16000, 576000000};
    std::ranges::for_each(kSamples, [](std::uint64_t samples) {
        EXPECT_EQ(lws_cli::pace_due(samples, 0.0), nanoseconds{0}) << "samples " << samples;
    });
}

TEST(CliPace, RealTimeDueTimes) {
    EXPECT_EQ(lws_cli::pace_due(16000, 1.0), seconds{1});
    EXPECT_EQ(lws_cli::pace_due(8000, 1.0), milliseconds{500});
    EXPECT_EQ(lws_cli::pace_due(16000, 8.0), milliseconds{125});
}

TEST(CliPace, TenHoursStayExact) {
    constexpr std::uint64_t kTenHours = 576000000;
    EXPECT_EQ(lws_cli::pace_due(kTenHours, 1.0), seconds{36000});
    EXPECT_EQ(lws_cli::pace_due(kTenHours, 2.0), seconds{18000});
}

} // namespace
