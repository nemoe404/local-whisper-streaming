#pragma once

#include "lws/options.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <vector>

// Public core header. No whisper, ggml, spdlog or thread type crosses it. It holds the decisions
// the decoder takes before and after one whisper call: the shortest decodable segment, the time
// base of the returned text, what a return code means, and which options are acceptable.
namespace lws {

// Whisper rejects audio shorter than 100 ms, so such a segment is never handed to it.
inline constexpr std::size_t kMinDecodeSamples = kSampleRateHz / 10U;
// Whisper reports times in units of 10 ms.
inline constexpr std::uint32_t kSamplesPerCentisecond = kSampleRateHz / 100U;
static_assert(kSampleRateHz % 100U == 0, "whisper times are in 10 ms units");

// What a successful call produced. TooShort and Cancelled carry no text; only a real failure is
// an error.
enum class DecodeStatus : std::uint8_t { Decoded, TooShort, Cancelled };

// The meaning of whisper's return code once the stop flag is known.
enum class RunOutcome : std::uint8_t { Ok, Cancelled, Failed };

// One piece of text. begin and end count samples from the segment's first sample.
struct TextSpan {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::string text; // trimmed, never empty
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::Decoded;
    std::vector<TextSpan> spans; // empty unless status is Decoded
};

// What the shell reads from whisper: times in 10 ms units from the first sample passed in.
struct RawSpan {
    std::int64_t t0_cs = 0;
    std::int64_t t1_cs = 0;
    std::string text;
};

// The values are member initializers on purpose, never constexpr constants. temperature_inc is
// whisper's greedy default: it lets whisper retry a window at a higher temperature inside the one
// call.
struct DecoderOptions {
    std::int32_t n_threads = 4;
    std::string language = "en";
    float temperature_inc = 0.2F;
    bool use_gpu = true;
};

// Returns the options unchanged when they hold, or Error::InvalidDecoderOptions.
[[nodiscard]] std::expected<DecoderOptions, Error> validate(const DecoderOptions& options);

[[nodiscard]] constexpr bool is_decodable(std::size_t n_samples) noexcept {
    return n_samples >= kMinDecodeSamples;
}

// A negative time gives 0; a time too large for 64-bit samples gives the largest value.
[[nodiscard]] constexpr std::uint64_t cs_to_samples(std::int64_t centiseconds) noexcept {
    if (centiseconds <= 0) {
        return 0;
    }
    const auto cs = static_cast<std::uint64_t>(centiseconds);
    constexpr std::uint64_t largest_exact =
        std::numeric_limits<std::uint64_t>::max() / kSamplesPerCentisecond;
    return cs > largest_exact ? std::numeric_limits<std::uint64_t>::max()
                              : cs * kSamplesPerCentisecond;
}

// The stop flag decides, not the code: whisper reports an abort with the same return codes as a
// real encode or decode failure.
[[nodiscard]] constexpr RunOutcome classify_run(std::int32_t rc, bool stop_requested) noexcept {
    if (rc == 0) {
        return RunOutcome::Ok;
    }
    return stop_requested ? RunOutcome::Cancelled : RunOutcome::Failed;
}

// Clamps every span into [0, n_samples], trims ASCII whitespace from the text, drops spans whose
// text is then empty, and keeps the input order.
[[nodiscard]] std::vector<TextSpan> map_spans(std::span<const RawSpan> raw, std::size_t n_samples);

} // namespace lws
