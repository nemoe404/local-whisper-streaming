#include "cli/run.hpp"

#include "cli/event_format.hpp"
#include "cli/pace.hpp"
#include "cli/pcm_assembler.hpp"
#include "cli/stats.hpp"
#include "lws/streamer_options.hpp"
#include "lws/text_event.hpp"

#include <fmt/format.h>
#include <sys/resource.h>
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
#include <vector>

namespace lws_cli {

namespace {

constexpr std::size_t kReadBytes = 16384;
// A day of audio is far more than any ring should hold. The bound keeps the rounding below in
// range.
constexpr double kMaxRingSeconds = 86400.0;

// What the printer counts, and when each text event arrived when the stats line asks for it. It is
// written on the decoder thread only and read after stop has joined that thread.
struct EventCounts {
    std::uint64_t text_events = 0;
    std::uint64_t loss_events = 0;
    bool record_deliveries = false;
    std::vector<Delivery> deliveries;
};

// Writes the whole line and flushes. A failed write is ignored on purpose: a reader that went away
// ends the process through SIGPIPE, and there is nowhere left to report to otherwise.
void write_and_flush(std::FILE* stream, std::string_view line) noexcept {
    assert(stream != nullptr);
    static_cast<void>(std::fwrite(line.data(), 1, line.size(), stream));
    static_cast<void>(std::fflush(stream));
}

void print_event(const lws::TextEvent& event, Clock::time_point arrived, std::FILE* out,
                 std::FILE* err, EventCounts& counts) {
    write_and_flush(goes_to_stdout(event.kind) ? out : err, format_event(event));
    if (event.kind != lws::EventKind::Text) {
        ++counts.loss_events;
        return;
    }
    ++counts.text_events;
    if (counts.record_deliveries) {
        counts.deliveries.push_back(Delivery{.t_end = event.t_end, .at = arrived});
    }
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
        // Stamped before anything is written, so the printing is not part of the lag.
        const Clock::time_point arrived = Clock::now();
        try {
            print_event(event, arrived, out, err, counts);
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
// With `record_marks`, each push is also noted in `marks` for the stats line.
struct PaceState {
    std::optional<std::chrono::steady_clock::time_point> start;
    std::uint64_t offered = 0;
    bool record_marks = false;
    std::vector<PushMark> marks;
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
    if (state.record_marks) {
        state.marks.push_back(PushMark{.samples_after = state.offered, .at = Clock::now()});
    }
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

// Peak resident memory and the CPU time of this process so far.
struct ResourceUse {
    double peak_rss_mib = 0.0;
    double user_s = 0.0;
    double system_s = 0.0;
};

double seconds_of(const timeval& time) {
    constexpr double kMicrosPerSecond = 1.0e6;
    return static_cast<double>(time.tv_sec) +
           (static_cast<double>(time.tv_usec) / kMicrosPerSecond);
}

ResourceUse resource_use() {
    constexpr double kKibPerMib = 1024.0;
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return {};
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): ru_maxrss is in a C union
    const auto peak_kib = static_cast<double>(usage.ru_maxrss);
    return {.peak_rss_mib = peak_kib / kKibPerMib,
            .user_s = seconds_of(usage.ru_utime),
            .system_s = seconds_of(usage.ru_stime)};
}

// The times the stats line needs from the run.
struct RunTimes {
    Clock::time_point process_start;
    Clock::time_point started;
    Clock::time_point stopped;
};

StatsLine make_stats(const RunReport& report, const RunTimes& times, const PaceState& pace_state,
                     const EventCounts& counts) {
    const ResourceUse use = resource_use();
    const double audio_s =
        static_cast<double>(report.samples_read) / static_cast<double>(lws::kSampleRateHz);
    const double wall_s = std::chrono::duration<double>(times.stopped - times.started).count();
    return StatsLine{
        .audio_s = audio_s,
        .wall_s = wall_s,
        .speed_x = wall_s > 0.0 ? audio_s / wall_s : 0.0,
        .load_ms =
            std::chrono::duration<double, std::milli>(times.started - times.process_start).count(),
        .text_events = counts.text_events,
        .lag = lag_summary(pace_state.marks, counts.deliveries),
        .peak_rss_mib = use.peak_rss_mib,
        .cpu_user_s = use.user_s,
        .cpu_sys_s = use.system_s};
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
    options.queue_segments = config.queue_segments;

    const Clock::time_point run_begin = Clock::now();
    // Declared before the streamer: the decoder thread writes it, and the streamer's destructor
    // joins that thread first.
    EventCounts counts;
    counts.record_deliveries = config.stats;
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

    const Clock::time_point started = Clock::now();
    RunReport report;
    PcmAssembler assembler;
    PaceState pace_state;
    pace_state.record_marks = config.stats;
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
    const Clock::time_point stopped = Clock::now();
    // stop joined the decoder thread, so the counts are final.
    report.text_events = counts.text_events;
    report.loss_events = counts.loss_events;
    report_ending(io, report);
    if (config.stats) {
        const bool process_start_known = io.process_start != Clock::time_point::min();
        const RunTimes times{.process_start = process_start_known ? io.process_start : run_begin,
                             .started = started,
                             .stopped = stopped};
        say(io, fmt::format("{}\n", format_stats(make_stats(report, times, pace_state, counts))));
    }
    return report;
}

} // namespace lws_cli
