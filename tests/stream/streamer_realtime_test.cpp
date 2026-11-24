// Real-time proof for Streamer::push: the capture thread's call allocates nothing, with the pump
// idle, with the pump working, and when the ring rejects a span.
//
// The allocation guard is scoped to one thread. The pump and the decoder allocate by design (the
// segmenter grows its window, the decoder builds text); only the thread that calls push must not.
// A constinit thread_local flag marks the armed thread, and the guard counts or aborts only when
// the allocating thread carries it. The allocation hook has two sources, chosen at compile time,
// and no test is ever skipped:
//   - address-sanitizer and thread-sanitizer builds install the sanitizer's malloc hook, because
//     replacing the global operator new does not link under -fsanitize=thread, and the hook sees
//     operator new allocations in both sanitizers;
//   - the release build replaces the global operator new and operator delete.
// Abort mode writes LWS-RT-ALLOCATION to stderr and aborts.

#include "stream/stream_fakes.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <latch>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "lws/options.hpp"
#include "lws/streamer.hpp"

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

enum class GuardMode : std::uint8_t { Off, Count, Abort };

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
// Reason: the allocation guard is process-wide state read from the allocator entry points.
std::atomic<GuardMode> g_mode{GuardMode::Off};
std::atomic<std::uint64_t> g_allocations{0};
// The armed thread. Constant-initialised, so reading it from the allocator never allocates.
constinit thread_local bool t_armed = false;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// Called from every allocation. Abort sets Off first so nothing below can recurse.
void on_allocation() noexcept {
    if (!t_armed) {
        return;
    }
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

// The calling thread is watched from here until disarm.
void arm(GuardMode mode) noexcept {
    g_mode.store(mode, std::memory_order_relaxed);
    t_armed = true;
}

void disarm() noexcept {
    t_armed = false;
    g_mode.store(GuardMode::Off, std::memory_order_relaxed);
}

// Escape hatch for the probe: clang elides an allocation whose pointer never escapes.
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
// This is an allocation counter for the real-time check (push must not allocate), not manual memory
// management.
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

using lws::test::EventLog;
using lws::test::IndexedVadLog;

constexpr std::size_t kPush = 160;

static_assert(noexcept(std::declval<lws::Streamer&>().push(std::declval<std::span<const float>>())),
              "the capture callback calls push: it must be noexcept");

// 2000 pushes of 160 samples cover 625 frames: utterances, then silence to the end.
std::vector<float> long_stream() {
    std::vector<float> values = lws::test::values_of(
        {{0.0F, 20}, {0.9F, 30}, {0.0F, 40}, {0.9F, 30}, {0.0F, 40}, {0.9F, 30}, {0.0F, 40}});
    values.resize((2000 * kPush) / lws::kVadFrameSamples, 0.0F);
    return lws::test::indexed_stream(values);
}

// Pushes `count` spans of 160 samples taken in turn from the stream and returns how many were
// refused. Nothing in here may allocate, and no gtest macro runs while the guard is armed.
std::size_t push_spans(lws::Streamer& streamer, std::span<const float> stream, std::size_t count) {
    std::size_t refused = 0;
    std::ranges::for_each(std::views::iota(std::size_t{0}, count), [&](std::size_t index) {
        const std::size_t start = (index * kPush) % (stream.size() - kPush + 1);
        if (!streamer.push(stream.subspan(start, kPush)).has_value()) {
            ++refused;
        }
    });
    return refused;
}

// Non-vacuity: the guard counts an allocation on the armed thread and ignores another thread's.
TEST(StreamerRealTime, ProbeCountsOnlyTheArmedThread) {
#ifdef LWS_RT_MALLOC_HOOK
    EXPECT_NE(g_hooks_installed, 0);
#endif
    std::latch go{1};
    std::latch allocated{1};
    // The helper is started before the guard is armed, so starting it is not counted either way.
    const std::jthread other{[&] {
        go.wait();
        const auto allocation = std::make_unique<int>(2);
        const int* volatile other_sink = allocation.get();
        std::ignore = other_sink;
        allocated.count_down();
    }};

    g_allocations.store(0, std::memory_order_relaxed);
    arm(GuardMode::Count);
    const auto own = std::make_unique<int>(1);
    g_sink = own.get();
    const std::uint64_t after_own = g_allocations.load(std::memory_order_relaxed);
    go.count_down();
    allocated.wait();
    const std::uint64_t after_other = g_allocations.load(std::memory_order_relaxed);
    disarm();

    EXPECT_EQ(after_own, 1U);
    EXPECT_EQ(after_other, 1U);
}

TEST(StreamerRealTime, PushNeverAllocatesWithThePumpIdle) {
    IndexedVadLog log;
    EventLog events;
    lws::Streamer streamer =
        lws::test::make_test_streamer(log, events, lws::test::position_decoder(), 200 * kPush);
    ASSERT_TRUE(streamer.start().has_value());
    const std::vector<float> silence(kPush, 0.0F);

    arm(GuardMode::Abort);
    const std::size_t refused = push_spans(streamer, silence, 200);
    disarm();

    EXPECT_EQ(refused, 0U);
    EXPECT_TRUE(streamer.stop(lws::StopMode::Drain).has_value());
}

TEST(StreamerRealTime, PushNeverAllocatesWithThePumpRunning) {
    const std::vector<float> stream = long_stream();
    IndexedVadLog log;
    EventLog events;
    lws::Streamer streamer = lws::test::make_test_streamer(
        log, events, lws::test::position_decoder(), stream.size(), 64);
    ASSERT_TRUE(streamer.start().has_value());

    arm(GuardMode::Abort);
    const std::size_t refused = push_spans(streamer, stream, 2000);
    disarm();

    EXPECT_EQ(refused, 0U);
    EXPECT_TRUE(streamer.stop(lws::StopMode::Drain).has_value());
    EXPECT_FALSE(events.events().empty());
}

TEST(StreamerRealTime, PushNeverAllocatesOnOverrun) {
    IndexedVadLog log;
    EventLog events;
    lws::Streamer streamer =
        lws::test::make_test_streamer(log, events, lws::test::position_decoder(), 4096);
    ASSERT_TRUE(streamer.start().has_value());
    const std::vector<float> too_long(8192, 0.0F);
    const std::vector<float> silence(kPush, 0.0F);
    std::expected<void, lws::Error> big{};
    std::size_t refused = 0;

    arm(GuardMode::Abort);
    big = streamer.push(too_long);
    refused = push_spans(streamer, silence, 20);
    disarm();

    ASSERT_FALSE(big.has_value());
    EXPECT_EQ(big.error(), lws::Error::Overrun);
    EXPECT_EQ(refused, 0U);
    EXPECT_TRUE(streamer.stop(lws::StopMode::Immediate).has_value());
}

} // namespace
