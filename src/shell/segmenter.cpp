#include "shell/segmenter.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "lws/vad_windower.hpp"
#include "shell/chunk_reader.hpp"
#include "shell/vad_engine.hpp"

// Failure contract: when detection fails the caller closes the open window and returns it inside
// the error. A failed call has scored nothing, so no frame of it reaches the windower, and no
// probability is made up to stand in for the missing one.

namespace lws {

Segmenter::Segmenter(ChunkReader reader, VadWindower windower) noexcept
    : reader_{std::move(reader)}, windower_{std::move(windower)} {}

std::expected<Segmenter, Error> Segmenter::create(VadEngine engine, const Options& options) {
    auto windower = VadWindower::create(options);
    if (!windower) {
        return std::unexpected(windower.error());
    }
    auto reader = ChunkReader::create(std::move(engine));
    if (!reader) {
        return std::unexpected(reader.error());
    }
    return Segmenter{std::move(*reader), std::move(*windower)};
}

std::expected<std::vector<Segment>, VadFailure> Segmenter::push(std::span<const float> samples) {
    const auto frames = reader_.feed(samples);
    if (!frames) {
        return std::unexpected(
            VadFailure{.error = frames.error(), .force_closed = windower_.force_close()});
    }

    std::vector<Segment> closed;
    std::ranges::for_each(std::views::iota(std::size_t{0}, frames->size()), [&](std::size_t i) {
        if (auto segment = windower_.feed(frames->frame(i), frames->probs[i])) {
            closed.push_back(std::move(*segment));
        }
    });
    return closed;
}

std::optional<Segment> Segmenter::notify_overrun() {
    reader_.notify_overrun();
    return windower_.notify_discontinuity();
}

std::optional<Segment> Segmenter::notify_relink() {
    reader_.notify_relink();
    return windower_.notify_discontinuity();
}

std::optional<Segment> Segmenter::flush() { return windower_.flush(); }

} // namespace lws
