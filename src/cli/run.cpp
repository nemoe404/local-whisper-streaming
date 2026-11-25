#include "cli/run.hpp"

#include "cli/event_format.hpp"
#include "cli/pace.hpp"
#include "cli/pcm_assembler.hpp"
#include "lws/streamer_options.hpp"
#include "lws/text_event.hpp"

#include <fmt/format.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace lws_cli {

namespace {

constexpr std::size_t kReadBytes = 16384;
// A day of audio is far more than any ring should hold. The bound keeps the rounding below in
// range.
constexpr double kMaxRingSeconds = 86400.0;

// What the printer counts. It is written on the decoder thread only and read after stop has joined
// that thread.
struct EventCounts {
    std::uint64_t text_events = 0;
    std::uint64_t loss_events = 0;
};

// Writes the whole line and flushes. A failed write is ignored on purpose: a reader that went away
// ends the process through SIGPIPE, and there is nowhere left to report to otherwise.
void write_and_flush(std::FILE* stream, std::string_view line) noexcept {
    assert(stream != nullptr);
    static_cast<void>(std::fwrite(line.data(), 1, line.size(), stream));
    static_cast<void>(std::fflush(stream));
}

void print_event(const lws::TextEvent& event, std::FILE* out, std::FILE* err, EventCounts& counts) {
    write_and_flush(goes_to_stdout(event.kind) ? out : err, format_event(event));
    if (event.kind != lws::EventKind::Text) {
        ++counts.loss_events;
        return;
    }
    ++counts.text_events;
    if (event.after_gap) {
        write_and_flush(
            err, fmt::format("[{} --> {}]  note: audio was lost just before this text\n",
                             format_stream_time(event.t_start), format_stream_time(event.t_end)));
    }
}

// The callback runs on the decoder thread and must not throw: formatting can run out of memory, so
// anything thrown is caught and a fixed line is written instead.
lws::TextCallback make_printer(const RunIo& io, EventCounts& counts) {
    return [out = io.out, err = io.err, &counts](const lws::TextEvent& event) noexcept {
        try {
            print_event(event, out, err, counts);
        } catch (...) {
            static_cast<void>(std::fputs("lws-cli: an event could not be printed\n", err));
        }
    };
}

std::size_t ring_capacity(double seconds) {
    const double samples = std::clamp(seconds, 0.0, kMaxRingSeconds) * lws::kSampleRateHz;
    return static_cast<std::size_t>(std::llround(samples));
}

bool stop_requested(const RunIo& io) noexcept {
    return io.stop_flag != nullptr && *io.stop_flag != 0;
}

enum class ReadOutcome : std::uint8_t { Data, End, Failed, Stopped };

struct ReadResult {
    ReadOutcome outcome = ReadOutcome::End;
    std::size_t bytes = 0;
    int error = 0;
};

// One read of at most buffer.size() bytes. An interrupted read is retried unless the stop flag is
// set, which is how a signal ends the input.
ReadResult read_some(const RunIo& io, std::span<std::byte> buffer) {
    for (;;) {
        if (stop_requested(io)) {
            return {.outcome = ReadOutcome::Stopped};
        }
        const ssize_t count = ::read(io.input_fd, buffer.data(), buffer.size());
        if (count > 0) {
            return {.outcome = ReadOutcome::Data, .bytes = static_cast<std::size_t>(count)};
        }
        if (count == 0) {
            return {.outcome = ReadOutcome::End};
        }
        if (errno != EINTR) {
            return {.outcome = ReadOutcome::Failed, .error = errno};
        }
    }
}

// Where the paced stream stands: when the first push happened and how many samples were offered.
struct PaceState {
    std::optional<std::chrono::steady_clock::time_point> start;
    std::uint64_t offered = 0;
};

// With a speed, waits until the samples offered so far are due. Without one, does not wait.
void pace(const PaceState& state, std::chrono::steady_clock::time_point start, double speed) {
    if (speed > 0.0) {
        std::this_thread::sleep_until(start + pace_due(state.offered, speed));
    }
}

// Offers samples to the streamer, once each. A span the ring refuses is counted and gone: offering
// it again would shift every later time.
std::expected<void, RunFailure> offer(lws::Streamer& streamer, RunReport& report, PaceState& state,
                                      double speed, std::span<const float> samples) {
    if (samples.empty()) {
        return {};
    }
    const auto start = state.start.has_value()
                           ? *state.start
                           : state.start.emplace(std::chrono::steady_clock::now());
    const auto result = streamer.push(samples);
    if (result) {
        report.samples_pushed += samples.size();
    } else if (result.error() == lws::Error::Overrun) {
        report.samples_dropped += samples.size();
        ++report.overruns;
    } else {
        return std::unexpected(RunFailure{.error = result.error(), .stage = RunStage::Push});
    }
    state.offered += samples.size();
    pace(state, start, speed);
    return {};
}

void say(const RunIo& io, std::string_view line) { write_and_flush(io.err, line); }

void report_ending(const RunIo& io, const RunReport& report) {
    if (report.samples_dropped > 0) {
        say(io, fmt::format("lws-cli: {} samples dropped in {} overruns\n", report.samples_dropped,
                            report.overruns));
    }
    if (report.trailing_bytes > 0) {
        say(io, fmt::format("lws-cli: {} trailing bytes dropped: the input is not a whole number "
                            "of f32 samples\n",
                            report.trailing_bytes));
    }
    if (report.replaced_samples > 0) {
        say(io,
            fmt::format("lws-cli: {} non-finite samples replaced by 0\n", report.replaced_samples));
    }
}

std::unexpected<RunFailure> failure(lws::Error error, RunStage stage) {
    return std::unexpected(RunFailure{.error = error, .stage = stage});
}

} // namespace

std::expected<RunReport, RunFailure> run(const RunConfig& config, const RunIo& io,
                                         lws::StopMode at_end) {
    lws::StreamerOptions options;
    options.model_path = config.model_path;
    options.vad_model_path = config.vad_model_path;
    options.decoder.language = config.language;
    options.decoder.n_threads = config.threads;
    options.decoder.use_gpu = config.use_gpu;
    options.ring_capacity_samples = ring_capacity(config.ring_seconds);

    // Declared before the streamer: the decoder thread writes it, and the streamer's destructor
    // joins that thread first.
    EventCounts counts;
    auto streamer = lws::Streamer::create(options);
    if (!streamer) {
        return failure(streamer.error(), RunStage::Create);
    }
    if (const auto set = streamer->on_text(make_printer(io, counts)); !set) {
        return failure(set.error(), RunStage::Callback);
    }
    if (const auto started = streamer->start(); !started) {
        return failure(started.error(), RunStage::Start);
    }

    RunReport report;
    PcmAssembler assembler;
    PaceState pace_state;
    std::array<std::byte, kReadBytes> buffer{};
    bool warned = false;
    for (;;) {
        const ReadResult read = read_some(io, buffer);
        if (read.outcome == ReadOutcome::Failed) {
            report.input_failed = true;
            say(io, fmt::format("lws-cli: reading stdin failed: {}\n",
                                std::error_code(read.error, std::generic_category()).message()));
        }
        if (read.outcome != ReadOutcome::Data) {
            break;
        }
        const std::span<const float> samples =
            assembler.feed(std::span<const std::byte>(buffer).first(read.bytes));
        report.samples_read += samples.size();
        if (!warned && assembler.looks_wrong()) {
            warned = true;
            say(io, "lws-cli: the input does not look like 16 kHz mono f32le (a producer writing "
                    "s16le gives this)\n");
        }
        if (const auto pushed = offer(*streamer, report, pace_state, config.speed, samples);
            !pushed) {
            return std::unexpected(pushed.error());
        }
    }

    report.trailing_bytes = assembler.pending_bytes();
    report.replaced_samples = assembler.replaced_samples();
    if (const auto stopped = streamer->stop(at_end); !stopped) {
        return failure(stopped.error(), RunStage::Stop);
    }
    // stop joined the decoder thread, so the counts are final.
    report.text_events = counts.text_events;
    report.loss_events = counts.loss_events;
    report_ending(io, report);
    return report;
}

} // namespace lws_cli
