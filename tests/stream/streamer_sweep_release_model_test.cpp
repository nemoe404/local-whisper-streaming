// On the release leg only: the recorded stream through the real models with different push sizes
// gives the same events every time, and the same size twice gives them twice.
#include "stream/stream_model_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <tuple>
#include <vector>

#include <fmt/core.h>

#include "lws/text_event.hpp"

namespace {

constexpr std::size_t kCopies = 2;

// The push size of each run. The last run repeats the third size.
std::array<std::size_t, 5> push_sizes(std::size_t whole_stream) {
    return {160, 512, 4096, whole_stream, 512};
}

} // namespace

TEST(StreamerSweepModel, PushSizeDoesNotChangeTheEvents) {
    const auto stream = lws::test::recorded_stream(kCopies);
    ASSERT_TRUE(stream.has_value()) << stream.error();

    const std::array<std::size_t, 5> sizes = push_sizes(stream->size());
    const auto lists = sizes | std::views::transform([&](std::size_t size) {
                           auto run = lws::test::stream_through(*stream, size);
                           return run ? run->events : std::vector<lws::TextEvent>{};
                       }) |
                       std::ranges::to<std::vector>();
    std::ranges::for_each(std::views::zip(sizes, lists), [](const auto& entry) {
        fmt::println("STREAM-SWEEP model={} size={} events={}", LWS_WHISPER_MODEL_NAME,
                     std::get<0>(entry), std::get<1>(entry).size());
    });

    ASSERT_EQ(lists.front().size(), kCopies);
    EXPECT_TRUE(
        std::ranges::all_of(lists, [&](const auto& list) { return list == lists.front(); }));
    EXPECT_EQ(lists[1], lists[4]) << "the two runs at 512 differ";
}
