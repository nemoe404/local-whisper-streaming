#include "pump_schedule.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>

namespace lws {

namespace {

// The nearest position any mark waits at, or the largest count when none is waiting.
std::uint64_t nearest_mark(const std::deque<std::uint64_t>& flushes,
                           const std::deque<std::pair<std::uint64_t, std::uint64_t>>& gaps,
                           const std::optional<std::uint64_t>& finish) {
    std::uint64_t nearest = std::numeric_limits<std::uint64_t>::max();
    if (!flushes.empty()) {
        nearest = std::min(nearest, flushes.front());
    }
    if (!gaps.empty()) {
        nearest = std::min(nearest, gaps.front().first);
    }
    return std::min(nearest, finish.value_or(nearest));
}

} // namespace

void PumpSchedule::add_flush(std::uint64_t target, std::uint64_t popped) {
    const std::uint64_t position = std::max(target, popped);
    assert(flushes_.empty() || position >= flushes_.back());
    flushes_.push_back(position);
}

void PumpSchedule::add_gap(std::uint64_t position, std::uint64_t dropped_total) {
    gaps_.emplace_back(std::min(position, finish_.value_or(position)), dropped_total);
}

void PumpSchedule::set_finish(std::uint64_t target, std::uint64_t popped) {
    assert(!finish_.has_value());
    const std::uint64_t position = std::max(target, popped);
    finish_ = position;
    std::ranges::for_each(gaps_,
                          [position](auto& gap) { gap.first = std::min(gap.first, position); });
}

std::size_t PumpSchedule::quota(std::size_t available, std::size_t piece,
                                std::uint64_t popped) const noexcept {
    const std::uint64_t nearest = nearest_mark(flushes_, gaps_, finish_);
    assert(nearest >= popped);
    const std::uint64_t room = nearest - popped;
    return static_cast<std::size_t>(std::min<std::uint64_t>({available, piece, room}));
}

std::optional<DueMark> PumpSchedule::take_due(std::uint64_t popped) {
    if (!flushes_.empty() && flushes_.front() == popped) {
        flushes_.pop_front();
        return DueMark{.kind = MarkKind::Flush, .dropped_total = 0};
    }
    if (!gaps_.empty() && gaps_.front().first == popped) {
        const std::uint64_t dropped_total = gaps_.front().second;
        gaps_.pop_front();
        return DueMark{.kind = MarkKind::Gap, .dropped_total = dropped_total};
    }
    if (finish_ == popped) {
        return DueMark{.kind = MarkKind::Finish, .dropped_total = 0};
    }
    return std::nullopt;
}

bool PumpSchedule::finish_set() const noexcept { return finish_.has_value(); }

} // namespace lws
