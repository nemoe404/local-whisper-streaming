// The frame-size probe over fake engines: no model, no ggml.
#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <ranges>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "fake_vad.hpp"
#include "shell/vad_engine.hpp"

namespace {

using lws::test::FakeCall;
using lws::test::FakeVadLog;

constexpr std::array<std::size_t, 5> kOtherWindows{256, 511, 513, 1024, 1536};

} // namespace

TEST(VadProbe, AcceptsFrameSize512) {
    FakeVadLog log;
    auto engine = lws::test::fake_engine(log, lws::kVadFrameSamples);
    const auto result = lws::probe_frame_size(engine);
    EXPECT_TRUE(result.has_value());
    const std::vector<FakeCall> expected{{FakeCall::Kind::Detect, 512},
                                         {FakeCall::Kind::Detect, 513}};
    EXPECT_EQ(log.calls, expected);
}

TEST(VadProbe, RejectsEveryOtherFrameSize) {
    const auto accepted_wrongly =
        kOtherWindows | std::views::filter([](std::size_t window) {
            FakeVadLog log;
            auto engine = lws::test::fake_engine(log, window);
            const auto result = lws::probe_frame_size(engine);
            return result.has_value() || result.error() != lws::Error::FrameSizeMismatch;
        }) |
        std::ranges::to<std::vector>();
    EXPECT_TRUE(accepted_wrongly.empty());
}

TEST(VadProbe, DetectErrorIsReturnedUnchanged) {
    lws::VadEngine engine{
        .detect = [](std::span<const float>) -> std::expected<std::span<const float>, lws::Error> {
            return std::unexpected(lws::Error::VadFailed);
        },
        .reset = [] {},
    };
    const auto result = lws::probe_frame_size(engine);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), lws::Error::VadFailed);
}

TEST(VadProbe, NeverResets) {
    FakeVadLog log;
    // Every window is probed on its own engine over the shared log; none may end in an error other
    // than a size mismatch.
    const auto probed_cleanly = [&log](std::size_t window) {
        auto engine = lws::test::fake_engine(log, window);
        const auto result = lws::probe_frame_size(engine);
        return result.has_value() || result.error() == lws::Error::FrameSizeMismatch;
    };
    EXPECT_TRUE(std::ranges::all_of(kOtherWindows, probed_cleanly));
    EXPECT_TRUE(probed_cleanly(lws::kVadFrameSamples));
    EXPECT_FALSE(lws::test::detect_sizes(log).empty());
    EXPECT_EQ(lws::test::resets(log), 0U);
}
