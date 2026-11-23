#include "lws/vad_windower.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace lws {

std::size_t preroll_tail_frames(std::size_t pre_roll_frames, std::size_t onset_frames) noexcept {
    assert(onset_frames >= 1);
    return pre_roll_frames + onset_frames - 1;
}

namespace {

// Appends one frame and its probability to a sample/probability pair of vectors.
void append_frame(std::vector<float>& samples, std::vector<float>& probs,
                  std::span<const float> frame, float prob) {
    samples.insert(samples.end(), frame.begin(), frame.end());
    probs.push_back(prob);
}

// Appends a frame, then drops whole frames from the front until at most cap_frames remain.
void push_tail(std::vector<float>& samples, std::vector<float>& probs, std::span<const float> frame,
               float prob, std::size_t cap_frames) {
    append_frame(samples, probs, frame, prob);
    if (probs.size() <= cap_frames) {
        return;
    }
    const auto excess = static_cast<std::ptrdiff_t>(probs.size() - cap_frames);
    samples.erase(samples.begin(), std::next(samples.begin(), excess * static_cast<std::ptrdiff_t>(
                                                                           kVadFrameSamples)));
    probs.erase(probs.begin(), std::next(probs.begin(), excess));
}

} // namespace

VadWindower::VadWindower(std::size_t onset_frames, std::size_t hangover_frames,
                         std::size_t pre_roll_frames, float threshold) noexcept
    : threshold_(threshold), onset_frames_(onset_frames), hangover_frames_(hangover_frames),
      tail_cap_frames_(preroll_tail_frames(pre_roll_frames, onset_frames)) {}

std::expected<VadWindower, Error> VadWindower::create(const Options& options) {
    const auto valid = validate(options);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    // A required duration clamps to one frame (D-15); the pre-roll may be zero.
    const auto onset = static_cast<std::size_t>(
        std::max<std::uint64_t>(1U, ms_to_frames_ceil(options.min_speech_duration_ms)));
    const auto hangover = static_cast<std::size_t>(
        std::max<std::uint64_t>(1U, ms_to_frames_ceil(options.min_silence_duration_ms)));
    const auto pre = static_cast<std::size_t>(ms_to_frames_ceil(options.pre_roll_ms));
    return VadWindower(onset, hangover, pre, options.threshold);
}

std::optional<Segment> VadWindower::feed(std::span<const float> frame, float prob) {
    assert(frame.size() == kVadFrameSamples);
    assert(prob >= 0.0F && prob <= 1.0F);

    const std::uint64_t frame_start = fed_samples_;
    fed_samples_ += kVadFrameSamples;
    const bool speech = prob >= threshold_; // ordered: false for NaN
    speech_streak_ = speech ? speech_streak_ + 1 : 0;

    if (open_) {
        if (speech) {
            append_frame(window_samples_, window_probs_, frame, prob);
            silence_frames_ = 0;
            return std::nullopt;
        }
        if (silence_frames_ + 1 < hangover_frames_) {
            append_frame(window_samples_, window_probs_, frame, prob);
            ++silence_frames_;
            return std::nullopt;
        }
        // The frame that reaches the hangover count closes the window and is not part of it. It
        // seeds the next pre-roll tail, which a hangover close leaves alone.
        Segment closed{.samples = std::move(window_samples_),
                       .start = window_start_,
                       .reason = CloseReason::Hangover,
                       .continues_previous = current_continues_previous_,
                       .discontinuous_before = current_discontinuous_before_};
        window_samples_.clear();
        window_probs_.clear();
        open_ = false;
        silence_frames_ = 0;
        push_tail(tail_samples_, tail_probs_, frame, prob, tail_cap_frames_);
        return closed;
    }

    if (speech && speech_streak_ >= onset_frames_) {
        const std::uint64_t tail_samples_count =
            static_cast<std::uint64_t>(tail_probs_.size()) * kVadFrameSamples;
        assert(frame_start >= tail_samples_count);
        window_start_ = frame_start - tail_samples_count;
        window_samples_ = std::move(tail_samples_);
        window_probs_ = std::move(tail_probs_);
        tail_samples_.clear();
        tail_probs_.clear();
        append_frame(window_samples_, window_probs_, frame, prob);
        silence_frames_ = 0;
        current_continues_previous_ = false;
        current_discontinuous_before_ = false;
        open_ = true;
        return std::nullopt;
    }

    push_tail(tail_samples_, tail_probs_, frame, prob, tail_cap_frames_);
    return std::nullopt;
}

} // namespace lws
