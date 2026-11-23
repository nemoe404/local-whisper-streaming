#pragma once

#include "lws/options.hpp"
#include "lws/ring_index.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>
#include <vector>

// Shell type: slot storage and atomics. It depends on core headers (Error, RingIndex); no core
// header includes it. One producer thread calls push, one consumer thread calls pop and
// available. That contract is a documented precondition, checked by tests, not by an assertion.
namespace lws {

class SpscRing {
public:
    // Allocates `capacity` float slots once. capacity > 0 is a programmer error check inside
    // RingIndex. A span longer than the capacity is always rejected, so the owner sizes the ring
    // from its largest push.
    explicit SpscRing(std::size_t capacity) : index_{capacity}, slots_(capacity) {}
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    // A moved ring would strand the reference the other thread holds.
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;
    ~SpscRing() = default;

    [[nodiscard]] std::size_t capacity() const noexcept { return index_.capacity(); }

    // Producer thread only. All or nothing: a span that does not fit returns Error::Overrun and
    // writes no slot. The span is copied before push returns and never stored. Wait-free: a fixed
    // number of steps, no loop, no retry, no read-modify-write, no allocation.
    [[nodiscard]] std::expected<void, Error> push(std::span<const float> samples) noexcept {
        const std::size_t n = samples.size();
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t r = read_.load(std::memory_order_acquire);
        if (n > index_.free_space(index_.used(r, w))) {
            return std::unexpected(Error::Overrun);
        }
        // Pinned for the seeded proofs: the plain slot copies into the free region.
        copy_in(w, samples);
        // Pinned for the seeded proofs: this release store publishes the slot writes.
        write_.store(w + n, std::memory_order_release);
        return {};
    }

    // Consumer thread only. Copies min(available, out.size()) samples in FIFO order and returns
    // that count.
    [[nodiscard]] std::size_t pop(std::span<float> out) noexcept {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t n = std::min(index_.used(r, w), out.size());
        copy_out(r, out.first(n));
        // Pinned for the seeded proofs: this release store hands the slots back to the producer.
        read_.store(r + n, std::memory_order_release);
        return n;
    }

    // Consumer thread only. Samples ready to pop.
    [[nodiscard]] std::size_t available() const noexcept {
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t r = read_.load(std::memory_order_relaxed);
        return index_.used(r, w);
    }

private:
    // The counters are free-running std::size_t values. used() stays correct across a counter
    // wrap, and wrap() is continuous across it only for a power-of-two capacity. At 16 kHz the
    // 64-bit counter wraps about 3.7e7 years from now, so the ring does not handle it.
    // At most two copies: one up to the end of the storage, one from its start.
    void copy_in(std::size_t position, std::span<const float> samples) noexcept {
        const std::size_t start = index_.wrap(position);
        const std::size_t first = std::min(samples.size(), index_.capacity() - start);
        std::ranges::copy(samples.first(first),
                          slots_.begin() + static_cast<std::ptrdiff_t>(start));
        std::ranges::copy(samples.subspan(first), slots_.begin());
    }

    // Mirror of copy_in, reading the used region.
    void copy_out(std::size_t position, std::span<float> out) noexcept {
        const std::size_t start = index_.wrap(position);
        const std::size_t first = std::min(out.size(), index_.capacity() - start);
        const std::span<const float> slots{slots_};
        std::ranges::copy(slots.subspan(start, first), out.begin());
        std::ranges::copy(slots.first(out.size() - first), out.subspan(first).begin());
    }

    static constexpr std::size_t kCacheLine = 64;
    RingIndex index_;          // read-only after construction
    std::vector<float> slots_; // plain float slots, sized once
    static_assert(std::is_same_v<decltype(slots_)::value_type, float>);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    alignas(kCacheLine) std::atomic<std::size_t> write_{0}; // producer-written, free-running
    alignas(kCacheLine) std::atomic<std::size_t> read_{0};  // consumer-written, free-running
};

} // namespace lws
