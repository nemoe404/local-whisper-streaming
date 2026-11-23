#include "shell/chunk_reader.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <expected>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

#include "lws/options.hpp"
#include "shell/vad_engine.hpp"

// Reset policy, as the decided list: session start, ring overrun, source relink. Never on a
// hangover close or a ceiling seam, which this type cannot see. Detection only ever sees whole
// frames: a partial frame waits in the pending buffer and is never padded. The one exception is the
// startup probe inside create, whose 513-sample call runs once on zeros before the session; the
// session-start reset clears the state it leaves.

namespace lws {

std::size_t ScoredFrames::size() const noexcept { return probs.size(); }

std::span<const float> ScoredFrames::frame(std::size_t index) const noexcept {
    assert(index < size());
    return std::span<const float>{samples}.subspan(index * kVadFrameSamples, kVadFrameSamples);
}

ChunkReader::ChunkReader(VadEngine engine) noexcept : engine_{std::move(engine)} {}

std::expected<ChunkReader, Error> ChunkReader::create(VadEngine engine) {
    assert(engine.detect && engine.reset);

    if (const auto probed = probe_frame_size(engine); !probed) {
        return std::unexpected(probed.error());
    }
    ChunkReader reader{std::move(engine)};
    reader.reset_state();
    return reader;
}

std::expected<ScoredFrames, Error> ChunkReader::feed(std::span<const float> samples) {
    pending_.insert(pending_.end(), samples.begin(), samples.end());

    const std::size_t frames = pending_.size() / kVadFrameSamples;
    const std::size_t whole = frames * kVadFrameSamples;
    if (whole == 0) {
        return ScoredFrames{};
    }

    const auto whole_end = pending_.begin() + static_cast<std::ptrdiff_t>(whole);
    const auto scored = engine_.detect(std::span<const float>{pending_}.first(whole));
    if (!scored || scored->size() != frames) {
        pending_.erase(pending_.begin(), whole_end);
        return std::unexpected(Error::VadFailed);
    }

    ScoredFrames out;
    out.probs.reserve(frames);
    out.samples.reserve(whole);
    std::ranges::copy(*scored, std::back_inserter(out.probs));
    std::ranges::copy(pending_.begin(), whole_end, std::back_inserter(out.samples));
    pending_.erase(pending_.begin(), whole_end);
    return out;
}

void ChunkReader::notify_overrun() { reset_state(); }

void ChunkReader::notify_relink() { reset_state(); }

std::size_t ChunkReader::pending() const noexcept { return pending_.size(); }

void ChunkReader::reset_state() {
    engine_.reset();
    pending_.clear();
}

} // namespace lws
