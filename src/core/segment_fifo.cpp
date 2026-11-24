#include "segment_fifo.hpp"

#include "lws/segment.hpp"
#include "lws/text_event.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <utility>
#include <variant>

namespace lws {

namespace {

[[nodiscard]] bool is_segment(const QueueItem& item) {
    return std::holds_alternative<Segment>(item);
}

} // namespace

SegmentFifo::SegmentFifo(std::size_t max_segments) : max_segments_(max_segments) {
    assert(max_segments_ >= 1);
}

void SegmentFifo::push(QueueItem item) {
    if (std::holds_alternative<Segment>(item)) {
        if (segments_ >= max_segments_) {
            const auto oldest = std::ranges::find_if(items_, is_segment);
            assert(oldest != items_.end());
            const auto& dropped = std::get<Segment>(*oldest);
            const Notice notice{.kind = EventKind::SegmentDropped,
                                .begin = dropped.start,
                                .end = dropped.start + dropped.samples.size()};
            *oldest = notice;
            --segments_;
        }
        ++segments_;
    }
    items_.push_back(std::move(item));
    assert(std::cmp_equal(std::ranges::count_if(items_, is_segment), segments_));
}

std::optional<QueueItem> SegmentFifo::pop_front() {
    if (items_.empty()) {
        return std::nullopt;
    }
    auto item = std::move(items_.front());
    items_.pop_front();
    if (is_segment(item)) {
        --segments_;
    }
    return item;
}

std::size_t SegmentFifo::segment_count() const noexcept { return segments_; }

std::size_t SegmentFifo::size() const noexcept { return items_.size(); }

} // namespace lws
