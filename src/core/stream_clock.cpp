#include "stream_clock.hpp"

#include <cassert>
#include <cstdint>
#include <utility>

#include "lws/segment.hpp"
#include "lws/text_event.hpp"

namespace lws {

Segment stamped(Segment segment, std::uint64_t offset) noexcept {
    segment.start += offset;
    return segment;
}

Notice overrun_notice(std::uint64_t popped, std::uint64_t skew_before,
                      std::uint64_t dropped_total) noexcept {
    assert(dropped_total >= skew_before);
    return Notice{
        .kind = EventKind::Overrun, .begin = popped + skew_before, .end = popped + dropped_total};
}

Notice vad_failure_notice(const StreamPosition& before, const StreamPosition& after) noexcept {
    const std::uint64_t begin = next_frame_position(before);
    return Notice{.kind = EventKind::VadFailure,
                  .begin = begin,
                  .end = begin + stream_offset(after) - stream_offset(before)};
}

} // namespace lws
