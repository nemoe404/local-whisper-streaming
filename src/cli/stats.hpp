#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

// The numbers behind the tool's one-line report: how long after the audio of a segment was pushed
// its text came out, and the line that prints them. Pure values: no clock is read and nothing is
// written here.
namespace lws_cli {

using Clock = std::chrono::steady_clock;

// The stream had been offered `samples_after` samples once a push returned at time `at`.
struct PushMark {
    std::uint64_t samples_after = 0;
    Clock::time_point at;
};

// A text event whose range ends at `t_end` (one past its last sample) reached the printer at `at`.
struct Delivery {
    std::uint64_t t_end = 0;
    Clock::time_point at;
};

struct LagSummary {
    std::size_t count = 0;
    double median_ms = 0.0;
    double p95_ms = 0.0;
    double max_ms = 0.0;
};

// The lag of a delivery is its time minus the time of the first push that held the delivery's last
// sample (`t_end - 1`); a range past every mark uses the last mark. Percentiles are nearest-rank.
// Empty when there is no delivery or no push mark.
[[nodiscard]] std::optional<LagSummary> lag_summary(std::span<const PushMark> marks,
                                                    std::span<const Delivery> deliveries);

struct StatsLine {
    double audio_s = 0.0;
    double wall_s = 0.0;
    double speed_x = 0.0;
    double load_ms = 0.0;
    std::uint64_t text_events = 0;
    std::optional<LagSummary> lag;
    double peak_rss_mib = 0.0;
    double cpu_user_s = 0.0;
    double cpu_sys_s = 0.0;
};

// `stats: key=value ...` with every key, in a fixed order, and no newline. The three lag keys print
// `-` when there is no lag summary.
[[nodiscard]] std::string format_stats(const StatsLine& line);

} // namespace lws_cli
