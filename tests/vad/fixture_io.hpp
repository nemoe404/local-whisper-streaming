#pragma once

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Readers for the committed audio fixtures and the reference probability traces. Not matched by the
// tests/vad/*_test.cpp glob, and outside the clang-tidy header filter like fake_vad.hpp. Every
// failed check is named in the error string.
namespace lws::test {

namespace detail {

inline std::expected<std::string, std::string> read_whole_file(std::string_view name) {
    const std::string path = std::string(LWS_FIXTURE_DIR) + "/" + std::string(name);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected("cannot open " + path);
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline std::uint32_t le32(std::string_view bytes, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8U |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16U |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24U;
}

inline std::uint16_t le16(std::string_view bytes, std::size_t at) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at])) |
        static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at + 1])) << 8U);
}

// The payload of the first chunk with this id at or after `at` in a RIFF body, walking by size.
inline std::optional<std::string_view> find_chunk(std::string_view bytes, std::size_t at,
                                                  std::string_view id) {
    constexpr std::size_t kChunkHeader = 8;
    if (at + kChunkHeader > bytes.size()) {
        return std::nullopt;
    }
    const std::size_t size = le32(bytes, at + 4);
    const std::size_t body = at + kChunkHeader;
    if (body + size > bytes.size()) {
        return std::nullopt;
    }
    if (bytes.substr(at, 4) == id) {
        return bytes.substr(body, size);
    }
    return find_chunk(bytes, body + size + (size % 2), id);
}

inline std::expected<float, std::string> parse_trace_line(std::size_t ordinal,
                                                          std::string_view line) {
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos) {
        return std::unexpected("trace line " + std::to_string(ordinal) + " has no space");
    }
    std::size_t index = 0;
    const std::string_view index_text = line.substr(0, space);
    const auto index_end = index_text.data() + index_text.size();
    const auto [index_stop, index_ec] = std::from_chars(index_text.data(), index_end, index);
    if (index_ec != std::errc{} || index_stop != index_end || index != ordinal) {
        return std::unexpected("trace line " + std::to_string(ordinal) +
                               " has a bad or out-of-order index");
    }
    float value = 0.0F;
    const std::string_view value_text = line.substr(space + 1);
    const auto value_end = value_text.data() + value_text.size();
    const auto [value_stop, value_ec] = std::from_chars(value_text.data(), value_end, value);
    if (value_ec != std::errc{} || value_stop != value_end) {
        return std::unexpected("trace line " + std::to_string(ordinal) + " has a bad probability");
    }
    return value;
}

} // namespace detail

// 16 kHz mono 16-bit PCM WAV under LWS_FIXTURE_DIR as floats in [-1, 1): int16 / 32768.
inline std::expected<std::vector<float>, std::string> load_fixture_wav(std::string_view name) {
    const auto file = detail::read_whole_file(name);
    if (!file) {
        return std::unexpected(file.error());
    }
    const std::string_view bytes = *file;
    constexpr std::size_t kRiffHeader = 12;
    if (bytes.size() < kRiffHeader || bytes.substr(0, 4) != "RIFF") {
        return std::unexpected("not a RIFF file");
    }
    if (bytes.substr(8, 4) != "WAVE") {
        return std::unexpected("RIFF form is not WAVE");
    }
    const auto fmt = detail::find_chunk(bytes, kRiffHeader, "fmt ");
    if (!fmt || fmt->size() < 16) {
        return std::unexpected("no fmt chunk of at least 16 bytes");
    }
    if (detail::le16(*fmt, 0) != 1) {
        return std::unexpected("format is not integer PCM");
    }
    if (detail::le16(*fmt, 2) != 1) {
        return std::unexpected("not one channel");
    }
    if (detail::le32(*fmt, 4) != 16000) {
        return std::unexpected("sample rate is not 16000 Hz");
    }
    if (detail::le16(*fmt, 14) != 16) {
        return std::unexpected("not 16 bits per sample");
    }
    const auto data = detail::find_chunk(bytes, kRiffHeader, "data");
    if (!data) {
        return std::unexpected("no data chunk");
    }
    const std::size_t count = data->size() / 2;
    return std::views::iota(std::size_t{0}, count) | std::views::transform([&](std::size_t i) {
               const auto sample = std::bit_cast<std::int16_t>(detail::le16(*data, i * 2));
               return static_cast<float>(sample) / 32768.0F;
           }) |
           std::ranges::to<std::vector>();
}

// One probability per line, "<index> <probability>", index equal to the ordinal of the data line;
// lines starting with '#' are skipped.
inline std::expected<std::vector<float>, std::string>
load_probability_trace(std::string_view name) {
    const auto file = detail::read_whole_file(name);
    if (!file) {
        return std::unexpected(file.error());
    }
    const auto lines =
        std::string_view(*file) | std::views::split('\n') | std::views::transform([](auto part) {
            return std::string_view(part.begin(), part.end());
        }) |
        std::views::filter([](std::string_view line) { return !line.empty() && line[0] != '#'; }) |
        std::ranges::to<std::vector>();
    const auto parsed = std::views::iota(std::size_t{0}, lines.size()) |
                        std::views::transform(
                            [&](std::size_t i) { return detail::parse_trace_line(i, lines[i]); }) |
                        std::ranges::to<std::vector>();
    const auto bad = std::ranges::find_if(parsed, [](const auto& r) { return !r.has_value(); });
    if (bad != parsed.end()) {
        return std::unexpected(bad->error());
    }
    return parsed | std::views::transform([](const auto& r) { return *r; }) |
           std::ranges::to<std::vector>();
}

} // namespace lws::test
