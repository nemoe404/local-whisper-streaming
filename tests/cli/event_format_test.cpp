#include "cli/event_format.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace {

TEST(CliEventFormat, StreamTimesAreSamplesOverSixteen) {
    constexpr std::array<std::pair<std::uint64_t, const char*>, 7> kCases{{
        {0, "00:00:00.000"},
        {15, "00:00:00.000"},
        {16, "00:00:00.001"},
        {15999, "00:00:00.999"},
        {16000, "00:00:01.000"},
        {57600000, "01:00:00.000"},
        {5760000000, "100:00:00.000"},
    }};
    std::ranges::for_each(kCases, [](const auto& entry) {
        EXPECT_EQ(lws_cli::format_stream_time(entry.first), entry.second)
            << "samples " << entry.first;
    });
}

TEST(CliEventFormat, TextGoesToStdoutAndLossesDoNot) {
    const lws::TextEvent text{
        .kind = lws::EventKind::Text, .t_start = 16000, .t_end = 48000, .text = "  \t hello world"};
    EXPECT_EQ(lws_cli::format_event(text), "[00:00:01.000 --> 00:00:03.000]  hello world\n");
    EXPECT_TRUE(lws_cli::goes_to_stdout(lws::EventKind::Text));

    const std::string secret = "must-not-appear";
    const auto loss = [&](lws::EventKind kind, std::uint64_t start, std::uint64_t end) {
        return lws::TextEvent{.kind = kind, .t_start = start, .t_end = end, .text = secret};
    };

    const auto overrun = lws_cli::format_event(loss(lws::EventKind::Overrun, 16000, 18048));
    EXPECT_EQ(overrun, "[00:00:01.000 --> 00:00:01.128]  overrun: 2048 samples dropped\n");
    EXPECT_EQ(lws_cli::format_event(loss(lws::EventKind::VadFailure, 0, 512)),
              "[00:00:00.000 --> 00:00:00.032]  vad failure\n");
    EXPECT_EQ(lws_cli::format_event(loss(lws::EventKind::SegmentDropped, 32000, 64000)),
              "[00:00:02.000 --> 00:00:04.000]  segment dropped\n");
    EXPECT_EQ(lws_cli::format_event(loss(lws::EventKind::DecodeFailure, 160, 320)),
              "[00:00:00.010 --> 00:00:00.020]  decode failure\n");

    constexpr std::array<lws::EventKind, 4> kLosses{
        lws::EventKind::Overrun, lws::EventKind::VadFailure, lws::EventKind::SegmentDropped,
        lws::EventKind::DecodeFailure};
    std::ranges::for_each(kLosses, [&](lws::EventKind kind) {
        EXPECT_FALSE(lws_cli::goes_to_stdout(kind));
        EXPECT_EQ(lws_cli::format_event(loss(kind, 0, 100)).find(secret), std::string::npos);
    });
}

} // namespace
