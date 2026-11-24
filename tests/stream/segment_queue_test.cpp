#include "shell/segment_queue.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <ranges>
#include <stop_token>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "core/segment_fifo.hpp"
#include "lws/segment.hpp"
#include "lws/text_event.hpp"

namespace {

using lws::Notice;
using lws::QueueItem;
using lws::SegmentQueue;
using namespace std::chrono_literals;

// How long a test waits for another thread before it calls a wake-up lost.
constexpr auto kBound = 10s;

QueueItem segment_at(std::uint64_t start, std::size_t size) {
    return lws::Segment{.samples = std::vector<float>(size, 0.0F), .start = start};
}

// The start of a segment item, nothing for a notice or an empty result.
std::optional<std::uint64_t> segment_start(const std::optional<QueueItem>& item) {
    if (!item.has_value()) {
        return std::nullopt;
    }
    if (const auto* segment = std::get_if<lws::Segment>(&*item)) {
        return segment->start;
    }
    return std::nullopt;
}

// The notice of a notice item, nothing for a segment or an empty result.
std::optional<Notice> notice_of(const std::optional<QueueItem>& item) {
    if (!item.has_value()) {
        return std::nullopt;
    }
    if (const auto* notice = std::get_if<Notice>(&*item)) {
        return *notice;
    }
    return std::nullopt;
}

// Closes the queue when a test ends, so a consumer thread that never got its wake-up still ends and
// the thread's join cannot hang a failing test. Declared after the thread, it runs before the join.
class CloseOnExit {
public:
    CloseOnExit(const CloseOnExit&) = delete;
    CloseOnExit& operator=(const CloseOnExit&) = delete;
    CloseOnExit(CloseOnExit&&) = delete;
    CloseOnExit& operator=(CloseOnExit&&) = delete;
    explicit CloseOnExit(SegmentQueue& target) : queue_{target} {}
    ~CloseOnExit() { queue_.close(); }

private:
    SegmentQueue& queue_;
};

} // namespace

TEST(SegmentQueue, PopReturnsNothingOnceStopIsRequestedFromAnotherThread) {
    SegmentQueue queue{2};
    std::stop_source source;
    std::promise<bool> got_item;
    std::future<bool> answer = got_item.get_future();
    const std::jthread consumer{
        [&] { got_item.set_value(queue.pop(source.get_token()).has_value()); }};
    const CloseOnExit rescue{queue};

    source.request_stop();

    ASSERT_EQ(answer.wait_for(kBound), std::future_status::ready);
    EXPECT_FALSE(answer.get());
}

TEST(SegmentQueue, PopWaitsForAnItemAndWakesWhenOneArrives) {
    SegmentQueue queue{2};
    std::stop_source source;
    std::promise<std::optional<std::uint64_t>> got_start;
    std::future<std::optional<std::uint64_t>> answer = got_start.get_future();
    const std::jthread consumer{
        [&] { got_start.set_value(segment_start(queue.pop(source.get_token()))); }};
    const CloseOnExit rescue{queue};

    EXPECT_EQ(answer.wait_for(50ms), std::future_status::timeout);
    queue.push(segment_at(4096, 512));

    ASSERT_EQ(answer.wait_for(kBound), std::future_status::ready);
    EXPECT_EQ(answer.get(), std::optional<std::uint64_t>{4096});
}

TEST(SegmentQueue, CloseWakesAWaitingPopWithNothing) {
    SegmentQueue queue{2};
    std::stop_source source;
    std::promise<bool> got_item;
    std::future<bool> answer = got_item.get_future();
    const std::jthread consumer{
        [&] { got_item.set_value(queue.pop(source.get_token()).has_value()); }};
    const CloseOnExit rescue{queue};

    queue.close();

    ASSERT_EQ(answer.wait_for(kBound), std::future_status::ready);
    EXPECT_FALSE(answer.get());
}

TEST(SegmentQueue, CloseHandsBackEveryItemInOrderThenNothing) {
    SegmentQueue queue{8};
    queue.push(segment_at(0, 512));
    queue.push(segment_at(1000, 512));
    queue.push(segment_at(2000, 512));
    queue.close();

    const std::stop_source source;
    const auto first = queue.pop(source.get_token());
    const auto second = queue.pop(source.get_token());
    const auto third = queue.pop(source.get_token());
    const auto after = queue.pop(source.get_token());

    EXPECT_EQ(segment_start(first), std::optional<std::uint64_t>{0});
    EXPECT_EQ(segment_start(second), std::optional<std::uint64_t>{1000});
    EXPECT_EQ(segment_start(third), std::optional<std::uint64_t>{2000});
    EXPECT_FALSE(after.has_value());
}

TEST(SegmentQueue, PushIntoAFullQueueReturnsAtOnceAndTheDropIsReportedAtItsPlace) {
    SegmentQueue queue{2};
    std::promise<void> pushed;
    const std::future<void> done = pushed.get_future();
    const std::jthread producer{[&] {
        std::ranges::for_each(
            std::views::iota(std::uint64_t{0}, std::uint64_t{4}),
            [&](std::uint64_t index) { queue.push(segment_at(index * 1000, 512)); });
        pushed.set_value();
    }};
    const CloseOnExit rescue{queue};

    ASSERT_EQ(done.wait_for(kBound), std::future_status::ready);
    queue.close();

    const std::stop_source source;
    const auto first = queue.pop(source.get_token());
    const auto second = queue.pop(source.get_token());
    const auto third = queue.pop(source.get_token());
    const auto fourth = queue.pop(source.get_token());
    const auto after = queue.pop(source.get_token());

    EXPECT_EQ(notice_of(first), (std::optional<Notice>{{lws::EventKind::SegmentDropped, 0, 512}}));
    EXPECT_EQ(notice_of(second),
              (std::optional<Notice>{{lws::EventKind::SegmentDropped, 1000, 1512}}));
    EXPECT_EQ(segment_start(third), std::optional<std::uint64_t>{2000});
    EXPECT_EQ(segment_start(fourth), std::optional<std::uint64_t>{3000});
    EXPECT_FALSE(after.has_value());
}
