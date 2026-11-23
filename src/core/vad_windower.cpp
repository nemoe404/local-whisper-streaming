#include "lws/vad_windower.hpp"
#include "lws/segment.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
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

// The cut looks for the lowest probability in the last 1.5 s of the window (CORE-05).
const std::size_t kCutSearchFrames = static_cast<std::size_t>(ms_to_frames_ceil(1500));

// The cut history is a strict ordering: a NaN is stored as 0.0F (D11b). The frame is still
// sub-threshold, because the threshold test reads the original probability.
float history_value(float prob) noexcept { return std::isnan(prob) ? 0.0F : prob; }

// Appends one frame and its probability to a sample/probability pair of vectors.
void append_frame(std::vector<float>& samples, std::vector<float>& probs,
                  std::span<const float> frame, float prob) {
    samples.insert(samples.end(), frame.begin(), frame.end());
    probs.push_back(history_value(prob));
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

// Index of the first minimum-probability frame among the last kCutSearchFrames, clamped to
// [1, n - 1] so that both the emitted part and the remainder hold a frame.
std::size_t find_cut_frame(std::span<const float> probs) {
    const std::size_t n = probs.size();
    assert(n >= 2);
    const std::size_t first = n > kCutSearchFrames ? n - kCutSearchFrames : 0;
    const auto searched = probs.subspan(first);
    const auto offset = static_cast<std::size_t>(
        std::ranges::distance(searched.begin(), std::ranges::min_element(searched)));
    return std::clamp(first + offset, std::size_t{1}, n - 1);
}

} // namespace

VadWindower::VadWindower(std::size_t onset_frames, std::size_t hangover_frames,
                         std::size_t pre_roll_frames, std::size_t ceiling_frames,
                         std::size_t overlap_frames, float threshold) noexcept
    : threshold_(threshold), onset_frames_(onset_frames), hangover_frames_(hangover_frames),
      tail_cap_frames_(preroll_tail_frames(pre_roll_frames, onset_frames)),
      ceiling_frames_(ceiling_frames), overlap_frames_(overlap_frames) {}

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
    // D3: the ceiling rounds up, so 14 s gives 438 frames. validate() has bounded the input.
    const auto ceiling = static_cast<std::size_t>(
        std::ceil(static_cast<double>(options.max_speech_duration_s) * 1000.0 / 32.0));
    const auto overlap = static_cast<std::size_t>(ms_to_frames_ceil(options.overlap_ms));
    return VadWindower(onset, hangover, pre, ceiling, overlap, options.threshold);
}

Segment VadWindower::cut_at_minimum() {
    const std::size_t cut = find_cut_frame(window_probs_);
    // D-07, user decision: at least one frame leaves the window, so a cut at or below the overlap
    // cannot leave the remainder equal to the whole window.
    const std::size_t overlap = std::min(overlap_frames_, cut - 1);
    const std::size_t removed = cut - overlap;
    const auto cut_samples = static_cast<std::ptrdiff_t>(cut * kVadFrameSamples);
    const auto removed_samples = static_cast<std::ptrdiff_t>(removed * kVadFrameSamples);

    Segment emitted{.samples = std::vector<float>(window_samples_.begin(),
                                                  std::next(window_samples_.begin(), cut_samples)),
                    .start = window_start_,
                    .reason = CloseReason::Ceiling,
                    .continues_previous = current_continues_previous_,
                    .discontinuous_before = current_discontinuous_before_};

    window_samples_.erase(window_samples_.begin(),
                          std::next(window_samples_.begin(), removed_samples));
    window_probs_.erase(window_probs_.begin(),
                        std::next(window_probs_.begin(), static_cast<std::ptrdiff_t>(removed)));
    window_start_ += static_cast<std::uint64_t>(removed) * kVadFrameSamples;
    current_continues_previous_ = true;
    current_discontinuous_before_ = false;
    return emitted;
}

Segment VadWindower::take_window(CloseReason reason) {
    assert(open_);
    Segment segment{.samples = std::move(window_samples_),
                    .start = window_start_,
                    .reason = reason,
                    .continues_previous = current_continues_previous_,
                    .discontinuous_before = current_discontinuous_before_};
    window_samples_.clear();
    window_probs_.clear();
    open_ = false;
    silence_frames_ = 0;
    current_continues_previous_ = false;
    current_discontinuous_before_ = false;
    return segment;
}

void VadWindower::reset_automaton() noexcept {
    speech_streak_ = 0;
    tail_samples_.clear();
    tail_probs_.clear();
}

std::optional<Segment> VadWindower::force_close() {
    std::optional<Segment> closed;
    if (open_) {
        closed = take_window(CloseReason::Discontinuity);
    }
    reset_automaton();
    return closed;
}

std::optional<Segment> VadWindower::flush() {
    std::optional<Segment> closed;
    if (open_) {
        closed = take_window(CloseReason::Flush);
    }
    reset_automaton();
    return closed;
}

std::optional<Segment> VadWindower::notify_discontinuity() {
    std::optional<Segment> closed;
    if (open_) {
        closed = take_window(CloseReason::Discontinuity);
    }
    reset_automaton();
    pending_discontinuity_ = true;
    assert(!(pending_discontinuity_ && open_));
    return closed;
}

std::optional<Segment> VadWindower::cut_if_full() {
    if (window_probs_.size() < ceiling_frames_) {
        return std::nullopt;
    }
    return cut_at_minimum();
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
            return cut_if_full();
        }
        if (silence_frames_ + 1 < hangover_frames_) {
            append_frame(window_samples_, window_probs_, frame, prob);
            ++silence_frames_;
            return cut_if_full();
        }
        // The frame that reaches the hangover count closes the window and is not part of it. It
        // seeds the next pre-roll tail, which a hangover close leaves alone.
        Segment closed = take_window(CloseReason::Hangover);
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
        current_discontinuous_before_ = std::exchange(pending_discontinuity_, false);
        open_ = true;
        return std::nullopt;
    }

    push_tail(tail_samples_, tail_probs_, frame, prob, tail_cap_frames_);
    return std::nullopt;
}

} // namespace lws
