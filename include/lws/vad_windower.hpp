#pragma once

#include "lws/options.hpp"
#include "lws/segment.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

// Public core header. Pure frame-and-probability automaton: no clock, no I/O, no whisper or thread
// type crosses it.
namespace lws {

// Frames of look-back the tail must hold so that, after the onset run is spent, a full pre-roll of
// leading frames is still in front of the first speech frame (D1, CORE-09).
// Not constexpr on purpose: the argument names are tuning identifiers.
[[nodiscard]] std::size_t preroll_tail_frames(std::size_t pre_roll_frames,
                                              std::size_t onset_frames) noexcept;

class VadWindower {
public:
    // Validates the options, then converts milliseconds to frame counts.
    [[nodiscard]] static std::expected<VadWindower, Error> create(const Options& options);

    // One 512-sample frame and its speech probability in [0, 1]. Returns the segment this frame
    // closed, if any. NaN is sub-threshold (ordered comparison).
    [[nodiscard]] std::optional<Segment> feed(std::span<const float> frame, float prob);

private:
    VadWindower(std::size_t onset_frames, std::size_t hangover_frames, std::size_t pre_roll_frames,
                std::size_t ceiling_frames, std::size_t overlap_frames, float threshold) noexcept;

    // Emits the first part of an over-long window, cut at the lowest-probability frame of its last
    // 1.5 s, and keeps the rest (with the overlap) as the still-open window (CORE-05, CORE-06).
    [[nodiscard]] Segment cut_at_minimum();

    // Cuts when the open window has reached the ceiling (T-02-04); otherwise returns nothing.
    [[nodiscard]] std::optional<Segment> cut_if_full();

    float threshold_;
    std::size_t onset_frames_;
    std::size_t hangover_frames_;
    std::size_t tail_cap_frames_; // preroll_tail_frames(pre_roll_frames, onset_frames)
    std::size_t ceiling_frames_;  // ceil(max_speech_duration_s * 1000 / 32) (D3)
    std::size_t overlap_frames_;  // ms_to_frames_ceil(overlap_ms) (D2)
    std::uint64_t fed_samples_ = 0;
    std::uint64_t window_start_ = 0;
    std::size_t speech_streak_ = 0;
    std::size_t silence_frames_ = 0;
    bool open_ = false;
    bool current_continues_previous_ = false;   // latched at birth (D-02)
    bool current_discontinuous_before_ = false; // latched at birth (D-03)
    std::vector<float> tail_samples_;
    std::vector<float> tail_probs_;
    std::vector<float> window_samples_;
    std::vector<float> window_probs_;
};

} // namespace lws
