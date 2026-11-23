// Real-time proof for SpscRing::push: it allocates nothing and never waits for the consumer.
//
// The allocation guard has one state (modes Off, Count, Abort) and two sources, chosen at compile
// time, and no test is ever skipped:
//   - address-sanitizer and thread-sanitizer builds install the sanitizer's malloc hook, because
//     replacing the global operator new does not link under -fsanitize=thread (the TSan C++
//     runtime already defines it), and the hook sees operator new allocations in both sanitizers;
//   - the release build replaces the global operator new and operator delete.
// Abort mode writes LWS-RT-ALLOCATION to stderr and aborts; the seeded proof greps for that token.

#include "lws/spsc_ring.hpp"
#include "ring_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <future>
#include <iterator>
#include <latch>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// Set once, here, so the two mechanisms below sit in opposite branches of one conditional.
#ifdef __has_feature
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
// Reason: a compile-time mechanism switch, not a constant.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LWS_RT_MALLOC_HOOK
#include <sanitizer/allocator_interface.h>
#endif
#endif

namespace {

using lws::test::bits_of;
using lws::test::pattern;

enum class GuardMode : std::uint8_t { Off, Count, Abort };

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
// Reason: the allocation guard is process-wide state read from the allocator entry points.
std::atomic<GuardMode> g_mode{GuardMode::Off};
std::atomic<std::uint64_t> g_allocations{0};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// Called from every allocation. Abort sets Off first so nothing below can recurse.
void on_allocation() noexcept {
    switch (g_mode.load(std::memory_order_relaxed)) {
    case GuardMode::Off:
        return;
    case GuardMode::Count:
        g_allocations.fetch_add(1, std::memory_order_relaxed);
        return;
    case GuardMode::Abort:
        g_mode.store(GuardMode::Off, std::memory_order_relaxed);
        // stderr is unbuffered, so this write does not allocate.
        std::ignore = std::fputs("LWS-RT-ALLOCATION\n", stderr);
        std::abort();
    }
}

// Escape hatch for the self-test: clang elides an allocation whose pointer never escapes.
// Reason: the volatile sink is the point, it makes the pointer escape.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
int* volatile g_sink = nullptr;

} // namespace

#ifdef LWS_RT_MALLOC_HOOK

namespace {

void on_malloc(const volatile void* /*ptr*/, std::size_t /*size*/) { on_allocation(); }

void on_free(const volatile void* /*ptr*/) {}

// Installed from a namespace-scope initialiser: it runs on the main thread before any test thread
// starts, which the sanitizer interface requires. The count of installed hooks must be non-zero.
// Reason: the initialiser calls a C function that cannot throw.
// NOLINTNEXTLINE(cert-err58-cpp)
const int g_hooks_installed = __sanitizer_install_malloc_and_free_hooks(on_malloc, on_free);

} // namespace

#else

// Replacement allocation functions: guard, then forward to the C allocator.
// NOLINTBEGIN(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,misc-new-delete-overloads,misc-const-correctness)
// Reason: the global operator new replacement has to forward to the C allocator.
// This is an allocation counter for the real-time check (push must not allocate), not manual memory management.
void* operator new(std::size_t size) {
    on_allocation();
    void* ptr = std::malloc(size != 0 ? size : 1);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void operator delete(void* ptr) noexcept { std::free(ptr); }

void operator delete(void* ptr, std::size_t /*size*/) noexcept { std::free(ptr); }
// NOLINTEND(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,misc-new-delete-overloads,misc-const-correctness)

#endif

namespace {

static_assert(noexcept(std::declval<lws::SpscRing&>().push(std::declval<std::span<const float>>())),
              "the capture callback calls push: it must be noexcept");

// Non-vacuity (Pitfall 6): the guard must see an allocation, on every leg.
TEST(SpscRingRealTime, AllocationProbeCountsAnEscapedAllocation) {
#ifdef LWS_RT_MALLOC_HOOK
    EXPECT_NE(g_hooks_installed, 0);
#endif
    g_allocations.store(0, std::memory_order_relaxed);
    g_mode.store(GuardMode::Count, std::memory_order_relaxed);
    auto allocation = std::make_unique<int>(1);
    g_sink = allocation.get();
    g_mode.store(GuardMode::Off, std::memory_order_relaxed);
    EXPECT_GE(g_allocations.load(std::memory_order_relaxed), 1U);
}

// RING-01, success criterion 3: accepted, straddling and overrun pushes allocate nothing. Abort
// mode is armed around the loop; no gtest macro runs and no container grows while it is armed.
TEST(SpscRingRealTime, PushNeverAllocatesOnAcceptStraddleOrOverrun) {
    constexpr std::size_t kCapacity = 1000;
    constexpr std::size_t kIterations = 1200;
    lws::SpscRing ring{kCapacity};
    const std::vector<float> source = pattern(0, 211);
    const std::vector<float> oversized = pattern(0, kCapacity + 1);
    std::vector<float> popped(97);
    std::size_t accepted_pushes = 0;
    std::size_t overruns = 0;
    std::size_t accepted_samples = 0;
    const auto push_span = [&](std::span<const float> span) {
        if (ring.push(span).has_value()) {
            ++accepted_pushes;
            accepted_samples += span.size();
        } else {
            ++overruns;
        }
    };

    g_mode.store(GuardMode::Abort, std::memory_order_relaxed);
    std::ranges::for_each(std::views::iota(std::size_t{0}, kIterations), [&](std::size_t i) {
        push_span(std::span<const float>{source}.first((i % 211) + 1));
        if (i % 10 == 9) {
            push_span(oversized);
        }
        std::ignore = ring.pop(popped);
        std::ignore = ring.pop(popped);
    });
    g_mode.store(GuardMode::Off, std::memory_order_relaxed);

    EXPECT_GT(accepted_pushes, 0U);
    EXPECT_GT(overruns, 0U);
    EXPECT_GT(accepted_samples, kCapacity); // the copies wrapped
}

// Success criterion 3, never blocks: the consumer is parked on a latch and never pops, so a
// blocking push would never return.
TEST(SpscRingRealTime, PushFinishesAgainstAStalledConsumer) {
    constexpr std::size_t kCapacity = 1024;
    constexpr std::size_t kSpan = 64;
    constexpr std::size_t kSpans = 100000;
    lws::SpscRing ring{kCapacity};
    const std::vector<float> source = pattern(0, kCapacity);
    std::vector<float> received;
    received.reserve(kCapacity + kSpan);
    std::latch release{1};

    std::jthread consumer{[&] {
        release.wait();
        std::array<float, 97> buffer{};
        std::ranges::find_if(std::views::iota(0U), [&](unsigned) {
            const std::size_t n = ring.pop(buffer);
            std::ranges::copy(std::span<const float>{buffer}.first(n),
                              std::back_inserter(received));
            return n == 0;
        });
    }};
    std::packaged_task<void()> task{[&] {
        std::ranges::for_each(std::views::iota(std::size_t{0}, kSpans), [&](std::size_t i) {
            const std::size_t start = (i % (kCapacity / kSpan)) * kSpan;
            std::ignore = ring.push(std::span<const float>{source}.subspan(start, kSpan));
        });
    }};
    const std::future<void> done = task.get_future();
    std::jthread producer{std::move(task)};

    const bool finished = done.wait_for(std::chrono::seconds{10}) == std::future_status::ready;
    release.count_down();
    if (!finished) {
        FAIL() << "push did not finish while the consumer was parked: it blocks";
    }
    producer.join();
    consumer.join();

    const lws::RingCounters counters = ring.counters();
    EXPECT_EQ(counters.offered, kSpans * kSpan);
    EXPECT_EQ(counters.accepted, kCapacity);
    EXPECT_EQ(counters.dropped, (kSpans * kSpan) - kCapacity);
    EXPECT_TRUE(ring.take_discontinuity());
    EXPECT_EQ(bits_of(received), bits_of(source));
}

} // namespace
