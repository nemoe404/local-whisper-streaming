#include "shell/streamer_impl.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "core/event_compose.hpp"
#include "core/lifecycle.hpp"
#include "core/pump_schedule.hpp"
#include "core/segment_fifo.hpp"
#include "core/stream_clock.hpp"
#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "lws/spsc_ring.hpp"
#include "lws/streamer.hpp"
#include "lws/streamer_options.hpp"
#include "lws/text_event.hpp"
#include "shell/decoder.hpp"
#include "shell/segment_queue.hpp"
#include "shell/segmenter.hpp"
#include "shell/vad_engine.hpp"

namespace lws {

namespace {

// Samples the pump takes from the ring in one step. It bounds how far an overrun can be misplaced,
// how long a stop waits inside one VAD call, and the size of the scratch buffer.
constexpr std::size_t kPumpPiece = 4 * kVadFrameSamples;

// What the pump keeps between steps: its scratch buffer, the positions it waits at, the samples it
// has taken from the ring and the dropped samples attributed to a gap so far.
struct PumpState {
    std::vector<float> scratch = std::vector<float>(kPumpPiece);
    PumpSchedule schedule;
    std::uint64_t popped = 0;
    std::uint64_t skew = 0;
};

// The four counts that put the windower's next frame in the offered stream.
StreamPosition position_of(const Segmenter& segmenter, const PumpState& state) {
    return {.popped = state.popped,
            .skew = state.skew,
            .pending = segmenter.pending(),
            .fed = segmenter.fed_samples()};
}

void deliver(TextCallback& callback, const TextEvent& event) {
    if (callback) {
        callback(event);
    }
}

// A decoded segment gives at most one text event; a failed decode gives a DecodeFailure event
// over the segment's range. A segment too short to decode and a cancelled decode give nothing.
void report(Decoder& decoder, TextCallback& callback, const Segment& segment,
            const std::stop_token& stop) {
    const auto decoded = decoder.decode(segment, stop);
    if (!decoded) {
        const Notice failure{.kind = EventKind::DecodeFailure,
                             .begin = segment.start,
                             .end = segment.start + segment.samples.size()};
        deliver(callback, notice_event(failure));
        return;
    }
    if (const auto event = text_event(segment, decoded->spans)) {
        deliver(callback, *event);
    }
}

void report(Decoder& /*decoder*/, TextCallback& callback, const Notice& notice,
            const std::stop_token& /*stop*/) {
    deliver(callback, notice_event(notice));
}

} // namespace

// Owns everything the two threads share. Declaration order is destruction order in reverse: the
// decoder thread is declared last, so it is stopped and joined first, then the pump, and only then
// do the queue, the ring and the rest go. The control calls and the two engines are touched by the
// control thread only; each engine moves into the thread that uses it at start.
// The padding finding comes from the ring's cache-line-aligned indices, and the member order cannot
// change: it is the destruction order the threads rely on.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class Streamer::Impl {
public:
    Impl(Segmenter segmenter, Decoder decoder, const StreamerOptions& options)
        : poll_{options.poll}, ring_{options.ring_capacity_samples},
          segmenter_{std::move(segmenter)}, decoder_{std::move(decoder)},
          queue_{options.queue_segments} {}

    // Every public call goes through the lifecycle table first: one atomic load of the run state
    // and the pure transition. A refused call changes nothing.
    [[nodiscard]] std::expected<void, Error> on_text(TextCallback callback) {
        if (const auto next = admit(Control::SetCallback); !next) {
            return std::unexpected(next.error());
        }
        callback_ = std::move(callback);
        return {};
    }

    [[nodiscard]] std::expected<void, Error> start() {
        const auto next = admit(Control::Start);
        if (!next) {
            return std::unexpected(next.error());
        }
        assert(segmenter_ && decoder_);
        // Stored before anything launches, so a push that races the start is already admitted.
        state_.store(*next, std::memory_order_release);
        decoder_thread_ = std::jthread{
            [this, decoder = std::move(*decoder_), callback = std::move(callback_)](
                const std::stop_token& stop) mutable { run_decoder(decoder, callback, stop); }};
        pump_thread_ =
            std::jthread{[this, segmenter = std::move(*segmenter_)](
                             const std::stop_token& stop) mutable { run_pump(segmenter, stop); }};
        decoder_.reset();
        segmenter_.reset();
        return {};
    }

    [[nodiscard]] std::expected<void, Error> push(std::span<const float> samples) noexcept {
        if (const auto next = admit(Control::Push); !next) {
            return std::unexpected(next.error());
        }
        return ring_.push(samples);
    }

    // The target is the count of samples accepted so far. On the producer thread that covers every
    // push that returned before this call; from another thread it covers whatever push the call is
    // ordered after. The pump closes the window when it has taken exactly that many samples.
    [[nodiscard]] std::expected<void, Error> flush() {
        if (const auto next = admit(Control::Flush); !next) {
            return std::unexpected(next.error());
        }
        const std::uint64_t target = ring_.counters().accepted;
        {
            const std::scoped_lock lock{pump_mutex_};
            pending_flushes_.push_back(target);
            wake_requested_ = true;
        }
        pump_wake_.notify_one();
        return {};
    }

    [[nodiscard]] std::expected<void, Error> stop(StopMode mode) {
        const auto next = admit(Control::Stop);
        if (!next) {
            return std::unexpected(next.error());
        }
        // Stored before the threads are told, so a push that races the stop is refused or lands in
        // a ring that is then drained or discarded.
        state_.store(*next, std::memory_order_release);
        if (mode == StopMode::Drain) {
            // The same count as a flush target, read the same way: the pump drains the ring to it,
            // closes the open window, and the queue is closed behind that last segment.
            const std::uint64_t target = ring_.counters().accepted;
            {
                const std::scoped_lock lock{pump_mutex_};
                pending_finish_ = target;
                wake_requested_ = true;
            }
            pump_wake_.notify_one();
            join(pump_thread_);
            join(decoder_thread_);
        } else {
            stop_now();
        }
        return {};
    }

    // Destruction is an immediate stop from any state.
    ~Impl() { stop_now(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

private:
    [[nodiscard]] std::expected<RunState, Error> admit(Control call) const noexcept {
        return next_state(state_.load(std::memory_order_acquire), call);
    }

    // Asks both threads to end and joins them; queued work is dropped with the impl. The calling
    // thread must be neither of them, or the join would wait on itself.
    void stop_now() {
        assert(std::this_thread::get_id() != pump_thread_.get_id());
        assert(std::this_thread::get_id() != decoder_thread_.get_id());
        pump_thread_.request_stop();
        decoder_thread_.request_stop();
        join(pump_thread_);
        join(decoder_thread_);
    }

    static void join(std::jthread& thread) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    // Sleeps until a stop request, a wake-up or the poll interval, whichever comes first.
    void idle(const std::stop_token& stop) {
        std::unique_lock lock{pump_mutex_};
        std::ignore = pump_wake_.wait_for(lock, stop, poll_, [this] { return wake_requested_; });
        wake_requested_ = false;
    }

    // Segments that closed go to the queue in order, each placed in the offered stream by the
    // offset taken before this call. A failed detection hands back the window that was open, closed
    // as it stood. That window holds speech collected before the loss, so it gets the same offset
    // and goes to the queue ahead of the report of the lost frames. A VAD failure does not mark the
    // next segment as following a gap: the lost range is in the report itself.
    void feed(Segmenter& segmenter, std::span<const float> samples, const StreamPosition& before,
              const PumpState& state) {
        const std::uint64_t offset = stream_offset(before);
        auto closed = segmenter.push(samples);
        if (closed) {
            std::ranges::for_each(*closed, [this, offset](Segment& segment) {
                queue_.push(stamped(std::move(segment), offset));
            });
            return;
        }
        auto& failure = closed.error();
        if (failure.force_closed.has_value()) {
            queue_.push(stamped(std::move(*failure.force_closed), offset));
        }
        queue_.push(vad_failure_notice(before, position_of(segmenter, state)));
    }

    // Moves the targets the control thread left into the pump's own schedule. Called after the
    // ring's available count was read, so a flush made before a push that count includes is seen
    // in the same step.
    void take_targets(PumpSchedule& schedule, std::uint64_t popped) {
        const std::scoped_lock lock{pump_mutex_};
        std::ranges::for_each(pending_flushes_,
                              [&](std::uint64_t target) { schedule.add_flush(target, popped); });
        pending_flushes_.clear();
        if (pending_finish_.has_value()) {
            schedule.set_finish(*pending_finish_, popped);
            pending_finish_.reset();
        }
    }

    // The window a flush or the end of the stream closes goes to the queue, when there is one.
    void flush_window(Segmenter& segmenter, const PumpState& state) {
        const std::uint64_t offset = stream_offset(position_of(segmenter, state));
        if (auto window = segmenter.flush()) {
            queue_.push(stamped(std::move(*window), offset));
        }
    }

    // The ring lost audio just after the samples taken so far. The window that was open is closed
    // where the gap is, and goes to the queue before the report of the loss, so the speech before
    // the gap is delivered ahead of it. The report covers the dropped samples not yet reported;
    // two losses seen before the first mark is handled can leave the second with nothing new, and
    // then only the reset and the close happen. From here on the offset counts every dropped
    // sample.
    void handle_gap(Segmenter& segmenter, PumpState& state, std::uint64_t dropped_total) {
        const std::uint64_t offset = stream_offset(position_of(segmenter, state));
        if (auto window = segmenter.notify_overrun()) {
            queue_.push(stamped(std::move(*window), offset));
        }
        if (dropped_total > state.skew) {
            queue_.push(overrun_notice(state.popped, state.skew, dropped_total));
        }
        state.skew = dropped_total;
    }

    // Acts on every mark due at the samples taken so far. True when the stream ended.
    bool handle_due(Segmenter& segmenter, PumpState& state, bool& handled) {
        bool finished = false;
        std::ignore = std::ranges::find_if(std::views::iota(0), [&](std::int32_t) {
            const std::optional<DueMark> due = state.schedule.take_due(state.popped);
            if (!due.has_value()) {
                return true;
            }
            handled = true;
            switch (due->kind) {
            case MarkKind::Flush:
                flush_window(segmenter, state);
                return false;
            case MarkKind::Gap:
                handle_gap(segmenter, state, due->dropped_total);
                return false;
            case MarkKind::Finish:
                // The end of the stream. A loss flagged since the last step is handled first, so
                // it is never left unreported. Then the open window is emitted and the queue is
                // closed, so the decoder thread ends once it has worked off what is queued.
                if (ring_.take_discontinuity()) {
                    handle_gap(segmenter, state, ring_.counters().dropped);
                }
                flush_window(segmenter, state);
                queue_.close();
                finished = true;
                return true;
            }
            return true;
        });
        return finished;
    }

    // Takes `quota` samples from the ring and feeds them to the Segmenter. The position is read
    // before the pop, while the counts still describe the samples the Segmenter has seen.
    void pop_and_feed(Segmenter& segmenter, PumpState& state, std::size_t quota) {
        const std::span<float> piece = std::span<float>{state.scratch}.first(quota);
        const StreamPosition before = position_of(segmenter, state);
        state.popped += ring_.pop(piece);
        feed(segmenter, piece, before, state);
    }

    // One step of the pump. True ends the thread.
    //
    // The ring is looked at for an overrun, and a gap mark is put where one is found. The snapshot
    // of the available samples is taken first: every sample it counts was accepted before the flag
    // is read, so it belongs before the gap, and the mark sits after them. The ring keeps no
    // position for the loss, so this is the best the pump can do. The pump reads at most one piece
    // per step, because popping frees space, and space is what lets the producer accept audio after
    // the gap before the pump looks again; that bounds the error in the mark's position by the free
    // space at the overrun plus one piece. The dropped total is read right after the flag, so it
    // covers the loss that set it.
    bool pump_step(Segmenter& segmenter, PumpState& state, const std::stop_token& stop) {
        if (stop.stop_requested()) {
            return true;
        }
        const std::size_t available = ring_.available();
        if (ring_.take_discontinuity()) {
            state.schedule.add_gap(state.popped + available, ring_.counters().dropped);
        }
        take_targets(state.schedule, state.popped);
        const std::size_t quota =
            state.schedule.quota(available, state.scratch.size(), state.popped);
        if (quota > 0) {
            pop_and_feed(segmenter, state, quota);
        }
        bool handled = false;
        if (handle_due(segmenter, state, handled)) {
            return true;
        }
        if (quota == 0 && !handled) {
            idle(stop);
        }
        return false;
    }

    void run_pump(Segmenter& segmenter, const std::stop_token& stop) {
        PumpState state;
        std::ignore = std::ranges::find_if(
            std::views::iota(0), [&](std::int32_t) { return pump_step(segmenter, state, stop); });
    }

    void run_decoder(Decoder& decoder, TextCallback& callback, const std::stop_token& stop) {
        std::ignore = std::ranges::find_if(std::views::iota(0), [&](std::int32_t) {
            const auto item = queue_.pop(stop);
            if (!item.has_value()) {
                return true;
            }
            std::visit([&](const auto& value) { report(decoder, callback, value, stop); }, *item);
            return false;
        });
    }

    static_assert(std::atomic<RunState>::is_always_lock_free);

    std::atomic<RunState> state_{RunState::Created};
    std::chrono::milliseconds poll_;
    SpscRing ring_;
    std::optional<Segmenter> segmenter_;
    std::optional<Decoder> decoder_;
    TextCallback callback_;
    SegmentQueue queue_;
    std::mutex pump_mutex_;
    std::condition_variable_any pump_wake_;
    std::deque<std::uint64_t> pending_flushes_;
    std::optional<std::uint64_t> pending_finish_;
    bool wake_requested_ = false;
    std::jthread pump_thread_;
    std::jthread decoder_thread_;
};

std::expected<Streamer, Error> StreamerAccess::assemble(VadEngine engine, Decoder decoder,
                                                        const StreamerOptions& options) {
    if (const auto valid = validate(options); !valid) {
        return std::unexpected(valid.error());
    }
    auto segmenter = Segmenter::create(std::move(engine), options.vad);
    if (!segmenter) {
        return std::unexpected(segmenter.error());
    }
    return Streamer{
        std::make_unique<Streamer::Impl>(std::move(*segmenter), std::move(decoder), options)};
}

std::expected<Streamer, Error> Streamer::create(const StreamerOptions& options) {
    if (const auto valid = validate(options); !valid) {
        return std::unexpected(valid.error());
    }
    auto engine = VadEngine::open(options.vad_model_path);
    if (!engine) {
        return std::unexpected(engine.error());
    }
    auto decoder = Decoder::open(options.model_path, options.decoder);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    return StreamerAccess::assemble(std::move(*engine), std::move(*decoder), options);
}

Streamer::Streamer(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}

Streamer::Streamer(Streamer&&) noexcept = default;

Streamer& Streamer::operator=(Streamer&&) noexcept = default;

Streamer::~Streamer() = default;

std::expected<void, Error> Streamer::on_text(TextCallback callback) {
    assert(impl_);
    return impl_->on_text(std::move(callback));
}

std::expected<void, Error> Streamer::start() {
    assert(impl_);
    return impl_->start();
}

std::expected<void, Error> Streamer::push(std::span<const float> samples) noexcept {
    assert(impl_);
    return impl_->push(samples);
}

std::expected<void, Error> Streamer::flush() {
    assert(impl_);
    return impl_->flush();
}

std::expected<void, Error> Streamer::stop(StopMode mode) {
    assert(impl_);
    return impl_->stop(mode);
}

} // namespace lws
