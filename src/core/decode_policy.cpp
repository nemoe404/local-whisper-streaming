#include "lws/decode_policy.hpp"

#include "lws/options.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lws {

namespace {

// Space, tab, newline, carriage return, vertical tab and form feed.
constexpr std::string_view kAsciiSpace = " \t\n\r\v\f";

std::string trimmed(std::string_view text) {
    const std::size_t first = text.find_first_not_of(kAsciiSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(kAsciiSpace);
    std::string result{text.substr(first, last - first + 1U)};
    return result;
}

// Whole centiseconds that cover n_samples, so a clamped time never lands past the segment.
constexpr std::int64_t covering_centiseconds(std::size_t n_samples) noexcept {
    return static_cast<std::int64_t>((n_samples + kSamplesPerCentisecond - 1U) /
                                     kSamplesPerCentisecond);
}

TextSpan to_text_span(const RawSpan& raw, std::int64_t max_cs, std::uint64_t n_samples) {
    const std::uint64_t begin =
        std::min(cs_to_samples(std::clamp<std::int64_t>(raw.t0_cs, 0, max_cs)), n_samples);
    const std::uint64_t end =
        std::clamp(cs_to_samples(std::clamp<std::int64_t>(raw.t1_cs, 0, max_cs)), begin, n_samples);
    return TextSpan{.begin = begin, .end = end, .text = trimmed(raw.text)};
}

} // namespace

std::expected<DecoderOptions, Error> validate(const DecoderOptions& options) {
    if (options.n_threads < 1 || options.language.empty()) {
        return std::unexpected(Error::InvalidDecoderOptions);
    }
    // Positive predicates, negated once, so NaN fails the check.
    const bool step_usable =
        options.temperature_inc >= 0.0F && std::isfinite(options.temperature_inc);
    if (!step_usable) {
        return std::unexpected(Error::InvalidDecoderOptions);
    }
    return options;
}

std::vector<TextSpan> map_spans(std::span<const RawSpan> raw, std::size_t n_samples) {
    const std::int64_t max_cs = covering_centiseconds(n_samples);
    return raw | std::views::transform([max_cs, n_samples](const RawSpan& span) {
               return to_text_span(span, max_cs, n_samples);
           }) |
           std::views::filter([](const TextSpan& span) { return !span.text.empty(); }) |
           std::ranges::to<std::vector>();
}

} // namespace lws
