#pragma once

#include "lws/options.hpp"
#include "lws/streamer.hpp"

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <string>

// The command-line tool's run loop: it reads raw 16 kHz mono f32le samples from a file descriptor,
// pushes them into a Streamer, prints the events, and ends the stream when the input ends. It is
// the shell of the tool: descriptors, stdio, the clock and the Streamer's threads live here, and
// the pure value modules (the byte carry, the line format, the pacing schedule) carry the
// decisions.
namespace lws_cli {

struct RunConfig {
    std::filesystem::path model_path;
    std::filesystem::path vad_model_path;
    std::string language = "en";
    std::int32_t threads = 4;
    bool use_gpu = true;
    // How many seconds of audio the ring between the reader and the pump holds. A push that does
    // not fit is dropped and reported, never retried.
    double ring_seconds = 60.0;
    // Push at this multiple of real time. 0 pushes as fast as the input arrives.
    double speed = 0.0;
    // Print one line of measurements on stderr after the stream ended.
    bool stats = false;
    // How many finished segments wait for the decoder before the oldest is discarded.
    std::size_t queue_segments = 8;
};

struct RunIo {
    int input_fd = 0;
    // Transcript lines go here, one flush per event.
    std::FILE* out = nullptr;
    // Loss events, notes, counts and warnings go here.
    std::FILE* err = nullptr;
    // When it turns non-zero the loop stops reading and ends the stream as usual. A signal handler
    // sets it. May be null.
    const volatile std::sig_atomic_t* stop_flag = nullptr;
};

struct RunReport {
    // Whole samples taken from the input, trailing bytes of an unfinished float excluded.
    std::uint64_t samples_read = 0;
    // Samples the ring accepted.
    std::uint64_t samples_pushed = 0;
    // Samples the ring refused, over `overruns` pushes. They are never offered again.
    std::uint64_t samples_dropped = 0;
    std::uint64_t overruns = 0;
    // Bytes left over at the end of input: 0 to 3.
    std::uint64_t trailing_bytes = 0;
    // Non-finite samples that were replaced by 0.
    std::uint64_t replaced_samples = 0;
    std::uint64_t text_events = 0;
    // Overrun, voice detector, queue and decoder events: everything that is not text.
    std::uint64_t loss_events = 0;
    // True when reading the input failed. The stream was still ended as asked.
    bool input_failed = false;
};

// The call that failed and the library's error for it.
enum class RunStage : std::uint8_t { Create, Callback, Start, Push, Stop };

struct RunFailure {
    lws::Error error;
    RunStage stage;
};

// Runs one stream from start to stop. `at_end` is how the stream ends when the input ends or the
// stop flag is set: Drain decodes the open utterance first, Immediate gives it up.
[[nodiscard]] std::expected<RunReport, RunFailure> run(const RunConfig& config, const RunIo& io,
                                                       lws::StopMode at_end);

} // namespace lws_cli
