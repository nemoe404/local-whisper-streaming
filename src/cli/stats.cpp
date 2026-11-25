#include "cli/stats.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <ranges>
#include <vector>

namespace lws_cli {

namespace {

constexpr std::size_t kMedianPercent = 50;
constexpr std::size_t kTailPercent = 95;
constexpr std::size_t kPercentScale = 100;

// The element at rank ceil(percent * n / 100) of the sorted values, counting from 1. Whole-number
// arithmetic, so no rounding error can move the rank.
double nearest_rank(const std::vector<double>& sorted, std::size_t percent) {
    assert(!sorted.empty() && std::ranges::is_sorted(sorted));
    const std::size_t rank = ((percent * sorted.size()) + kPercentScale - 1) / kPercentScale;
    return sorted.at(rank - 1);
}

// Milliseconds from the push that first held the delivery's last sample to the delivery.
double lag_ms(std::span<const PushMark> marks, const Delivery& delivery) {
    assert(!marks.empty());
    auto held = std::ranges::lower_bound(marks, delivery.t_end, {}, &PushMark::samples_after);
    if (held == marks.end()) {
        held = std::prev(marks.end());
    }
    return std::chrono::duration<double, std::milli>(delivery.at - held->at).count();
}

} // namespace

std::optional<LagSummary> lag_summary(std::span<const PushMark> marks,
                                      std::span<const Delivery> deliveries) {
    if (marks.empty() || deliveries.empty()) {
        return std::nullopt;
    }
    std::vector<double> lags = deliveries |
                               std::views::transform([marks](const Delivery& delivery) {
                                   return lag_ms(marks, delivery);
                               }) |
                               std::ranges::to<std::vector>();
    std::ranges::sort(lags);
    return LagSummary{.count = lags.size(),
                      .median_ms = nearest_rank(lags, kMedianPercent),
                      .p95_ms = nearest_rank(lags, kTailPercent),
                      .max_ms = lags.back()};
}

std::string format_stats(const StatsLine& line) {
    const std::string lag =
        line.lag ? fmt::format("lag_ms_median={:.1f} lag_ms_p95={:.1f} lag_ms_max={:.1f}",
                               line.lag->median_ms, line.lag->p95_ms, line.lag->max_ms)
                 : std::string("lag_ms_median=- lag_ms_p95=- lag_ms_max=-");
    return fmt::format("stats: audio_s={:.2f} wall_s={:.2f} speed_x={:.2f} load_ms={:.1f} "
                       "text_events={} {} peak_rss_mib={:.1f} cpu_user_s={:.2f} cpu_sys_s={:.2f}",
                       line.audio_s, line.wall_s, line.speed_x, line.load_ms, line.text_events, lag,
                       line.peak_rss_mib, line.cpu_user_s, line.cpu_sys_s);
}

} // namespace lws_cli
