#include "shell/segment_queue.hpp"

#include <cassert>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

#include "core/segment_fifo.hpp"

namespace lws {

SegmentQueue::SegmentQueue(std::size_t max_segments) : fifo_{max_segments} {}

void SegmentQueue::push(QueueItem item) {
    {
        const std::scoped_lock lock{mutex_};
        assert(!closed_);
        fifo_.push(std::move(item));
    }
    // Notified after the lock is released, so the woken thread does not wake into a held lock.
    ready_.notify_one();
}

std::optional<QueueItem> SegmentQueue::pop(const std::stop_token& stop) {
    std::unique_lock lock{mutex_};
    ready_.wait(lock, stop, [this] { return closed_ || fifo_.size() > 0; });
    if (stop.stop_requested()) {
        return std::nullopt;
    }
    return fifo_.pop_front();
}

void SegmentQueue::close() {
    {
        const std::scoped_lock lock{mutex_};
        closed_ = true;
    }
    ready_.notify_all();
}

} // namespace lws
