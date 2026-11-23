// ChunkReader over fake engines: startup, exact frames and the reset points. No ggml compute.
#include <array>
#include <cstddef>
#include <ranges>
#include <vector>

#include <gtest/gtest.h>

#include "fake_vad.hpp"
#include "shell/chunk_reader.hpp"

namespace {

using lws::test::FakeCall;
using lws::test::FakeVadLog;

constexpr std::size_t kFrame = lws::kVadFrameSamples;

// The calls a successful create leaves in the log: the two probe calls, then the session-start
// reset.
std::vector<FakeCall> startup_log() {
    return {
        {FakeCall::Kind::Detect, kFrame},
        {FakeCall::Kind::Detect, kFrame + 1},
        {FakeCall::Kind::Reset, 0},
    };
}

} // namespace

TEST(ChunkReaderStartup, ProbeThenOneReset) {
    FakeVadLog log;
    const auto reader = lws::ChunkReader::create(lws::test::fake_engine(log, kFrame));
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(log.calls, startup_log());
}

TEST(ChunkReaderStartup, FrameSizeOtherThan512FailsCreate) {
    constexpr std::array<std::size_t, 2> kWindows{256, 1024};
    const auto failures =
        kWindows | std::views::filter([](std::size_t window) {
            FakeVadLog log;
            const auto reader = lws::ChunkReader::create(lws::test::fake_engine(log, window));
            const bool wrong_error =
                reader.has_value() || reader.error() != lws::Error::FrameSizeMismatch;
            return wrong_error || lws::test::resets(log) != 0;
        }) |
        std::ranges::to<std::vector>();
    EXPECT_TRUE(failures.empty());
}

TEST(ChunkReaderStartup, OneWholeFrameIsScored) {
    FakeVadLog log;
    auto reader = lws::ChunkReader::create(lws::test::fake_engine(log, kFrame));
    ASSERT_TRUE(reader.has_value());

    const std::array<float, 1> values{0.75F};
    const auto input = lws::test::scripted_stream(values);
    const auto scored = reader->feed(input);

    ASSERT_TRUE(scored.has_value());
    ASSERT_EQ(scored->size(), 1U);
    EXPECT_EQ(scored->probs.front(), 0.75F);
    EXPECT_EQ(scored->samples, input);
    EXPECT_EQ(reader->pending(), 0U);
}
