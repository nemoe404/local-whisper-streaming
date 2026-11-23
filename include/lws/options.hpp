#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

// Public core header. No whisper, spdlog or thread type crosses it.
namespace lws {

// One flat error channel for core validation and, later, shell calls (D-17). Trivially copyable.
// Overrun, VadFailed and ModelLoad are declared now and unused until the shell phases.
enum class Error : std::uint8_t {
    InvalidThreshold,
    InvalidDuration,
    CeilingTooLong,
    CeilingTooShort,
    Overrun,
    VadFailed,
    ModelLoad
};

// The single owner of the VAD frame size (D-16): 512 samples at 16 kHz is 32 ms.
inline constexpr std::size_t kVadFrameSamples = 512;
inline constexpr std::uint32_t kSampleRateHz = 16000;
static_assert(kVadFrameSamples * 1000U % kSampleRateHz == 0,
              "a VAD frame must be a whole number of ms");

// Starting values inherited from consigliere's sink-preset sweep (Silero v5.1.2). They are member
// initializers, not constexpr constants (D-14), and are to be re-validated on lws fixtures before
// release. Whisper's own VAD defaults are rejected (D-12).
struct Options {
    float threshold = 0.5F;
    std::uint32_t min_speech_duration_ms = 250;
    std::uint32_t min_silence_duration_ms = 700;
    std::uint32_t pre_roll_ms = 300;
    float max_speech_duration_s = 14.0F;
    std::uint32_t overlap_ms = 250;
};

// Returns the options unchanged when they hold, or the first violated rule (CORE-10).
[[nodiscard]] std::expected<Options, Error> validate(const Options& options);

// Overlap and seam merge go together (Q2): an overlap means the next window repeats words that
// seam_merge removes, and no overlap means there is nothing to merge. Not constexpr on purpose.
[[nodiscard]] bool seam_merge_enabled(const Options& options) noexcept;

// ceil(ms / 32) in 64-bit (D-15). A required duration clamps to at least one frame at its point of
// use.
[[nodiscard]] constexpr std::uint64_t ms_to_frames_ceil(std::uint64_t ms) noexcept {
    return (ms + 31U) / 32U;
}

} // namespace lws
