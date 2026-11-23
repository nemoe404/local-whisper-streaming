#include <cassert>
#include <cstddef>
#include <expected>
#include <span>
#include <vector>

#include "shell/vad_engine.hpp"

namespace lws {

// whisper.cpp returns ceil(n / window) probabilities for n samples. One probability for 512 samples
// means window >= 512; two for 513 means window <= 512. Only that pair fixes the frame size at 512.
// The 513-sample call is the one call in the tree that is not a whole number of frames: it runs
// once, on zeros, before the session starts, only its probability count is read, and the
// session-start reset clears the state it leaves behind.
std::expected<void, Error> probe_frame_size(VadEngine& engine) {
    assert(engine.detect);

    const std::vector<float> zeros(kVadFrameSamples + 1, 0.0F);
    const std::span<const float> all{zeros};

    // The result span is borrowed, so its size is read before the next call.
    const auto whole_frame = engine.detect(all.first(kVadFrameSamples));
    if (!whole_frame) {
        return std::unexpected(whole_frame.error());
    }
    const std::size_t whole_frame_count = whole_frame->size();

    const auto one_over = engine.detect(all);
    if (!one_over) {
        return std::unexpected(one_over.error());
    }
    const std::size_t one_over_count = one_over->size();

    if (whole_frame_count != 1 || one_over_count != 2) {
        return std::unexpected(Error::FrameSizeMismatch);
    }
    return {};
}

} // namespace lws
