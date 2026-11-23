#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <vector>

// Helpers for ring tests. Not matched by the tests/shell/*_test.cpp glob, and outside the
// clang-tidy header filter like tests/core/trace_support.hpp, so it holds no logic beyond these
// helpers.
namespace lws::test {

// A self-describing sample: the low 32 bits of index * 2654435761 as a float bit pattern. The
// stream includes NaN patterns, so compare through bits_of, never float equality.
inline float pattern_sample(std::uint64_t index) {
    return std::bit_cast<float>(static_cast<std::uint32_t>(index * 2654435761ULL));
}

inline std::vector<float> pattern(std::uint64_t first, std::size_t count) {
    std::vector<float> samples(count);
    std::ranges::transform(std::views::iota(first, first + count), samples.begin(),
                           [](std::uint64_t index) { return pattern_sample(index); });
    return samples;
}

// Quiet NaN with payload, signalling NaN pattern, -0.0F, a subnormal, +inf, -inf, largest finite.
inline std::array<std::uint32_t, 7> special_bits() {
    return {0x7FC00001U, 0x7F800001U, 0x80000000U, 0x00000001U,
            0x7F800000U, 0xFF800000U, 0x7F7FFFFFU};
}

inline std::vector<std::uint32_t> bits_of(std::span<const float> samples) {
    std::vector<std::uint32_t> bits(samples.size());
    std::ranges::transform(samples, bits.begin(),
                           [](float sample) { return std::bit_cast<std::uint32_t>(sample); });
    return bits;
}

} // namespace lws::test
