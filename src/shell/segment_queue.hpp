#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stop_token>

#include "core/segment_fifo.hpp"

// Shell header: the queue between the pump thread and the decoder thread. The drop policy is the
// pure SegmentFifo; this type adds the one lock and the waiting around it. The lock lives inside
// this component and no foreign code runs under it: items are moved in and out by value. A push
// never waits for space, because the drop policy decides what a full queue loses.

namespace lws {

class SegmentQueue {
public:
    // max_segments must be at least 1.
    explicit SegmentQueue(std::size_t max_segments);

    // Never waits. The queue must not be closed. A segment pushed into a full queue replaces the
    // oldest waiting segment by a SegmentDropped notice, at its place.
    void push(QueueItem item);

    // Waits until an item is ready, the queue is closed, or stop is requested. Returns nothing when
    // stop was requested (waiting items stay behind) or when the queue is closed and empty;
    // otherwise the front item. After close, every remaining item comes back in order first.
    [[nodiscard]] std::optional<QueueItem> pop(const std::stop_token& stop);

    // No more items will be pushed. Wakes every waiter.
    void close();

private:
    std::mutex mutex_;
    std::condition_variable_any ready_;
    SegmentFifo fifo_;
    bool closed_ = false;
};

} // namespace lws
