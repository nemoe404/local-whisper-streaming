#include "lws/vad_windower.hpp"

#include "trace_support.hpp"

#include <gtest/gtest.h>

#include <cstddef>

namespace {

constexpr float kSpeech = 0.9F;
constexpr float kQuiet = 0.1F;
constexpr std::size_t kFrame = lws::kVadFrameSamples;

} // namespace

// CORE-11, D-03, Edge 21: a force close emits the open window as is, never truncated, with the
// Discontinuity reason. The window was born without a notice, so the flag stays false.
TEST(ForceClose, OpenWindowIsEmittedAsIsWithDiscontinuityAndNeverTruncated) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 8, kSpeech).empty());
    const lws::Segment closed = lws::test::segment_of(windower.force_close());
    EXPECT_EQ(closed.reason, lws::CloseReason::Discontinuity);
    EXPECT_FALSE(closed.discontinuous_before);
    EXPECT_FALSE(closed.continues_previous);
    EXPECT_EQ(lws::test::frame_count(closed), 8U);
    EXPECT_EQ(closed.start, 0U);
    EXPECT_FLOAT_EQ(lws::test::frame_id(closed, 0), 1.0F);
    EXPECT_FLOAT_EQ(lws::test::frame_id(closed, 7), 8.0F);
    EXPECT_FALSE(windower.force_close().has_value());
}

// Edge 22, 27: the force close clears the tail, so the next window holds none of ids 1..8.
TEST(ForceClose, ClearsTheTailSoTheNextWindowHasNoPreCloseAudio) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 8, kSpeech).empty());
    EXPECT_TRUE(windower.force_close().has_value());
    EXPECT_TRUE(lws::test::feed_run(windower, 9.0F, 8, kSpeech).empty());
    const auto emitted = lws::test::feed_run(windower, 17.0F, 22, kQuiet);
    ASSERT_EQ(emitted.size(), 1U);
    EXPECT_FLOAT_EQ(lws::test::frame_id(emitted.front(), 0), 9.0F);
    EXPECT_FALSE(emitted.front().discontinuous_before);
    EXPECT_EQ(emitted.front().start, 8U * kFrame);
}
