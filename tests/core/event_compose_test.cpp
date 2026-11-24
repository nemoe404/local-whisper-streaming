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
#include <tuple>
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

namespace {

lws::Segment seam_segment(std::uint64_t start, lws::CloseReason reason, bool continues_previous) {
    lws::Segment segment = segment_starting_at(start);
    segment.reason = reason;
    segment.continues_previous = continues_previous;
    return segment;
}

// One span holding the text, from the segment's first sample.
std::vector<lws::TextSpan> said(std::string text) { return {span(0, 800, std::move(text))}; }

std::string text_of(const std::optional<lws::TextEvent>& event) {
    return event.has_value() ? event->text : std::string{"<no event>"};
}

constexpr auto kCeiling = lws::CloseReason::Ceiling;
constexpr auto kHangover = lws::CloseReason::Hangover;

} // namespace

TEST(SeamText, AContinuationLosesTheWordsItSharesWithItsPredecessor) {
    lws::SeamText seam;
    const auto first = said("we went to the");
    const auto second = said("the store opened");

    const auto one = seam.compose(seam_segment(0, kCeiling, false), first, true);
    const auto two = seam.compose(seam_segment(1000, kHangover, true), second, true);

    EXPECT_EQ(text_of(one), "we went to the");
    EXPECT_EQ(text_of(two), "store opened");
    // The merged event still starts at its first span.
    EXPECT_EQ(two.transform([](const lws::TextEvent& event) { return event.t_start; }),
              std::optional<std::uint64_t>{1000});
}

TEST(SeamText, TheSameInputsAfterAnInterruptionAreNotMerged) {
    lws::SeamText seam;
    const auto first = said("we went to the");
    const auto second = said("the store opened");

    const auto one = seam.compose(seam_segment(0, kCeiling, false), first, true);
    seam.interrupt();
    const auto two = seam.compose(seam_segment(1000, kHangover, true), second, true);

    EXPECT_EQ(text_of(one), "we went to the");
    EXPECT_EQ(text_of(two), "the store opened");
    EXPECT_TRUE(seam.reference().empty());
}

TEST(SeamText, ASegmentThatDidNotCloseAtTheCeilingLeavesNothingToMergeWith) {
    lws::SeamText seam;
    const auto first = said("we went to the");
    const auto second = said("the store opened");

    std::ignore = seam.compose(seam_segment(0, kHangover, false), first, true);
    EXPECT_TRUE(seam.reference().empty());
    const auto two = seam.compose(seam_segment(1000, kHangover, true), second, true);

    EXPECT_EQ(text_of(two), "the store opened");
}

TEST(SeamText, NothingIsMergedWhenMergingIsOff) {
    lws::SeamText seam;
    const auto first = said("we went to the");
    const auto second = said("the store opened");

    std::ignore = seam.compose(seam_segment(0, kCeiling, false), first, false);
    const auto two = seam.compose(seam_segment(1000, kHangover, true), second, false);

    EXPECT_EQ(text_of(two), "the store opened");
}

TEST(SeamText, ASegmentThatDoesNotContinueItsPredecessorIsNotMerged) {
    lws::SeamText seam;
    const auto first = said("we went to the");
    const auto second = said("the store opened");

    std::ignore = seam.compose(seam_segment(0, kCeiling, false), first, true);
    const auto two = seam.compose(seam_segment(1000, kHangover, false), second, true);

    EXPECT_EQ(text_of(two), "the store opened");
}

TEST(SeamText, ATextThatMergesToNothingGivesNoEventButStillBecomesTheReference) {
    lws::SeamText seam;
    const auto first = said("we went to the");
    const auto second = said("to the");
    const auto third = said("the store");

    std::ignore = seam.compose(seam_segment(0, kCeiling, false), first, true);
    const auto two = seam.compose(seam_segment(1000, kCeiling, true), second, true);
    EXPECT_FALSE(two.has_value());
    EXPECT_EQ(seam.reference(), "to the");
    const auto three = seam.compose(seam_segment(2000, kHangover, true), third, true);

    EXPECT_EQ(text_of(three), "store");
}

TEST(SeamText, ASegmentWithoutSpansGivesNoEventAndEmptiesTheReference) {
    lws::SeamText seam;
    const auto first = said("we went to the");

    std::ignore = seam.compose(seam_segment(0, kCeiling, false), first, true);
    const auto none = seam.compose(seam_segment(1000, kCeiling, true), {}, true);

    EXPECT_FALSE(none.has_value());
    EXPECT_TRUE(seam.reference().empty());
}
