#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <vector>

#include "lws/options.hpp"
#include "shell/vad_engine.hpp"

// Shell header: the reader between arbitrary-sized sample spans and the VAD engine. It includes no
// whisper header.

namespace lws {

// Whole frames scored by one or more detect calls, in stream order.
struct ScoredFrames {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes): a value type whose two vectors
    // are the result; callers read and move them, and size() and frame() only view them.
    std::vector<float> samples; // probs.size() * kVadFrameSamples samples
    std::vector<float> probs;   // one probability per frame
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] std::size_t size() const noexcept;
    // The index-th frame's kVadFrameSamples samples. Asserts index < size().
    [[nodiscard]] std::span<const float> frame(std::size_t index) const noexcept;
};

// Owns the injected engine and hands it exact kVadFrameSamples frames only. The state the model
// carries between calls is reset in three places and nowhere else: at session start (create), when
// the ring reports an overrun, and when the source relinks.
class ChunkReader {
public:
    // The VAD startup: probe_frame_size, then one reset. Error::FrameSizeMismatch if the engine's
    // frame size is not kVadFrameSamples; a detect error from the probe is returned unchanged.
    // Asserts both engine members are non-empty.
    [[nodiscard]] static std::expected<ChunkReader, Error> create(VadEngine engine);

    // Appends behind the buffered partial frame and scores every whole frame now available in one
    // detect call. A partial frame stays buffered and is never padded. Error::VadFailed on a detect
    // error or a probability count that disagrees with the frame count; that call's whole frames
    // are dropped and the partial frame is kept. Never resets.
    [[nodiscard]] std::expected<ScoredFrames, Error> feed(std::span<const float> samples);

    // Buffered samples; always fewer than kVadFrameSamples between calls.
    [[nodiscard]] std::size_t pending() const noexcept;

private:
    explicit ChunkReader(VadEngine engine) noexcept;
    void reset_state(); // the one caller of the engine's reset; drops the partial frame

    VadEngine engine_;
    std::vector<float> pending_;
};

} // namespace lws
