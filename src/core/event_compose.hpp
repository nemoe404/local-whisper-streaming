#pragma once

#include "segment_fifo.hpp"

#include "lws/decode_policy.hpp"
#include "lws/segment.hpp"
#include "lws/text_event.hpp"

#include <optional>
#include <span>
#include <string>

// Internal core header. What a caller sees for each queue item, as pure values: a decoded segment
// becomes at most one text event, a notice becomes an event of its own kind. No I/O, no thread.
namespace lws {

// The span texts joined with one space between each; no span gives an empty string.
[[nodiscard]] std::string joined_text(std::span<const TextSpan> spans);

// One Text event for the segment, from the first span's begin to the last span's end, placed in the
// offered stream by the segment's start. after_gap copies the segment's discontinuity mark. An
// empty span list gives nothing: a decode with no text produces no event.
[[nodiscard]] std::optional<TextEvent> text_event(const Segment& segment,
                                                  std::span<const TextSpan> spans);

// The notice's kind and range with empty text and no gap mark.
[[nodiscard]] TextEvent notice_event(const Notice& notice);

} // namespace lws
