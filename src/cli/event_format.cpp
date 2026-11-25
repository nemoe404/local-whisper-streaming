#include "cli/event_format.hpp"

#include "lws/options.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace lws_cli {

namespace {

constexpr std::uint64_t kSamplesPerMs = lws::kSampleRateHz / 1000U;
constexpr std::uint64_t kMsPerSecond = 1000;
constexpr std::uint64_t kMsPerMinute = 60 * kMsPerSecond;
constexpr std::uint64_t kMsPerHour = 60 * kMsPerMinute;

std::string_view without_leading_space(std::string_view text) {
    constexpr std::string_view kSpace = " \t\n\r\f\v";
    return text.substr(std::min(text.find_first_not_of(kSpace), text.size()));
}

} // namespace

std::string format_stream_time(std::uint64_t samples) {
    const std::uint64_t ms = samples / kSamplesPerMs;
    return fmt::format("{:02}:{:02}:{:02}.{:03}", ms / kMsPerHour, ms / kMsPerMinute % 60,
                       ms / kMsPerSecond % 60, ms % kMsPerSecond);
}

std::string format_event(const lws::TextEvent& event) {
    const std::string range = fmt::format("[{} --> {}]", format_stream_time(event.t_start),
                                          format_stream_time(event.t_end));
    switch (event.kind) {
    case lws::EventKind::Text:
        return fmt::format("{}  {}\n", range, without_leading_space(event.text));
    case lws::EventKind::Overrun:
        return fmt::format("{}  overrun: {} samples dropped\n", range, event.t_end - event.t_start);
    case lws::EventKind::VadFailure:
        return fmt::format("{}  vad failure\n", range);
    case lws::EventKind::SegmentDropped:
        return fmt::format("{}  segment dropped\n", range);
    case lws::EventKind::DecodeFailure:
        return fmt::format("{}  decode failure\n", range);
    }
    std::unreachable();
}

bool goes_to_stdout(lws::EventKind kind) noexcept { return kind == lws::EventKind::Text; }

} // namespace lws_cli
