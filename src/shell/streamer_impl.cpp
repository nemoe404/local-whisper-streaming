#include "shell/streamer_impl.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
#include "core/segment_fifo.hpp"
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

    void on_text(TextCallback callback) { callback_ = std::move(callback); }

    void start() {
        if (!segmenter_ || !decoder_) {
            return;
        }
        decoder_thread_ = std::jthread{
            [this, decoder = std::move(*decoder_), callback = std::move(callback_)](
                const std::stop_token& stop) mutable { run_decoder(decoder, callback, stop); }};
        pump_thread_ =
            std::jthread{[this, segmenter = std::move(*segmenter_)](
                             const std::stop_token& stop) mutable { run_pump(segmenter, stop); }};
        decoder_.reset();
        segmenter_.reset();
    }

    [[nodiscard]] std::expected<void, Error> push(std::span<const float> samples) noexcept {
        return ring_.push(samples);
    }

    void stop(StopMode mode) {
        if (mode == StopMode::Drain) {
            {
                const std::scoped_lock lock{pump_mutex_};
                finish_target_ = ring_.counters().accepted;
                wake_requested_ = true;
            }
            pump_wake_.notify_one();
        } else {
            pump_thread_.request_stop();
            decoder_thread_.request_stop();
        }
        join(pump_thread_);
        join(decoder_thread_);
    }

private:
    static void join(std::jthread& thread) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    [[nodiscard]] std::optional<std::uint64_t> finish_target() {
        const std::scoped_lock lock{pump_mutex_};
        return finish_target_;
    }

    // Sleeps until a stop request, a wake-up or the poll interval, whichever comes first.
    void idle(const std::stop_token& stop) {
        std::unique_lock lock{pump_mutex_};
        std::ignore = pump_wake_.wait_for(lock, stop, poll_, [this] { return wake_requested_; });
        wake_requested_ = false;
    }

    // Segments that closed go to the queue in order. A failed detection hands back the window that
    // was open, closed as it stood, so speech already collected is not lost.
    void feed(Segmenter& segmenter, std::span<const float> samples) {
        auto closed = segmenter.push(samples);
        if (closed) {
            std::ranges::for_each(*closed,
                                  [this](Segment& segment) { queue_.push(std::move(segment)); });
            return;
        }
        auto& failure = closed.error();
        if (failure.force_closed.has_value()) {
            queue_.push(std::move(*failure.force_closed));
        }
    }

    // The end of the stream: the open window is emitted and the queue is closed, so the decoder
    // thread ends once it has worked off what is queued.
    void finish(Segmenter& segmenter) {
        if (auto window = segmenter.flush()) {
            queue_.push(std::move(*window));
        }
        queue_.close();
    }

    // One step of the pump. True ends the thread.
    bool pump_step(Segmenter& segmenter, std::span<float> scratch, std::uint64_t& popped,
                   const std::stop_token& stop) {
        if (stop.stop_requested()) {
            return true;
        }
        const std::size_t available = ring_.available();
        const std::optional<std::uint64_t> target = finish_target();
        std::size_t quota = std::min(available, scratch.size());
        if (target.has_value()) {
            assert(*target >= popped);
            quota = std::min<std::size_t>(quota, static_cast<std::size_t>(*target - popped));
        }
        if (quota > 0) {
            const std::span<float> piece = scratch.first(quota);
            popped += ring_.pop(piece);
            feed(segmenter, piece);
        }
        if (target.has_value() && popped == *target) {
            finish(segmenter);
            return true;
        }
        if (quota == 0) {
            idle(stop);
        }
        return false;
    }

    void run_pump(Segmenter& segmenter, const std::stop_token& stop) {
        std::vector<float> scratch(kPumpPiece);
        std::uint64_t popped = 0;
        std::ignore = std::ranges::find_if(std::views::iota(0), [&](std::int32_t) {
            return pump_step(segmenter, scratch, popped, stop);
        });
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

    std::chrono::milliseconds poll_;
    SpscRing ring_;
    std::optional<Segmenter> segmenter_;
    std::optional<Decoder> decoder_;
    TextCallback callback_;
    SegmentQueue queue_;
    std::mutex pump_mutex_;
    std::condition_variable_any pump_wake_;
    std::optional<std::uint64_t> finish_target_;
    bool wake_requested_ = false;
    std::jthread pump_thread_;
    std::jthread decoder_thread_;
};

std::expected<Streamer, Error> StreamerAccess::assemble(VadEngine engine, Decoder decoder,
                                                        const StreamerOptions& options) {
    auto segmenter = Segmenter::create(std::move(engine), options.vad);
    if (!segmenter) {
        return std::unexpected(segmenter.error());
    }
    return Streamer{
        std::make_unique<Streamer::Impl>(std::move(*segmenter), std::move(decoder), options)};
}

std::expected<Streamer, Error> Streamer::create(const StreamerOptions& options) {
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
    impl_->on_text(std::move(callback));
    return {};
}

std::expected<void, Error> Streamer::start() {
    assert(impl_);
    impl_->start();
    return {};
}

std::expected<void, Error> Streamer::push(std::span<const float> samples) noexcept {
    assert(impl_);
    return impl_->push(samples);
}

std::expected<void, Error> Streamer::flush() {
    assert(impl_);
    return {};
}

std::expected<void, Error> Streamer::stop(StopMode mode) {
    assert(impl_);
    impl_->stop(mode);
    return {};
}

} // namespace lws
