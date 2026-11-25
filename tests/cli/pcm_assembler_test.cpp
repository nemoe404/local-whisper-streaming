#include "cli/pcm_assembler.hpp"
#include "vad/fixture_io.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <numbers>
#include <ranges>
#include <span>
#include <vector>

namespace {

std::vector<std::byte> bytes_of(std::span<const float> floats) {
    std::vector<std::byte> bytes(floats.size_bytes());
    std::memcpy(bytes.data(), floats.data(), floats.size_bytes());
    return bytes;
}

std::vector<std::uint32_t> bits_of(std::span<const float> floats) {
    std::vector<std::uint32_t> bits;
    bits.reserve(floats.size());
    std::ranges::transform(floats, std::back_inserter(bits),
                           [](float value) { return std::bit_cast<std::uint32_t>(value); });
    return bits;
}

// Feeds `bytes` to a fresh assembler in slices of `slice` bytes and collects every float it emits.
std::vector<float> feed_sliced(std::span<const std::byte> bytes, std::size_t slice,
                               lws_cli::PcmAssembler& assembler) {
    std::vector<float> out;
    const std::size_t slices = (bytes.size() + slice - 1) / slice;
    std::ranges::for_each(std::views::iota(std::size_t{0}, slices), [&](std::size_t index) {
        const std::size_t offset = index * slice;
        const auto emitted =
            assembler.feed(bytes.subspan(offset, std::min(slice, bytes.size() - offset)));
        out.insert(out.end(), emitted.begin(), emitted.end());
    });
    return out;
}

// Ordinary values with the awkward ones spread through them.
std::vector<float> awkward_buffer() {
    std::vector<float> values(10000);
    std::ranges::transform(std::views::iota(0, 10000), values.begin(), [](int index) {
        return std::sin(static_cast<float>(index) * 0.01F) * 0.7F;
    });
    values[0] = -0.0F;
    values[1] = std::numeric_limits<float>::denorm_min();
    values[2] = 1.0F;
    values[3] = -1.0F;
    values[4999] = std::numeric_limits<float>::min();
    values[9999] = 1.0F;
    return values;
}

TEST(CliPcmAssembler, SlicedFeedsEqualOneWholeFeed) {
    const auto floats = awkward_buffer();
    const auto bytes = bytes_of(floats);
    const auto expected = bits_of(floats);

    lws_cli::PcmAssembler whole;
    const auto whole_out = whole.feed(bytes);
    ASSERT_EQ(bits_of(whole_out), expected);
    EXPECT_EQ(whole.pending_bytes(), 0U);

    constexpr std::array<std::size_t, 5> kSlices{1, 3, 4, 5, 4099};
    std::ranges::for_each(kSlices, [&](std::size_t slice) {
        lws_cli::PcmAssembler assembler;
        const auto out = feed_sliced(bytes, slice, assembler);
        EXPECT_EQ(bits_of(out), expected) << "slice " << slice;
        EXPECT_EQ(assembler.pending_bytes(), 0U) << "slice " << slice;
    });

    lws_cli::PcmAssembler assembler;
    EXPECT_EQ(assembler.feed(std::span{bytes}.first(bytes.size())).size(), floats.size());
    const std::array<std::byte, 3> extra{std::byte{1}, std::byte{2}, std::byte{3}};
    EXPECT_TRUE(assembler.feed(extra).empty());
    EXPECT_EQ(assembler.pending_bytes(), 3U);
}

TEST(CliPcmAssembler, ACarriedFloatSurvivesAFeedOfOneByte) {
    const std::array<float, 1> value{0.3125F};
    const auto bytes = bytes_of(value);
    ASSERT_EQ(bytes.size(), 4U);

    lws_cli::PcmAssembler assembler;
    std::vector<std::size_t> emitted_per_feed;
    std::vector<float> all;
    std::ranges::for_each(bytes, [&](std::byte byte) {
        const auto out = assembler.feed(std::span{&byte, 1});
        emitted_per_feed.push_back(out.size());
        all.insert(all.end(), out.begin(), out.end());
    });

    EXPECT_EQ(emitted_per_feed, (std::vector<std::size_t>{0, 0, 0, 1}));
    ASSERT_EQ(all.size(), 1U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(all.front()), std::bit_cast<std::uint32_t>(value[0]));
    EXPECT_EQ(assembler.pending_bytes(), 0U);
}

TEST(CliPcmAssembler, NonFiniteSamplesBecomeZeroAndAreCounted) {
    const std::array<float, 5> values{
        std::numeric_limits<float>::quiet_NaN(),
        std::bit_cast<float>(std::uint32_t{0x7FC12345U}),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        0.25F,
    };
    lws_cli::PcmAssembler assembler;
    const auto out = assembler.feed(bytes_of(values));

    ASSERT_EQ(out.size(), 5U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(out[0]), 0U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(out[1]), 0U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(out[2]), 0U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(out[3]), 0U);
    EXPECT_EQ(out[4], 0.25F);
    EXPECT_EQ(assembler.replaced_samples(), 4U);
}

TEST(CliPcmAssembler, Int16PairsReadAsFloatLookWrong) {
    constexpr std::size_t kSamples = 16000;
    std::vector<std::byte> bytes(kSamples * 2);
    std::ranges::for_each(std::views::iota(std::size_t{0}, kSamples), [&](std::size_t index) {
        const double phase = 2.0 * std::numbers::pi * 440.0 * static_cast<double>(index) / 16000.0;
        const auto sample = static_cast<std::int16_t>(std::lround(0.8 * 32767.0 * std::sin(phase)));
        std::memcpy(bytes.data() + index * 2, &sample, sizeof sample);
    });

    lws_cli::PcmAssembler assembler;
    EXPECT_FALSE(assembler.looks_wrong());
    static_cast<void>(assembler.feed(bytes));
    EXPECT_TRUE(assembler.looks_wrong());
}

TEST(CliPcmAssembler, QuietFloatSpeechDoesNotLookWrong) {
    const auto wav = lws::test::load_fixture_wav("clean_en_sample.wav");
    ASSERT_TRUE(wav.has_value()) << wav.error();
    ASSERT_GE(wav->size(), 16000U);

    lws_cli::PcmAssembler assembler;
    static_cast<void>(assembler.feed(bytes_of(std::span{*wav}.first(16000))));
    EXPECT_FALSE(assembler.looks_wrong());
    EXPECT_EQ(assembler.replaced_samples(), 0U);
}

} // namespace
