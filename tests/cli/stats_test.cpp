#include "cli/stats.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
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

// The summary of a run that must have one: a missing summary fails the test and gives zeros.
lws_cli::LagSummary summary_of(std::span<const PushMark> marks,
                               std::span<const Delivery> deliveries) {
    const auto summary = lws_cli::lag_summary(marks, deliveries);
    EXPECT_TRUE(summary.has_value());
    return summary.value_or(lws_cli::LagSummary{});
}

void expect_summary(const lws_cli::LagSummary& summary, std::size_t count, double median,
                    double p95, double max) {
    EXPECT_EQ(summary.count, count);
    EXPECT_DOUBLE_EQ(summary.median_ms, median);
    EXPECT_DOUBLE_EQ(summary.p95_ms, p95);
    EXPECT_DOUBLE_EQ(summary.max_ms, max);
}

} // namespace

TEST(CliStats, LagUsesThePushThatHeldTheLastSample) {
    const std::vector<PushMark> marks = three_marks();
    const std::array<Delivery, 1> on_the_edge{Delivery{.t_end = 8192, .at = at_ms(900)}};
    const std::array<Delivery, 1> one_past{Delivery{.t_end = 8193, .at = at_ms(900)}};

    EXPECT_DOUBLE_EQ(summary_of(marks, on_the_edge).max_ms, 800.0);
    EXPECT_DOUBLE_EQ(summary_of(marks, one_past).max_ms, 700.0);
}

TEST(CliStats, NearestRankPercentiles) {
    const std::array<PushMark, 1> marks{PushMark{.samples_after = 100, .at = at_ms(0)}};
    std::vector<Delivery> lags =
        std::views::iota(1, 21) |
        std::views::transform([](int lag) { return Delivery{.t_end = 100, .at = at_ms(lag)}; }) |
        std::ranges::to<std::vector>();
    // Out of order on purpose: the summary sorts.
    std::ranges::reverse(lags);
    const std::array<Delivery, 1> single{Delivery{.t_end = 100, .at = at_ms(5)}};

    const lws_cli::LagSummary many = summary_of(marks, lags);
    const lws_cli::LagSummary one = summary_of(marks, single);

    expect_summary(many, 20, 10.0, 19.0, 20.0);
    expect_summary(one, 1, 5.0, 5.0, 5.0);
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

    EXPECT_DOUBLE_EQ(summary_of(marks, past).max_ms, 250.0);
    EXPECT_FALSE(lws_cli::lag_summary({}, past).has_value());
}
