#pragma once

#include "segment_fifo.hpp"

#include "lws/segment.hpp"

#include <cassert>
#include <cstdint>

// Internal core header. The clock that puts every segment on the offered stream, as pure values.
//
// The windower numbers the frames it is fed from zero and knows nothing of what happened before
// them: audio the ring dropped, whole frames of a failed detect call and the partial frame an
// overrun discards never reach it. The next frame the windower will see sits at
// `popped + skew - pending` in the offered stream and at `fed` in windower time, where `popped` is
// what the pump has taken from the ring, `skew` the dropped samples attributed to a gap so far,
// `pending` the reader's buffered partial frame and `fed` what the windower has been fed. The
// difference is the offset that stamps every segment. It does not depend on how the same samples
// were split into pops, which is why the events of a stream do not depend on chunking.
namespace lws {

struct StreamPosition {
    std::uint64_t popped = 0;  // samples the pump has taken from the ring and fed to the Segmenter
    std::uint64_t skew = 0;    // dropped samples attributed to a gap so far
    std::uint64_t pending = 0; // the reader's partial frame
    std::uint64_t fed = 0;     // samples the windower has been fed
};

// Where the next frame the windower will see sits in the offered stream.
[[nodiscard]] constexpr std::uint64_t next_frame_position(const StreamPosition& p) noexcept {
    assert(p.popped + p.skew >= p.pending);
    return p.popped + p.skew - p.pending;
}

// What to add to a windower time to get an offered-stream time.
[[nodiscard]] constexpr std::uint64_t stream_offset(const StreamPosition& p) noexcept {
    assert(p.popped + p.skew >= p.pending + p.fed);
    return p.popped + p.skew - p.pending - p.fed;
}

// The segment with its start moved by the offset; every other field is kept.
[[nodiscard]] Segment stamped(Segment segment, std::uint64_t offset) noexcept;

// The loss report for a gap seen after `popped` samples with `dropped_total` samples dropped so
// far, `skew_before` of them already reported: the range is the dropped samples not yet reported.
[[nodiscard]] Notice overrun_notice(std::uint64_t popped, std::uint64_t skew_before,
                                    std::uint64_t dropped_total) noexcept;

// The loss report for a failed detect call: the frames the call lost, from the position of the
// next frame before the call to that position plus the growth of the offset the call caused.
[[nodiscard]] Notice vad_failure_notice(const StreamPosition& before,
                                        const StreamPosition& after) noexcept;

} // namespace lws
