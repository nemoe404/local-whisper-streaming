#pragma once

#include "lws/segment.hpp"
#include "lws/text_event.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <variant>

// Internal core header. The queue policy between the pump and the decoder, as pure values: no
// lock, no thread. The shell wraps one of these in its own lock.
namespace lws {

// A loss that sits in the queue until the decoder thread reports it. begin and end count samples
// of the offered stream, end one past the last.
struct Notice {
    EventKind kind = EventKind::Overrun;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;

    friend bool operator==(const Notice&, const Notice&) = default;
};

using QueueItem = std::variant<Segment, Notice>;

// First in, first out. The capacity counts segments only. Pushing a segment into a full queue
// replaces the oldest waiting segment, in its own place, by a SegmentDropped notice and frees its
// samples. A notice is never evicted and never counts toward the capacity: each loss must reach the
// caller as exactly one event, at the position in the stream where it happened.
class SegmentFifo {
public:
    // max_segments must be at least 1.
    explicit SegmentFifo(std::size_t max_segments);

    void push(QueueItem item);

    // The front item, or nothing when the queue is empty.
    [[nodiscard]] std::optional<QueueItem> pop_front();

    // Segments waiting.
    [[nodiscard]] std::size_t segment_count() const noexcept;
    // Segments and notices waiting.
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::deque<QueueItem> items_;
    std::size_t max_segments_;
    std::size_t segments_ = 0;
};

} // namespace lws
