#include "event_compose.hpp"

#include "segment_fifo.hpp"

#include "lws/decode_policy.hpp"
#include "lws/segment.hpp"
#include "lws/text_event.hpp"

#include <optional>
#include <ranges>
#include <span>
#include <string>

namespace lws {

std::string joined_text(std::span<const TextSpan> spans) {
    return spans | std::views::transform(&TextSpan::text) | std::views::join_with(' ') |
           std::ranges::to<std::string>();
}

std::optional<TextEvent> text_event(const Segment& segment, std::span<const TextSpan> spans) {
    if (spans.empty()) {
        return std::nullopt;
    }
    return TextEvent{.kind = EventKind::Text,
                     .t_start = segment.start + spans.front().begin,
                     .t_end = segment.start + spans.back().end,
                     .text = joined_text(spans),
                     .after_gap = segment.discontinuous_before};
}

TextEvent notice_event(const Notice& notice) {
    return TextEvent{.kind = notice.kind,
                     .t_start = notice.begin,
                     .t_end = notice.end,
                     .text = {},
                     .after_gap = false};
}

} // namespace lws
