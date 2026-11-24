#include "stream/stream_fakes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iterator>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/core.h>

#include "lws/options.hpp"
#include "lws/streamer.hpp"
#include "lws/streamer_options.hpp"
#include "lws/text_event.hpp"

// A producer thread, the pump and the decoder run together, on fake engines only, so any report
// from the thread sanitizer is in this library's own code. The last case measures how long a push
// into an idle pump waits before the pump looks at it.
namespace {

using lws::test::IndexedVadLog;
using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

constexpr milliseconds kBound{10000};
constexpr milliseconds kPollBound{20000};
constexpr milliseconds kStopBound{2000};
constexpr std::size_t kFrame = lws::kVadFrameSamples;
constexpr std::size_t kPiece = 160;

// Events with the thread each one was delivered on. record runs on the decoder thread; the rest is
// read by the test, either after stop returned or through wait_for_count.
class ThreadedEvents {
public:
    void record(const lws::TextEvent& event) noexcept {
        {
            const std::scoped_lock lock{mutex_};
            events_.push_back(event);
            threads_.push_back(std::this_thread::get_id());
        }
        changed_.notify_all();
    }

    // False when the bound expires before `count` events arrived.
    [[nodiscard]] bool wait_for_count(std::size_t count, milliseconds bound) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, bound, [&] { return events_.size() >= count; });
    }

    [[nodiscard]] std::vector<lws::TextEvent> events() const {
        const std::scoped_lock lock{mutex_};
        return events_;
    }

    [[nodiscard]] std::vector<std::thread::id> threads() const {
        const std::scoped_lock lock{mutex_};
        return threads_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<lws::TextEvent> events_;
    std::vector<std::thread::id> threads_;
};

lws::Streamer make_streamer(IndexedVadLog& log, ThreadedEvents& events, std::size_t ring_samples) {
    lws::StreamerOptions options;
    options.ring_capacity_samples = ring_samples;
    auto streamer = lws::StreamerAccess::assemble(lws::test::indexed_engine(log),
                                                  lws::test::position_decoder(), options)
                        .value();
    streamer.on_text([&events](const lws::TextEvent& event) noexcept { events.record(event); })
        .value();
    return streamer;
}

// Twenty utterances of 30 speech frames, each after 40 silent frames, then 40 silent frames.
std::vector<float> twenty_utterances() {
    auto values = std::views::repeat(lws::test::values_of({{0.0F, 40}, {0.9F, 30}}), 20) |
                  std::views::join | std::ranges::to<std::vector>();
    values.insert(values.end(), 40, 0.0F);
    return lws::test::indexed_stream(values);
}

// What the producer thread reports once it is joined.
struct ProducerReport {
    std::thread::id id;
    bool pushed_everything = false;
    bool saw_first_event = false;
};

// The producer pushes the first half, waits for the first event, then pushes the rest, so pushes,
// pump work and callbacks overlap in every run.
ProducerReport push_from_producer_thread(lws::Streamer& streamer, ThreadedEvents& events,
                                         std::span<const float> all) {
    const std::size_t half = (all.size() / 2 / kPiece) * kPiece;
    ProducerReport report;
    const std::jthread producer{[&] {
        report.id = std::this_thread::get_id();
        const bool first = lws::test::push_pieces(streamer, all.first(half), kPiece);
        report.saw_first_event = events.wait_for_count(1, kBound);
        const bool rest = lws::test::push_pieces(streamer, all.subspan(half), kPiece);
        report.pushed_everything = first && rest;
    }};
    return report;
}

bool all_on_one_thread(const std::vector<std::thread::id>& threads) {
    return std::ranges::all_of(threads, [&](std::thread::id id) { return id == threads.front(); });
}

// Everything the producer case observes.
struct RunOutcome {
    ProducerReport report;
    std::vector<lws::TextEvent> events;
    std::vector<std::thread::id> threads;
    bool controls_accepted = false;
};

RunOutcome run_with_producer(std::span<const float> stream) {
    IndexedVadLog log;
    ThreadedEvents events;
    lws::Streamer streamer = make_streamer(log, events, stream.size());
    RunOutcome outcome;
    outcome.controls_accepted = streamer.start().has_value();
    outcome.report = push_from_producer_thread(streamer, events, stream);
    outcome.controls_accepted = outcome.controls_accepted && streamer.flush().has_value() &&
                                streamer.stop(lws::StopMode::Drain).has_value();
    outcome.events = events.events();
    outcome.threads = events.threads();
    return outcome;
}

// The names of the promises the run breaks.
std::vector<std::string> broken_promises(const RunOutcome& run,
                                         std::span<const lws::TextEvent> expected) {
    const bool one_thread = run.threads.size() == expected.size() && all_on_one_thread(run.threads);
    const bool other_thread = one_thread && run.threads.front() != run.report.id &&
                              run.threads.front() != std::this_thread::get_id();
    const std::array<std::pair<bool, const char*>, 5> checks{{
        {run.controls_accepted, "a control call was refused"},
        {run.report.pushed_everything, "the producer's pushes were refused"},
        {run.report.saw_first_event, "no event arrived while the producer was pushing"},
        {std::ranges::equal(run.events, expected), "the events differ from the direct segmenter's"},
        {other_thread, "events were not all delivered on a thread of their own"},
    }};
    return checks | std::views::filter([](const auto& c) { return !c.first; }) |
           std::views::transform([](const auto& c) { return std::string(c.second); }) |
           std::ranges::to<std::vector>();
}

void producer_pump_and_decoder_together() {
    const std::vector<float> stream = twenty_utterances();
    const std::vector<lws::TextEvent> expected =
        lws::test::expected_text_events(lws::test::direct_segments(stream, kFrame));
    ASSERT_EQ(expected.size(), 20U);

    const RunOutcome run = run_with_producer(stream);
    EXPECT_EQ(broken_promises(run, expected), std::vector<std::string>{});
}

// Pushes pieces of the pattern in a cycle until a push is refused with NotRunning. True when one
// was; false when the bound on the number of pushes ran out first.
bool push_until_not_running(lws::Streamer& streamer, std::span<const float> pattern) {
    const auto pushes = std::views::iota(0, 200000);
    const auto refused = std::ranges::find_if(pushes, [&](std::int32_t i) {
        const std::size_t at = (static_cast<std::size_t>(i) * kPiece) % pattern.size();
        const auto pushed = streamer.push(pattern.subspan(at, kPiece));
        if (pushed) {
            return false;
        }
        if (pushed.error() == lws::Error::NotRunning) {
            return true;
        }
        // The ring is full: let the pump catch up instead of spinning.
        std::this_thread::sleep_for(std::chrono::microseconds{200});
        return false;
    });
    return refused != pushes.end();
}

// A producer that keeps pushing while the test thread stops the streamer with Immediate: pushes
// are refused with NotRunning after the stop, the stop returns promptly, and the producer ends.
void push_racing_immediate_stop() {
    const std::vector<float> pattern = lws::test::indexed_stream(lws::test::values_of(
        {{0.0F, 20}, {0.9F, 30}, {0.0F, 40}, {0.9F, 30}, {0.0F, 40}, {0.9F, 30}, {0.0F, 10}}));
    IndexedVadLog log;
    ThreadedEvents events;
    lws::Streamer streamer = make_streamer(log, events, pattern.size());
    ASSERT_TRUE(streamer.start().has_value());

    std::atomic<bool> saw_not_running{false};
    std::jthread producer{
        [&] { saw_not_running.store(push_until_not_running(streamer, pattern)); }};

    const bool first_event = events.wait_for_count(1, kBound);
    const auto before = Clock::now();
    const bool stopped = streamer.stop(lws::StopMode::Immediate).has_value();
    const auto latency = std::chrono::duration_cast<milliseconds>(Clock::now() - before);
    producer.join();

    EXPECT_TRUE(first_event && stopped);
    EXPECT_LT(latency, kStopBound);
    EXPECT_TRUE(saw_not_running.load());
}

double median_of(std::vector<double> values) {
    std::ranges::sort(values);
    const std::size_t middle = values.size() / 2;
    return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

constexpr std::size_t kWarmupFrames = 10;
constexpr std::size_t kStreamFrames = 60;

// Process CPU, in milliseconds, spent over one idle second.
double idle_cpu_ms_over_one_second() {
    const std::clock_t before = std::clock();
    std::this_thread::sleep_for(std::chrono::seconds{1});
    const std::clock_t after = std::clock();
    return 1000.0 * static_cast<double>(after - before) / CLOCKS_PER_SEC;
}

// Pushes frames kWarmupFrames and on one at a time, each after a pause longer than the poll and
// at a different phase of it, and returns when each push returned. False in `accepted` when a push
// was refused.
std::vector<Clock::time_point> push_idle_frames(lws::Streamer& streamer, std::span<const float> all,
                                                bool& accepted) {
    std::vector<Clock::time_point> pushed_at;
    std::ranges::transform(
        std::views::iota(kWarmupFrames, kStreamFrames), std::back_inserter(pushed_at),
        [&](std::size_t i) {
            std::this_thread::sleep_for(milliseconds{40 + ((7 * i) % 32)});
            accepted = streamer.push(all.subspan(i * kFrame, kFrame)).has_value() && accepted;
            return Clock::now();
        });
    return pushed_at;
}

// For each pushed frame, the time from its push to the first detect call that carried it, in
// milliseconds. A frame no call carried is left out.
std::vector<double> wake_delays(const IndexedVadLog& log,
                                std::span<const Clock::time_point> pushed_at) {
    std::vector<double> delays;
    std::ranges::for_each(std::views::iota(kWarmupFrames, kStreamFrames), [&](std::size_t i) {
        const auto call = std::ranges::find(log.calls, static_cast<std::uint64_t>(i),
                                            &IndexedVadLog::Call::first_index);
        if (call != log.calls.end()) {
            const std::chrono::duration<double, std::milli> delay =
                call->at - pushed_at[i - kWarmupFrames];
            delays.push_back(delay.count());
        }
    });
    return delays;
}

// How long a push into an idle pump waits before the pump's next detect call, over 50 pushes, and
// what an idle streamer costs in process CPU.
void measure_poll_wake() {
    const std::vector<float> stream =
        lws::test::indexed_stream(std::vector<float>(kStreamFrames, 0.0F));
    const std::span<const float> all{stream};
    IndexedVadLog log;
    ThreadedEvents events;
    lws::Streamer streamer = make_streamer(log, events, stream.size());
    ASSERT_TRUE(streamer.start().has_value());

    ASSERT_TRUE(streamer.push(all.first(kWarmupFrames * kFrame)).has_value());
    const double idle_cpu_ms = idle_cpu_ms_over_one_second();
    bool accepted = true;
    const std::vector<Clock::time_point> pushed_at = push_idle_frames(streamer, all, accepted);
    ASSERT_TRUE(streamer.stop(lws::StopMode::Drain).has_value());
    ASSERT_TRUE(accepted);

    const std::vector<double> delays = wake_delays(log, pushed_at);
    ASSERT_EQ(delays.size(), kStreamFrames - kWarmupFrames);
    EXPECT_TRUE(std::ranges::all_of(delays, [](double ms) { return ms < 1000.0; }));
    fmt::println("PUMP-POLL leg={} poll_ms={} trials={} wake_delay_ms_median={:.1f} "
                 "wake_delay_ms_max={:.1f} idle_cpu_ms_per_s={:.1f}",
                 lws::test::sanitizer_leg_name(), lws::StreamerOptions{}.poll.count(),
                 delays.size(), median_of(delays), std::ranges::max(delays), idle_cpu_ms);
}

} // namespace

TEST(StreamerThreads, ProducerPumpAndDecoderRunTogether) {
    lws::test::run_bounded("producer-pump-decoder", kBound, producer_pump_and_decoder_together);
}

TEST(StreamerThreads, PushRacingImmediateStopIsRefusedCleanly) {
    lws::test::run_bounded("push-racing-stop", kBound, push_racing_immediate_stop);
}

TEST(StreamerThreads, PollWakeDelayIsMeasured) {
    lws::test::run_bounded("poll-wake-delay", kPollBound, measure_poll_wake);
}
