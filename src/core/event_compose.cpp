#include "event_compose.hpp"

#include "segment_fifo.hpp"

#include "lws/decode_policy.hpp"
#include "lws/seam_merge.hpp"
#include "lws/segment.hpp"
#include "lws/text_event.hpp"

#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>

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

std::optional<TextEvent> SeamText::compose(const Segment& segment, std::span<const TextSpan> spans,
                                           bool merge_enabled) {
    std::string raw = joined_text(spans);
    const bool merge = merge_enabled && segment.continues_previous && !reference_.empty();
    std::string text = merge ? seam_merge(reference_, raw) : raw;
    reference_ = segment.reason == CloseReason::Ceiling ? std::move(raw) : std::string{};
    auto event = text_event(segment, spans);
    if (!event.has_value() || text.empty()) {
        return std::nullopt;
    }
    event->text = std::move(text);
    return event;
}

void SeamText::interrupt() noexcept { reference_.clear(); }

const std::string& SeamText::reference() const noexcept { return reference_; }

} // namespace lws
