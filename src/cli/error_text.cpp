#include "cli/error_text.hpp"

#include <utility>

namespace lws_cli {

namespace {

constexpr int kRuntimeFailure = 1;
constexpr int kUsageFailure = 2;

} // namespace

// No default: -Wswitch names the enumerator that a later change adds without a message here.
ErrorText error_text(lws::Error error) noexcept {
    switch (error) {
    case lws::Error::InvalidThreshold:
        return {.message = "the voice detector threshold is out of range",
                .exit_code = kUsageFailure};
    case lws::Error::InvalidDuration:
        return {.message = "a voice detector duration is out of range", .exit_code = kUsageFailure};
    case lws::Error::CeilingTooLong:
        return {.message = "the longest speech segment is above what the decoder accepts",
                .exit_code = kUsageFailure};
    case lws::Error::CeilingTooShort:
        return {.message = "the longest speech segment is shorter than the minimum speech duration",
                .exit_code = kUsageFailure};
    case lws::Error::Overrun:
        return {.message = "the audio ring buffer was full", .exit_code = kRuntimeFailure};
    case lws::Error::VadFailed:
        return {.message = "the voice detector failed", .exit_code = kRuntimeFailure};
    case lws::Error::ModelLoad:
        return {.message = "cannot load the model", .exit_code = kRuntimeFailure};
    case lws::Error::FrameSizeMismatch:
        return {.message = "the voice detector model is not a 512-sample Silero model",
                .exit_code = kRuntimeFailure};
    case lws::Error::DecodeFailed:
        return {.message = "decoding failed", .exit_code = kRuntimeFailure};
    case lws::Error::InvalidDecoderOptions:
        return {.message = "invalid decoder options: check --threads (at least 1) and --language "
                           "(not empty)",
                .exit_code = kUsageFailure};
    case lws::Error::NotRunning:
        return {.message = "internal error: the streamer is not running",
                .exit_code = kRuntimeFailure};
    case lws::Error::AlreadyStarted:
        return {.message = "internal error: the streamer was already started",
                .exit_code = kRuntimeFailure};
    case lws::Error::InvalidStreamerOptions:
        return {.message = "invalid streamer options: --ring-seconds is too small",
                .exit_code = kUsageFailure};
    }
    std::unreachable();
}

} // namespace lws_cli
