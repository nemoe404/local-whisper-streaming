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

// The text of consecutive decoded segments, with the words that overlap at a ceiling cut removed.
//
// A window cut at the ceiling keeps its last overlap_ms as the start of the next one, so the
// predecessor's last words are heard again at the start of the continuation. The reference is the
// raw text of the last decoded segment, and only a segment closed at the ceiling can be continued,
// so it is kept only after one. Any report between two segments, and any segment that gave no text
// (too short, cancelled, empty or failed), empties it: a continuation must never be merged against
// a segment that is not its predecessor. A merged continuation still starts at its first span, so
// it may overlap the previous event by up to the overlap. Pure value: the owner serialises calls.
class SeamText {
public:
    // The event for the segment, or nothing when its text is empty (no spans, or all of it removed
    // by the merge). The text is merged against the reference only when merging is enabled, the
    // segment continues the previous one and a reference is held. The reference then becomes this
    // segment's raw text if it closed at the ceiling, else it is emptied.
    [[nodiscard]] std::optional<TextEvent>
    compose(const Segment& segment, std::span<const TextSpan> spans, bool merge_enabled);

    // Something other than a decoded segment sits at this point of the stream.
    void interrupt() noexcept;

    [[nodiscard]] const std::string& reference() const noexcept;

private:
    std::string reference_; // raw text of the last decoded segment if it closed at the ceiling
};

} // namespace lws
