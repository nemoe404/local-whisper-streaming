#include "lws/options.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>

namespace {

// Each helper builds options that differ from the defaults in one field only.
lws::Options with_threshold(float value) {
    lws::Options options;
    options.threshold = value;
    return options;
}

lws::Options with_ceiling(float seconds) {
    lws::Options options;
    options.max_speech_duration_s = seconds;
    return options;
}

} // namespace

// The defaults are accepted.
TEST(OptionsDefaults, DefaultsValidate) { EXPECT_TRUE(lws::validate(lws::Options{}).has_value()); }

// The open interval (0, 1), NaN included.
TEST(OptionsThreshold, ZeroOneNanNegativeAndAboveOneAreInvalidThreshold) {
    const std::array values{0.0F, 1.0F, std::numeric_limits<float>::quiet_NaN(), -0.1F, 1.5F};
    std::ranges::for_each(values, [](float value) {
        const auto result = lws::validate(with_threshold(value));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), lws::Error::InvalidThreshold);
    });
}

// A ceiling that is not a positive finite number is an invalid duration.
TEST(OptionsDuration, NonFiniteAndNonPositiveCeilingAreInvalidDuration) {
    const std::array values{std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity(), 0.0F, -1.0F};
    std::ranges::for_each(values, [](float value) {
        const auto result = lws::validate(with_ceiling(value));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), lws::Error::InvalidDuration);
    });
}

// Exactly 25 s is the largest accepted ceiling.
TEST(OptionsDuration, TwentyFiveSecondsAcceptedAboveTwentyFiveRejected) {
    EXPECT_TRUE(lws::validate(with_ceiling(25.0F)).has_value());
    const auto result = lws::validate(with_ceiling(25.001F));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), lws::Error::CeilingTooLong);
}

// Q6: the ceiling must leave room for the pre-roll and the onset streak.
TEST(OptionsDuration, CeilingNoLongerThanPreRollPlusOnsetIsCeilingTooShort) {
    lws::Options options;
    options.pre_roll_ms = 300;
    options.min_speech_duration_ms = 250;
    options.max_speech_duration_s = 0.25F;
    const auto result = lws::validate(options);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), lws::Error::CeilingTooShort);
}

// Zero minimum speech is accepted; it clamps to one frame later.
TEST(OptionsOnset, ZeroMinSpeechIsAccepted) {
    lws::Options options;
    options.min_speech_duration_ms = 0;
    EXPECT_TRUE(lws::validate(options).has_value());
}

// ceil(ms / 32).
TEST(FrameConversion, CeilDivisionByThirtyTwoMs) {
    EXPECT_EQ(lws::ms_to_frames_ceil(250), 8U);
    EXPECT_EQ(lws::ms_to_frames_ceil(300), 10U);
    EXPECT_EQ(lws::ms_to_frames_ceil(700), 22U);
    EXPECT_EQ(lws::ms_to_frames_ceil(0), 0U);
    EXPECT_EQ(lws::ms_to_frames_ceil(32), 1U);
    EXPECT_EQ(lws::ms_to_frames_ceil(33), 2U);
}

// One frame is a whole 32 ms.
TEST(FrameOwner, Frame512IsWholeMilliseconds) {
    EXPECT_EQ(lws::kVadFrameSamples * 1000U / lws::kSampleRateHz, 32U);
}
