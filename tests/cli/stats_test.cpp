#include "cli/stats.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace {

using lws_cli::Clock;
using lws_cli::Delivery;
using lws_cli::PushMark;
using std::chrono::milliseconds;

Clock::time_point at_ms(std::int64_t ms) { return Clock::time_point{} + milliseconds{ms}; }

constexpr std::array<std::uint64_t, 3> kMarkSamples{4096, 8192, 12288};

// Marks 100 ms apart, the first at 0 ms.
std::vector<PushMark> three_marks() {
    return std::views::iota(0, 3) | std::views::transform([](int index) {
               return PushMark{.samples_after = kMarkSamples.at(static_cast<std::size_t>(index)),
                               .at = at_ms(std::int64_t{index} * 100)};
           }) |
           std::ranges::to<std::vector>();
}

} // namespace

TEST(CliStats, LagUsesThePushThatHeldTheLastSample) {
    const std::vector<PushMark> marks = three_marks();
    const std::array<Delivery, 1> on_the_edge{Delivery{.t_end = 8192, .at = at_ms(900)}};
    const std::array<Delivery, 1> one_past{Delivery{.t_end = 8193, .at = at_ms(900)}};

    const auto first = lws_cli::lag_summary(marks, on_the_edge);
    const auto second = lws_cli::lag_summary(marks, one_past);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_DOUBLE_EQ(first->max_ms, 800.0);
    EXPECT_DOUBLE_EQ(second->max_ms, 700.0);
}

TEST(CliStats, NearestRankPercentiles) {
    const std::array<PushMark, 1> marks{PushMark{.samples_after = 100, .at = at_ms(0)}};
    const std::vector<Delivery> twenty =
        std::views::iota(1, 21) |
        std::views::transform([](int lag) { return Delivery{.t_end = 100, .at = at_ms(lag)}; }) |
        std::ranges::to<std::vector>();
    // Out of order on purpose: the summary sorts.
    std::vector<Delivery> shuffled = twenty;
    std::ranges::reverse(shuffled);
    const std::array<Delivery, 1> single{Delivery{.t_end = 100, .at = at_ms(5)}};

    const auto many = lws_cli::lag_summary(marks, shuffled);
    const auto one = lws_cli::lag_summary(marks, single);

    ASSERT_TRUE(many.has_value());
    EXPECT_EQ(many->count, 20U);
    EXPECT_DOUBLE_EQ(many->median_ms, 10.0);
    EXPECT_DOUBLE_EQ(many->p95_ms, 19.0);
    EXPECT_DOUBLE_EQ(many->max_ms, 20.0);
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->count, 1U);
    EXPECT_DOUBLE_EQ(one->median_ms, 5.0);
    EXPECT_DOUBLE_EQ(one->p95_ms, 5.0);
    EXPECT_DOUBLE_EQ(one->max_ms, 5.0);
}

TEST(CliStats, NoDeliveriesGiveNoLagSummary) {
    const std::vector<PushMark> marks = three_marks();
    EXPECT_FALSE(lws_cli::lag_summary(marks, {}).has_value());

    const lws_cli::StatsLine empty{.audio_s = 10.77,
                                   .wall_s = 3.5,
                                   .speed_x = 3.08,
                                   .load_ms = 250.0,
                                   .text_events = 0,
                                   .lag = std::nullopt,
                                   .peak_rss_mib = 321.5,
                                   .cpu_user_s = 4.25,
                                   .cpu_sys_s = 0.5};
    EXPECT_EQ(lws_cli::format_stats(empty),
              "stats: audio_s=10.77 wall_s=3.50 speed_x=3.08 load_ms=250.0 text_events=0 "
              "lag_ms_median=- lag_ms_p95=- lag_ms_max=- peak_rss_mib=321.5 cpu_user_s=4.25 "
              "cpu_sys_s=0.50");

    lws_cli::StatsLine with_lag = empty;
    with_lag.text_events = 2;
    with_lag.lag =
        lws_cli::LagSummary{.count = 2, .median_ms = 810.3, .p95_ms = 905.5, .max_ms = 905.5};
    EXPECT_EQ(lws_cli::format_stats(with_lag),
              "stats: audio_s=10.77 wall_s=3.50 speed_x=3.08 load_ms=250.0 text_events=2 "
              "lag_ms_median=810.3 lag_ms_p95=905.5 lag_ms_max=905.5 peak_rss_mib=321.5 "
              "cpu_user_s=4.25 cpu_sys_s=0.50");
}

TEST(CliStats, ADeliveryPastEveryMarkUsesTheLastMark) {
    const std::vector<PushMark> marks = three_marks();
    const std::array<Delivery, 1> past{Delivery{.t_end = 1'000'000, .at = at_ms(450)}};

    const auto summary = lws_cli::lag_summary(marks, past);

    ASSERT_TRUE(summary.has_value());
    EXPECT_DOUBLE_EQ(summary->max_ms, 250.0);
    EXPECT_FALSE(lws_cli::lag_summary({}, past).has_value());
}
