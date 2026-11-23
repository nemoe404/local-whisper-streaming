#pragma once

#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "lws/vad_windower.hpp"
#include "shell/chunk_reader.hpp"
#include "shell/vad_engine.hpp"

// Shell header: the reader plus the windower, and the one place that decides what a failed
// detection means. It includes no whisper header.

namespace lws {

// What a failed push hands back: the error and the window that was open when detection failed.
struct VadFailure {
    Error error = Error::VadFailed;
    // The open window, closed as is (reason Discontinuity), so a failure never drops speech that
    // was already collected. Empty when no window was open.
    std::optional<Segment> force_closed;
};

class Segmenter {
public:
    // Validates the options before the engine is touched, then starts the VAD session through
    // ChunkReader::create (probe, one reset). Either error is returned unchanged.
    [[nodiscard]] static std::expected<Segmenter, Error> create(VadEngine engine,
                                                                const Options& options);

    // Scores every whole frame now available and feeds each, in order, to the windower; returns
    // the segments they closed. On Error::VadFailed the open window is force-closed and returned
    // inside the error. No frame of a failed call reaches the windower and no probability is made
    // up for it.
    [[nodiscard]] std::expected<std::vector<Segment>, VadFailure>
    push(std::span<const float> samples);

    // The ring reported an overrun: the reader's reset point, then the windower closes its open
    // window as a discontinuity and marks the next one.
    [[nodiscard]] std::optional<Segment> notify_overrun();
    // The source relinked: same effect as an overrun.
    [[nodiscard]] std::optional<Segment> notify_relink();

    // End of stream: emits the open window. No reset, and the buffered partial frame is not
    // scored.
    [[nodiscard]] std::optional<Segment> flush();

private:
    Segmenter(ChunkReader reader, VadWindower windower) noexcept;

    ChunkReader reader_;
    VadWindower windower_;
};

} // namespace lws
