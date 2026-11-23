#pragma once

#include <cassert>
#include <cstddef>

// Pure index arithmetic for a single-producer single-consumer ring. The core owns no storage; the
// shell's ring uses these functions. No whisper, spdlog or thread type crosses this header.
namespace lws {

class RingIndex {
public:
    constexpr explicit RingIndex(std::size_t capacity) noexcept : capacity_{capacity} {
        assert(capacity > 0);
    }

    [[nodiscard]] constexpr std::size_t capacity() const noexcept { return capacity_; }

    // Position inside the storage for a free-running counter value.
    [[nodiscard]] constexpr std::size_t wrap(std::size_t position) const noexcept {
        return position % capacity_;
    }

    // Occupancy from two free-running counters. Equal wrapped indices mean empty or full; the
    // counters tell them apart, so the caller never subtracts. Unsigned subtraction stays correct
    // when a counter wraps past the std::size_t range, so write >= read is not asserted.
    [[nodiscard]] constexpr std::size_t used(std::size_t read, std::size_t write) const noexcept {
        const std::size_t occupied = write - read;
        assert(occupied <= capacity_);
        return occupied;
    }

    [[nodiscard]] constexpr std::size_t free_space(std::size_t used) const noexcept {
        assert(used <= capacity_);
        return capacity_ - used;
    }

private:
    std::size_t capacity_;
};

} // namespace lws
