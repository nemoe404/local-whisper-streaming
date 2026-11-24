// The queue between the pump and the decoder drops the oldest whole segment when it is full and
// leaves a notice in its place, so every loss reaches the caller as exactly one event at the right
// position in the stream. These tests pin that policy as plain values.
#include "core/segment_fifo.hpp"

#include "lws/segment.hpp"
#include "lws/text_event.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <variant>
#include <vector>

namespace {

lws::Segment segment_at(std::uint64_t start, std::size_t size) {
    lws::Segment segment;
    segment.samples.assign(size, 0.0F);
    segment.start = start;
    return segment;
}

lws::Notice notice(lws::EventKind kind, std::uint64_t begin, std::uint64_t end) {
    return lws::Notice{.kind = kind, .begin = begin, .end = end};
}

// What a test compares: an item's kind of entry and the stream range it covers.
struct Row {
    bool is_segment = false;
    lws::EventKind kind = lws::EventKind::Text; // meaningful for a notice only
    std::uint64_t begin = 0;
    std::uint64_t end = 0;

    friend bool operator==(const Row&, const Row&) = default;
};

Row segment_row(std::uint64_t start, std::size_t size) {
    return Row{
        .is_segment = true, .kind = lws::EventKind::Text, .begin = start, .end = start + size};
}

Row notice_row(lws::EventKind kind, std::uint64_t begin, std::uint64_t end) {
    return Row{.is_segment = false, .kind = kind, .begin = begin, .end = end};
}

Row row_of(const lws::QueueItem& item) {
    struct Visitor {
        Row operator()(const lws::Segment& segment) const {
            return segment_row(segment.start, segment.samples.size());
        }
        Row operator()(const lws::Notice& n) const { return notice_row(n.kind, n.begin, n.end); }
    };
    return std::visit(Visitor{}, item);
}

std::vector<Row> drain_rows(lws::SegmentFifo& fifo) {
    std::vector<Row> rows;
    std::ranges::find_if(std::views::iota(0), [&](int) {
        const auto item = fifo.pop_front();
        if (item) {
            rows.push_back(row_of(*item));
        }
        return !item;
    });
    return rows;
}

} // namespace

TEST(SegmentFifo, PushPastCapacityReplacesTheOldestSegmentsInPlaceByNotices) {
    lws::SegmentFifo fifo(2);
    std::ranges::for_each(std::views::iota(0U, 5U), [&](std::uint32_t i) {
        fifo.push(segment_at(std::uint64_t{i} * 1000U, 512));
    });
    EXPECT_EQ(fifo.size(), 5U);
    EXPECT_EQ(fifo.segment_count(), 2U);

    const std::vector<Row> expected{
        notice_row(lws::EventKind::SegmentDropped, 0, 512),
        notice_row(lws::EventKind::SegmentDropped, 1000, 1512),
        notice_row(lws::EventKind::SegmentDropped, 2000, 2512),
        segment_row(3000, 512),
        segment_row(4000, 512),
    };
    EXPECT_EQ(drain_rows(fifo), expected);
}

TEST(SegmentFifo, OverrunNoticeSurvivesAndTheDroppedSegmentKeepsItsOwnPlace) {
    lws::SegmentFifo fifo(1);
    fifo.push(segment_at(0, 512));
    fifo.push(notice(lws::EventKind::Overrun, 700, 900));
    fifo.push(segment_at(1000, 512));
    EXPECT_EQ(fifo.segment_count(), 1U);
    EXPECT_EQ(fifo.size(), 3U);

    const std::vector<Row> expected{
        notice_row(lws::EventKind::SegmentDropped, 0, 512),
        notice_row(lws::EventKind::Overrun, 700, 900),
        segment_row(1000, 512),
    };
    EXPECT_EQ(drain_rows(fifo), expected);
}

TEST(SegmentFifo, NoticesNeverCountTowardTheCapacityAndAreAllKeptInOrder) {
    lws::SegmentFifo fifo(1);
    std::vector<Row> expected;
    std::ranges::for_each(std::views::iota(0U, 20U), [&](std::uint32_t i) {
        const auto kind = i % 2U == 0U ? lws::EventKind::VadFailure : lws::EventKind::Overrun;
        fifo.push(notice(kind, std::uint64_t{i}, std::uint64_t{i} + 1U));
        expected.push_back(notice_row(kind, std::uint64_t{i}, std::uint64_t{i} + 1U));
    });
    EXPECT_EQ(fifo.size(), 20U);
    EXPECT_EQ(fifo.segment_count(), 0U);
    EXPECT_EQ(drain_rows(fifo), expected);
}

TEST(SegmentFifo, PopFrontOnAnEmptyFifoReturnsNothing) {
    lws::SegmentFifo fifo(3);
    EXPECT_FALSE(fifo.pop_front().has_value());
    EXPECT_EQ(fifo.size(), 0U);
    EXPECT_EQ(fifo.segment_count(), 0U);
}

TEST(SegmentFifo, PoppingASegmentFreesRoomSoTheNextPushDropsNothing) {
    lws::SegmentFifo fifo(2);
    fifo.push(segment_at(0, 512));
    fifo.push(segment_at(1000, 512));
    const auto first = fifo.pop_front();
    EXPECT_EQ(first.has_value() ? row_of(*first) : Row{}, segment_row(0, 512));
    EXPECT_EQ(fifo.segment_count(), 1U);

    fifo.push(segment_at(2000, 512));
    EXPECT_EQ(fifo.segment_count(), 2U);
    EXPECT_EQ(fifo.size(), 2U);
    const std::vector<Row> expected{segment_row(1000, 512), segment_row(2000, 512)};
    EXPECT_EQ(drain_rows(fifo), expected);
}

TEST(SegmentFifo, ADroppedNoticeCarriesTheRangeOfTheSegmentSamplesNotAnythingElse) {
    lws::SegmentFifo fifo(1);
    fifo.push(segment_at(123456, 1536));
    fifo.push(segment_at(200000, 512));
    const std::vector<Row> expected{
        notice_row(lws::EventKind::SegmentDropped, 123456, 123456 + 1536),
        segment_row(200000, 512),
    };
    EXPECT_EQ(drain_rows(fifo), expected);
}
