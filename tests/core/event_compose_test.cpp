// A decoded segment becomes one text event placed in the offered stream, and every queued notice
// becomes an event of its own kind and range. These tests pin that composition as plain values.
#include "core/event_compose.hpp"

#include "lws/decode_policy.hpp"
#include "lws/segment.hpp"
#include "lws/text_event.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

lws::Segment segment_starting_at(std::uint64_t start, bool discontinuous_before = false) {
    lws::Segment segment;
    segment.samples.assign(2048, 0.0F);
    segment.start = start;
    segment.discontinuous_before = discontinuous_before;
    return segment;
}

lws::TextSpan span(std::uint64_t begin, std::uint64_t end, std::string text) {
    return lws::TextSpan{.begin = begin, .end = end, .text = std::move(text)};
}

} // namespace

TEST(EventCompose, TextEventSpansFromTheFirstSpanBeginToTheLastSpanEnd) {
    const auto segment = segment_starting_at(5000);
    const std::vector<lws::TextSpan> spans{span(100, 900, "hello"), span(900, 1700, "world")};

    const auto event = lws::text_event(segment, spans);

    const lws::TextEvent expected{.kind = lws::EventKind::Text,
                                  .t_start = 5100,
                                  .t_end = 6700,
                                  .text = "hello world",
                                  .after_gap = false};
    EXPECT_EQ(event, std::optional<lws::TextEvent>{expected});
}

TEST(EventCompose, TextEventCopiesTheDiscontinuityMarkIntoAfterGap) {
    const auto segment = segment_starting_at(5000, true);
    const std::vector<lws::TextSpan> spans{span(0, 800, "again")};

    const auto event = lws::text_event(segment, spans);

    const lws::TextEvent expected{.kind = lws::EventKind::Text,
                                  .t_start = 5000,
                                  .t_end = 5800,
                                  .text = "again",
                                  .after_gap = true};
    EXPECT_EQ(event, std::optional<lws::TextEvent>{expected});
}

TEST(EventCompose, AnEmptySpanListGivesNoTextEvent) {
    const auto segment = segment_starting_at(5000);
    EXPECT_FALSE(lws::text_event(segment, {}).has_value());
}

TEST(EventCompose, JoinedTextOfOneSpanIsItsTextUnchanged) {
    const std::vector<lws::TextSpan> spans{span(0, 10, "  keep as is ")};
    EXPECT_EQ(lws::joined_text(spans), "  keep as is ");
}

TEST(EventCompose, JoinedTextOfThreeSpansUsesOneSpaceBetweenEach) {
    const std::vector<lws::TextSpan> spans{span(0, 10, "one"), span(10, 20, "two"),
                                           span(20, 30, "three")};
    EXPECT_EQ(lws::joined_text(spans), "one two three");
}

TEST(EventCompose, JoinedTextOfNoSpansIsEmpty) { EXPECT_TRUE(lws::joined_text({}).empty()); }

TEST(EventCompose, NoticeEventKeepsTheKindAndRangeAndHasNoText) {
    const std::array kinds{lws::EventKind::Overrun, lws::EventKind::VadFailure,
                           lws::EventKind::SegmentDropped, lws::EventKind::DecodeFailure};
    std::ranges::for_each(kinds, [](lws::EventKind kind) {
        SCOPED_TRACE(static_cast<int>(kind));
        const auto event = lws::notice_event(lws::Notice{.kind = kind, .begin = 700, .end = 1212});
        const lws::TextEvent expected{
            .kind = kind, .t_start = 700, .t_end = 1212, .text = {}, .after_gap = false};
        EXPECT_EQ(event, expected);
    });
}
