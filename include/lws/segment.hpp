#pragma once

#include <cstdint>
#include <vector>

// Public core header. No whisper, spdlog or thread type crosses it.
namespace lws {

// Why a window was emitted (D-04). Hangover is the only reason that means "the speaker stopped".
enum class CloseReason : std::uint8_t { Hangover, Ceiling, Flush, Discontinuity };

// One utterance window, ready for the decoder. Owns its samples (D-01).
struct Segment {
    std::vector<float> samples; // whole 512-sample frames
    std::uint64_t start = 0;    // absolute sample index of samples[0]
    CloseReason reason = CloseReason::Hangover;
    bool continues_previous = false; // this segment starts inside the previous one's speech (D-02)
    bool discontinuous_before = false; // audio was lost just before this segment (D-03)
};

} // namespace lws
