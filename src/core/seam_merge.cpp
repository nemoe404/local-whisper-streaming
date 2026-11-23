#include "lws/seam_merge.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <string>
#include <vector>

namespace lws {

namespace {

// CORE-07: a seam duplicates at most this many words.
constexpr std::size_t kMaxRun = 4;

// Keys at or above this value stand for one invalid byte (key = base + byte), so an invalid byte
// equals only the same byte and never a real code point.
constexpr std::uint32_t kInvalidKeyBase = 0x110000;

[[nodiscard]] std::uint8_t byte_at(std::string_view text, std::size_t at) {
    return static_cast<std::uint8_t>(text[at]);
}

[[nodiscard]] bool is_continuation(std::uint8_t byte) { return byte >= 0x80 && byte <= 0xBF; }

// Sequence length and the allowed range of the first continuation byte for a lead byte.
// Length 0 means the byte cannot start a multi-byte sequence.
struct Shape {
    std::size_t length;
    std::uint8_t first_low;
    std::uint8_t first_high;
};

[[nodiscard]] Shape shape_of(std::uint8_t lead) {
    if (lead >= 0xC2 && lead <= 0xDF) {
        return {.length = 2, .first_low = 0x80, .first_high = 0xBF};
    }
    if (lead == 0xE0) {
        return {.length = 3, .first_low = 0xA0, .first_high = 0xBF};
    }
    if (lead == 0xED) { // no surrogates
        return {.length = 3, .first_low = 0x80, .first_high = 0x9F};
    }
    if (lead >= 0xE1 && lead <= 0xEF) {
        return {.length = 3, .first_low = 0x80, .first_high = 0xBF};
    }
    if (lead == 0xF0) {
        return {.length = 4, .first_low = 0x90, .first_high = 0xBF};
    }
    if (lead >= 0xF1 && lead <= 0xF3) {
        return {.length = 4, .first_low = 0x80, .first_high = 0xBF};
    }
    if (lead == 0xF4) {
        return {.length = 4, .first_low = 0x80, .first_high = 0x8F};
    }
    return {.length = 0, .first_low = 0, .first_high = 0};
}

// Byte length of the valid UTF-8 sequence starting at `at`, or 0 when none does.
[[nodiscard]] std::size_t unit_length(std::string_view text, std::size_t at) {
    assert(at < text.size());
    const std::uint8_t lead = byte_at(text, at);
    if (lead <= 0x7F) {
        return 1;
    }
    const Shape shape = shape_of(lead);
    if (shape.length == 0 || at + shape.length > text.size()) {
        return 0;
    }
    const std::uint8_t first = byte_at(text, at + 1);
    if (first < shape.first_low || first > shape.first_high) {
        return 0;
    }
    const auto tail = text.substr(at + 2, shape.length - 2);
    const bool tail_ok = std::ranges::all_of(
        tail, [](char c) { return is_continuation(static_cast<std::uint8_t>(c)); });
    return tail_ok ? shape.length : 0;
}

struct Unit {
    std::uint32_t key;
    std::size_t length;
};

// The code point of the valid sequence at `at`, or one invalid byte as its own unit.
[[nodiscard]] Unit decode_unit(std::string_view text, std::size_t at) {
    const std::size_t length = unit_length(text, at);
    if (length == 0) {
        return {.key = kInvalidKeyBase + byte_at(text, at), .length = 1};
    }
    if (length == 1) {
        return {.key = byte_at(text, at), .length = 1};
    }
    // Lead payload mask: 0x1F for two bytes, 0x0F for three, 0x07 for four.
    const std::uint32_t lead_bits = byte_at(text, at) & (0xFFU >> (length + 1));
    const std::uint32_t code_point = std::ranges::fold_left(
        text.substr(at + 1, length - 1), lead_bits, [](std::uint32_t acc, char c) {
            return (acc << 6U) | (static_cast<std::uint8_t>(c) & 0x3FU);
        });
    return {.key = code_point, .length = length};
}

// False only for a continuation byte that a valid sequence covers. The covering lead is the nearest
// non-continuation byte before `at`, searched back at most three bytes; it covers `at` when its
// sequence reaches that position. This matches a left-to-right greedy decode.
[[nodiscard]] bool is_unit_start(std::string_view text, std::size_t at) {
    if (!is_continuation(byte_at(text, at))) {
        return true;
    }
    const std::size_t window = std::min<std::size_t>(3, at);
    const auto prefix = text.substr(at - window, window) | std::views::reverse;
    const auto lead = std::ranges::find_if_not(
        prefix, [](char c) { return is_continuation(static_cast<std::uint8_t>(c)); });
    if (lead == prefix.end()) {
        return true;
    }
    const auto behind = static_cast<std::size_t>(std::ranges::distance(prefix.begin(), lead)) + 1;
    return unit_length(text, at - behind) <= behind;
}

[[nodiscard]] std::vector<Unit> units(std::string_view text) {
    return std::views::iota(std::size_t{0}, text.size()) |
           std::views::filter([text](std::size_t at) { return is_unit_start(text, at); }) |
           std::views::transform([text](std::size_t at) { return decode_unit(text, at); }) |
           std::ranges::to<std::vector>();
}

// ASCII punctuation plus the typographic marks Whisper emits at word edges.
[[nodiscard]] bool is_boundary(std::uint32_t key) {
    const bool ascii_punctuation = (key >= 0x21 && key <= 0x2F) || (key >= 0x3A && key <= 0x40) ||
                                   (key >= 0x5B && key <= 0x60) || (key >= 0x7B && key <= 0x7E);
    const bool typographic = key == 0xAB || key == 0xBB || key == 0x201E || key == 0x201C ||
                             key == 0x201D || key == 0x2014 || key == 0x2026;
    return ascii_punctuation || typographic;
}

// Lightweight one-to-one case fold: ASCII, Latin-1 Supplement, Latin Extended-A, Cyrillic and
// U+0490. Greek and other scripts are compared as code points (out of scope in v0.1.0).
[[nodiscard]] std::uint32_t fold(std::uint32_t key) {
    if ((key >= 0x41 && key <= 0x5A) || (key >= 0xC0 && key <= 0xDE && key != 0xD7)) {
        return key + 0x20;
    }
    if (key == 0x130) { // I with dot above folds to plain i, not to its parity neighbour
        return 0x69;
    }
    if (key == 0x178) { // Y with diaeresis folds into Latin-1
        return 0xFF;
    }
    // Even-uppercase blocks.
    if ((key >= 0x100 && key <= 0x137) || (key >= 0x14A && key <= 0x177)) {
        return key % 2 == 0 ? key + 1 : key;
    }
    // Odd-uppercase blocks: the uppercase letter has the odd code point.
    if ((key >= 0x139 && key <= 0x148) || (key >= 0x179 && key <= 0x17E)) {
        return key % 2 == 1 ? key + 1 : key;
    }
    if (key >= 0x400 && key <= 0x40F) {
        return key + 0x50;
    }
    if (key >= 0x410 && key <= 0x42F) {
        return key + 0x20;
    }
    if (key == 0x490) {
        return 0x491;
    }
    return key;
}

struct Word {
    std::size_t offset; // byte index of the first byte in the text it came from
    std::vector<std::uint32_t> key;
};

// Boundary punctuation stripped at both ends, then folded. Empty for a punctuation-only word.
[[nodiscard]] std::vector<std::uint32_t> normalised_key(std::string_view word) {
    return units(word) | std::views::transform(&Unit::key) | std::views::drop_while(is_boundary) |
           std::views::reverse | std::views::drop_while(is_boundary) | std::views::reverse |
           std::views::transform(fold) | std::ranges::to<std::vector>();
}

// Non-empty words split on the ASCII space byte. A space never lies inside a valid UTF-8 sequence.
[[nodiscard]] std::vector<Word> words(std::string_view text) {
    return std::views::split(text, ' ') |
           std::views::filter([](const auto& piece) { return !std::ranges::empty(piece); }) |
           std::views::transform([text](const auto& piece) {
               const auto offset =
                   static_cast<std::size_t>(std::ranges::distance(text.begin(), piece.begin()));
               const auto length = static_cast<std::size_t>(std::ranges::distance(piece));
               return Word{.offset = offset, .key = normalised_key(text.substr(offset, length))};
           }) |
           std::ranges::to<std::vector>();
}

// D8b: a word whose key is empty (punctuation only) never matches.
[[nodiscard]] bool words_match(const Word& a, const Word& b) {
    return !a.key.empty() && !b.key.empty() && std::ranges::equal(a.key, b.key);
}

} // namespace

std::string seam_merge(std::string_view previous, std::string_view continuation) {
    const auto prev = words(previous);
    const auto cont = words(continuation);
    const std::size_t max_run = std::min({kMaxRun, prev.size(), cont.size()});

    // Largest run first, so a two-word repeat is removed as two words (Pitfall 15).
    const auto runs = std::views::iota(std::size_t{1}, max_run + 1) | std::views::reverse;
    const auto hit = std::ranges::find_if(runs, [&](std::size_t k) {
        return std::ranges::equal(prev | std::views::drop(prev.size() - k),
                                  cont | std::views::take(k), words_match);
    });
    if (hit == runs.end()) {
        return std::string(continuation);
    }
    const std::size_t run = *hit;
    if (run == cont.size()) {
        return {};
    }
    assert(cont[run].offset <= continuation.size());
    return std::string(continuation.substr(cont[run].offset));
}

} // namespace lws
