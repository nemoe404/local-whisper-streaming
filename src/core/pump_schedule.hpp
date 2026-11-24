#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

// Internal core header. Every position at which the pump must stop reading and act is a count of
// samples taken from the stream, never a time and never a look at whether the ring happens to be
// empty: the segments a stream yields must not depend on how the threads were scheduled. This
// value type holds those positions (flush targets, gaps and the finish target) and says how far
// the pump may read before the next one, and which one is due. It has no lock and no thread.
namespace lws {

enum class MarkKind : std::uint8_t { Flush, Gap, Finish };

struct DueMark {
    MarkKind kind = MarkKind::Flush;
    // Gap only: the ring's dropped count read right after the flag.
    std::uint64_t dropped_total = 0;
};

class PumpSchedule {
public:
    // A flush at `target` samples. A target below the samples already taken (a flush from a thread
    // not ordered with the producer) is raised to `popped`. Targets must not decrease.
    void add_flush(std::uint64_t target, std::uint64_t popped);

    // A loss seen at `position` samples, with the ring's dropped total. After the finish target is
    // set a position beyond it is lowered to it, so no gap is left behind the last position the
    // pump reaches.
    void add_gap(std::uint64_t position, std::uint64_t dropped_total);

    // The end of the stream at `target` samples, raised to `popped` when below it. Set once.
    // Every gap beyond it is lowered to it.
    void set_finish(std::uint64_t target, std::uint64_t popped);

    // How many samples the pump may take now: at most what the ring holds, at most one piece, and
    // never past the nearest mark. Zero while a mark is due.
    [[nodiscard]] std::size_t quota(std::size_t available, std::size_t piece,
                                    std::uint64_t popped) const noexcept;

    // Removes and returns the mark due at `popped` samples, in the order flush, gap, finish, or
    // nothing. The finish mark stays, so the pump that has handled it asks no further.
    [[nodiscard]] std::optional<DueMark> take_due(std::uint64_t popped);

    [[nodiscard]] bool finish_set() const noexcept;

private:
    std::deque<std::uint64_t> flushes_;
    std::deque<std::pair<std::uint64_t, std::uint64_t>> gaps_; // position, dropped total
    std::optional<std::uint64_t> finish_;
};

} // namespace lws
