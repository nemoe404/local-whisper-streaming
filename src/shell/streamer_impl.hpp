#pragma once

#include <expected>

#include "lws/options.hpp"
#include "lws/streamer.hpp"
#include "lws/streamer_options.hpp"
#include "shell/decoder.hpp"
#include "shell/vad_engine.hpp"

// Shell header. StreamerAccess is the factory behind Streamer::create and the seam tests use to
// inject a fake VAD engine and a fake decoder: it builds a streamer from engines that are already
// open, so no model file is needed.

namespace lws {

struct StreamerAccess {
    // Builds the segmenter from the engine and the options' VAD settings (its error is returned
    // unchanged) and wraps everything in a streamer that has not started.
    [[nodiscard]] static std::expected<Streamer, Error> assemble(VadEngine engine, Decoder decoder,
                                                                 const StreamerOptions& options);
};

} // namespace lws
