#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <functional>
#include <future>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "lws/decode_policy.hpp"
#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "lws/streamer.hpp"
#include "lws/streamer_options.hpp"
#include "lws/text_event.hpp"
#include "shell/decoder.hpp"
#include "shell/segmenter.hpp"
#include "shell/streamer_impl.hpp"
#include "shell/vad_engine.hpp"
#include "vad/fake_vad.hpp"
#include "vad/segmenter_support.hpp"

// Fakes for the streamer tests: a VAD engine and a decoder that run no model and can tell which
// frames of the offered stream they were given. Not matched by the tests/stream/*_test.cpp glob,
// and outside the clang-tidy header filter like the other support headers.
//
// An indexed stream writes each frame's value into sample 0 (a probability or a marker, as the
// fake VAD reads it) and the frame's index into samples 1 to 511, so a fake can read back which
// frames a segment holds and a test can check every time against the offered stream.
namespace lws::test {

// One frame per value. The index is exact as a float below 2 to the 24th.
inline std::vector<float> indexed_stream(std::span<const float> frame_values) {
    std::vector<float> stream;
    stream.reserve(frame_values.size() * kVadFrameSamples);
    std::ranges::for_each(
        std::views::iota(std::size_t{0}, frame_values.size()), [&](std::size_t index) {
            stream.push_back(frame_values[index]);
            stream.insert(stream.end(), kVadFrameSamples - 1, static_cast<float>(index));
        });
    return stream;
}

// The frame values of a list of bursts, to feed indexed_stream.
inline std::vector<float> values_of(std::initializer_list<Burst> bursts) {
    std::vector<float> values;
    std::ranges::for_each(bursts, [&](const Burst& burst) {
        values.insert(values.end(), burst.frames, burst.value);
    });
    return values;
}

// What the engine saw. The pump thread writes it; a test reads it only after stop returned.
struct IndexedVadLog {
    struct Call {
        std::uint64_t first_index; // index of the first frame of the call
        std::size_t frames;
        bool failed;
        std::chrono::steady_clock::time_point at; // when the call began
    };
    std::vector<Call> calls;
    std::size_t resets = 0;
};

// The calls the engine failed, in order.
inline std::vector<IndexedVadLog::Call> failed_calls(const IndexedVadLog& log) {
    return log.calls | std::views::filter(&IndexedVadLog::Call::failed) |
           std::ranges::to<std::vector>();
}

// A frame whose sample 0 is this scores as silence, and makes the indexed engine's detect call
// wait, once, until the test releases it. It holds the pump inside the VAD while the test fills the
// ring.
inline constexpr float kStallMarker = -4.0F;

// How long a held detect call waits before it gives up, so a broken test cannot hold the pump.
inline constexpr std::chrono::seconds kStallBound{30};

// Holds one detect call and lets the test watch it start. The pump thread calls hold_once; the test
// waits for entry with a bound and decides when to release.
class StallGate {
public:
    // The first call marks entry and waits for the release or the bound; later calls pass.
    void hold_once() {
        std::unique_lock lock{mutex_};
        if (entered_) {
            return;
        }
        entered_ = true;
        changed_.notify_all();
        std::ignore = changed_.wait_for(lock, kStallBound, [this] { return released_; });
    }

    // False when the bound expires before a detect call entered.
    [[nodiscard]] bool wait_entered(std::chrono::milliseconds bound) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, bound, [this] { return entered_; });
    }

    void release() {
        {
            const std::scoped_lock lock{mutex_};
            released_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool released_ = false;
};

// Splits the input into ceil(n / 512) chunks like the other fake engine and reads each chunk's
// sample 0: the fail marker fails the whole call, the stall marker holds the call once (see
// StallGate) and scores as silence, any other value is clamped to [0, 1] and is the chunk's
// probability. A call is logged with the index its first chunk carries (0 for a call too short to
// carry one, like the startup probe). `delay` makes every call take at least that long. The test
// owns the log and the gate and outlives the engine.
inline VadEngine indexed_engine(IndexedVadLog& log, StallGate* stall = nullptr,
                                std::chrono::milliseconds delay = std::chrono::milliseconds{0}) {
    return VadEngine{
        .detect = [&log, stall, delay,
                   probs = std::vector<float>{}](std::span<const float> samples) mutable
            -> std::expected<std::span<const float>, Error> {
            const auto began = std::chrono::steady_clock::now();
            const std::size_t chunks = (samples.size() + kVadFrameSamples - 1) / kVadFrameSamples;
            const auto firsts =
                std::views::iota(std::size_t{0}, chunks) |
                std::views::transform([&](std::size_t i) { return samples[i * kVadFrameSamples]; });
            if (stall != nullptr &&
                std::ranges::any_of(firsts, [](float v) { return v == kStallMarker; })) {
                stall->hold_once();
            }
            if (delay.count() > 0) {
                std::this_thread::sleep_for(delay);
            }
            const bool failed =
                std::ranges::any_of(firsts, [](float v) { return v == kFailMarker; });
            const std::uint64_t first_index =
                samples.size() >= 2 ? static_cast<std::uint64_t>(samples[1]) : 0;
            log.calls.push_back({first_index, chunks, failed, began});
            if (failed) {
                return std::unexpected(Error::VadFailed);
            }
            probs.clear();
            std::ranges::transform(firsts, std::back_inserter(probs),
                                   [](float v) { return std::clamp(v, 0.0F, 1.0F); });
            return std::span<const float>{probs};
        },
        .reset = [&log] { ++log.resets; },
    };
}

inline std::string position_text(std::uint64_t first_index, std::uint64_t last_index) {
    return fmt::format("f{} l{}", first_index, last_index);
}

// The first and last frame index a position text names.
struct FrameRange {
    std::uint64_t first = 0;
    std::uint64_t last = 0;
};

// Reads back what position_text wrote. Text that does not have that shape gives {0, 0}, which no
// test stream's utterance has as its range except one that really starts and ends at frame 0.
inline FrameRange frame_range_of(const std::string& text) {
    if (text.size() < 4 || text.front() != 'f') {
        return {};
    }
    FrameRange range;
    const char* const end = text.data() + text.size();
    const auto first = std::from_chars(text.data() + 1, end, range.first);
    if (first.ec != std::errc{} || first.ptr + 2 > end) {
        return {};
    }
    std::ignore = std::from_chars(first.ptr + 2, end, range.last);
    return range;
}

// The text a decoder gives for the frames [first, last] of the segment it was given.
using TextOfFrames = std::string (*)(std::uint64_t first, std::uint64_t last);

// The backend behind the test decoders: the text names the first and last frame index of the
// segment it was given, formatted by `text_of`, over one span from the segment's start to its end.
// `before_run` runs at the top of every decode, with that decode's stop token, so a test can hold a
// decode. The run with the 1-based number `failing_run` returns -1 without producing text.
inline DecodeBackend
position_backend(std::move_only_function<void(const std::stop_token&)> before_run = {},
                 TextOfFrames text_of = position_text,
                 std::optional<std::size_t> failing_run = std::nullopt) {
    struct State {
        std::size_t size = 0;
        std::uint64_t first = 0;
        std::uint64_t last = 0;
        std::size_t runs = 0;
    };
    auto state = std::make_shared<State>();
    return DecodeBackend{
        .run = [state, failing_run, before_run = std::move(before_run)](
                   std::span<const float> samples, std::stop_token stop) mutable -> std::int32_t {
            if (before_run) {
                before_run(stop);
            }
            ++state->runs;
            if (failing_run == state->runs) {
                return -1;
            }
            state->size = samples.size();
            state->first = static_cast<std::uint64_t>(samples[1]);
            state->last = static_cast<std::uint64_t>(samples[samples.size() - 511]);
            return 0;
        },
        .spans =
            [state, text_of] {
                const std::int64_t end_cs = static_cast<std::int64_t>((state->size + 159) / 160);
                return std::vector<RawSpan>{
                    {.t0_cs = 0, .t1_cs = end_cs, .text = text_of(state->first, state->last)}};
            },
    };
}

// A decoder whose text names the first and last frame index of the segment it was given.
inline Decoder position_decoder() { return Decoder{position_backend()}; }

// Lets a test hold a decode and watch it start. The decoder thread marks entry; the test waits for
// it with a bound and decides when to let go. Every wait here also ends on a stop request, so a
// held decode can never outlive the thread that runs it.
class DecodeGate {
public:
    // Marks entry. The first call also waits for release or a stop request; later calls pass.
    void enter_first(const std::stop_token& stop) {
        std::unique_lock lock{mutex_};
        const bool first = !entered_;
        entered_ = true;
        changed_.notify_all();
        if (first) {
            std::ignore = changed_.wait(lock, stop, [this] { return released_; });
        }
    }

    // Marks entry and waits for a stop request only.
    void enter_and_wait_for_stop(const std::stop_token& stop) {
        std::unique_lock lock{mutex_};
        entered_ = true;
        changed_.notify_all();
        std::ignore = changed_.wait(lock, stop, [] { return false; });
    }

    // False when the bound expires before a decode entered.
    [[nodiscard]] bool wait_entered(std::chrono::milliseconds bound) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, bound, [this] { return entered_; });
    }

    void release() {
        {
            const std::scoped_lock lock{mutex_};
            released_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable_any changed_;
    bool entered_ = false;
    bool released_ = false;
};

// The position decoder, but its first decode waits until the gate is released.
inline Decoder gated_position_decoder(DecodeGate& gate) {
    return Decoder{
        position_backend([&gate](const std::stop_token& stop) { gate.enter_first(stop); })};
}

// The text of a decoder that begins and ends on the same word, so a ceiling cut that is merged
// loses exactly that word and a merge that should not happen shows.
inline std::string seam_text(std::uint64_t first, std::uint64_t last) {
    return fmt::format("seam f{} l{} seam", first, last);
}

// A decoder whose text is seam_text of the segment's frames. Its first decode waits on the gate
// when one is given; the run with the given 1-based number fails.
inline Decoder seam_decoder(DecodeGate* gate = nullptr,
                            std::optional<std::size_t> failing_run = std::nullopt) {
    std::move_only_function<void(const std::stop_token&)> before_run;
    if (gate != nullptr) {
        before_run = [gate](const std::stop_token& stop) { gate->enter_first(stop); };
    }
    return Decoder{position_backend(std::move(before_run), seam_text, failing_run)};
}

// whisper's code for a run its abort callback ended.
inline constexpr std::int32_t kWhisperAbort = -6;

// Every decode marks entry and then waits for its stop token, and returns the abort code. It
// behaves as a decode that only a stop request can end.
inline Decoder stop_blocking_decoder(DecodeGate& gate) {
    return Decoder{DecodeBackend{
        .run = [&gate](std::span<const float>, std::stop_token stop) -> std::int32_t {
            gate.enter_and_wait_for_stop(stop);
            return kWhisperAbort;
        },
        .spans = [] { return std::vector<RawSpan>{}; },
    }};
}

// What a Segmenter cuts from the stream fed in pieces of `chunk` samples and then flushed: the
// reference the streamer's events are compared against.
inline std::vector<Segment> direct_segments(std::span<const float> stream, std::size_t chunk,
                                            const Options& options = Options{}) {
    IndexedVadLog log;
    auto segmenter = Segmenter::create(indexed_engine(log), options).value();
    auto pushed = push_chunked(segmenter, stream, chunk);
    if (auto window = segmenter.flush()) {
        pushed.segments.push_back(std::move(*window));
    }
    return std::move(pushed.segments);
}

// The reference for a stream that is flushed along the way: what a Segmenter cuts when the stream
// is fed in pieces of `chunk` samples, split at each flush position and flushed there, then
// flushed at the end. Positions are sample counts in increasing order.
inline std::vector<Segment> direct_segments_flushed(std::span<const float> stream,
                                                    std::size_t chunk,
                                                    std::span<const std::size_t> flush_positions) {
    IndexedVadLog log;
    auto segmenter = Segmenter::create(indexed_engine(log), Options{}).value();
    std::vector<Segment> segments;
    const auto flush = [&] {
        if (auto window = segmenter.flush()) {
            segments.push_back(std::move(*window));
        }
    };
    const auto feed = [&](std::span<const float> part) {
        auto pushed = push_chunked(segmenter, part, chunk);
        std::ranges::move(pushed.segments, std::back_inserter(segments));
    };
    std::size_t at = 0;
    std::ranges::for_each(flush_positions, [&](std::size_t position) {
        feed(stream.subspan(at, position - at));
        flush();
        at = position;
    });
    feed(stream.subspan(at));
    flush();
    return segments;
}

// The event a segment must produce with the position decoder: its exact stream range and the
// frames it holds.
inline TextEvent expected_text_event(const Segment& segment) {
    const std::uint64_t end = segment.start + segment.samples.size();
    return TextEvent{
        .kind = EventKind::Text,
        .t_start = segment.start,
        .t_end = end,
        .text = position_text(segment.start / kVadFrameSamples, (end / kVadFrameSamples) - 1),
        .after_gap = false};
}

inline std::vector<TextEvent> expected_text_events(std::span<const Segment> segments) {
    return segments | std::views::transform(expected_text_event) | std::ranges::to<std::vector>();
}

// The leg this build is, for the lines tests print: release, asan-ubsan or tsan.
constexpr const char* sanitizer_leg_name() {
#if LWS_STREAM_SANITIZER_LEG == 0
    return "release";
#elifdef __has_feature
#if __has_feature(thread_sanitizer)
    return "tsan";
#else
    return "asan-ubsan";
#endif
#else
    return "asan-ubsan";
#endif
}

// Runs the body on its own thread and waits for it with a bound. A body that does not finish in
// time cannot be stopped, so the hang is reported and the process ends: a hang fails fast instead
// of waiting for the test runner's timeout.
inline void run_bounded(const char* name, std::chrono::milliseconds bound,
                        std::move_only_function<void()> body) {
    std::packaged_task<void()> task{std::move(body)};
    std::future<void> done = task.get_future();
    std::jthread worker{std::move(task)};
    if (done.wait_for(bound) != std::future_status::ready) {
        std::ignore = std::fprintf(stderr, "STREAMER-HANG %s\n", name);
        std::abort();
    }
    worker.join();
    done.get();
}

// Collects the events the callback delivers. record runs on the decoder thread; events is read
// after stop returned.
class EventLog {
public:
    void record(const TextEvent& event) noexcept {
        {
            const std::scoped_lock lock{mutex_};
            events_.push_back(event);
        }
        changed_.notify_all();
    }

    // False when the bound expires before `count` events arrived.
    [[nodiscard]] bool wait_for_count(std::size_t count, std::chrono::milliseconds bound) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, bound, [&] { return events_.size() >= count; });
    }

    [[nodiscard]] std::vector<TextEvent> events() const {
        const std::scoped_lock lock{mutex_};
        return events_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<TextEvent> events_;
};

// A streamer over the indexed engine and the given decoder, with its events going to `events`. The
// ring and the queue are as large as the caller says. A failure throws, which fails the test.
inline Streamer make_test_streamer(IndexedVadLog& log, EventLog& events, Decoder decoder,
                                   std::size_t ring_samples, std::size_t queue_segments = 8,
                                   StallGate* stall = nullptr, const Options& vad = Options{}) {
    StreamerOptions options;
    options.ring_capacity_samples = ring_samples;
    options.queue_segments = queue_segments;
    options.vad = vad;
    auto streamer =
        StreamerAccess::assemble(indexed_engine(log, stall), std::move(decoder), options).value();
    streamer.on_text([&events](const TextEvent& event) noexcept { events.record(event); }).value();
    return streamer;
}

// Pushes the stream in pieces of `piece` samples (the last one shorter). False when any push was
// refused.
inline bool push_pieces(Streamer& streamer, std::span<const float> stream, std::size_t piece) {
    return std::ranges::all_of(
        stream | std::views::chunk(static_cast<std::ptrdiff_t>(piece)), [&](auto samples) {
            return streamer.push(std::span<const float>{samples.begin(), samples.end()})
                .has_value();
        });
}

} // namespace lws::test
