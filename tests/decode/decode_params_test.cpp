// The whisper_full parameters the decoder builds, read back without loading a model: whisper's own
// speech detection is off for every option variant, the fixed fields hold, and the abort callback
// follows the caller's stop token.
#include <algorithm>
#include <array>
#include <cstdint>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <whisper.h>

#include <gtest/gtest.h>

#include "lws/decode_policy.hpp"
#include "shell/decoder.hpp"

namespace {

lws::DecoderOptions with_language(std::string_view language) {
    lws::DecoderOptions options;
    options.language = std::string(language);
    return options;
}

lws::DecoderOptions with_threads(std::int32_t n_threads) {
    lws::DecoderOptions options;
    options.n_threads = n_threads;
    return options;
}

lws::DecoderOptions with_temperature_step(float step) {
    lws::DecoderOptions options;
    options.temperature_inc = step;
    return options;
}

lws::DecoderOptions without_gpu() {
    lws::DecoderOptions options;
    options.use_gpu = false;
    return options;
}

std::vector<lws::DecoderOptions> every_variant() {
    return {lws::DecoderOptions{}, with_language("de"),         with_threads(1),
            with_threads(8),       with_temperature_step(0.0F), without_gpu()};
}

} // namespace

TEST(DecodeParams, WhisperVadIsOffForEveryOptionVariant) {
    std::ranges::for_each(every_variant(), [](const lws::DecoderOptions& options) {
        lws::AbortHook hook;
        const whisper_full_params params = lws::build_decode_params(options, hook);
        EXPECT_FALSE(params.vad);
        EXPECT_EQ(params.vad_model_path, nullptr);
    });
}

namespace {

using Lines = std::vector<std::string>;

// The names of the fixed fields that do not hold their pinned value.
Lines unpinned_fields(const whisper_full_params& params) {
    const std::array<std::pair<bool, const char*>, 12> pins{{
        {params.no_context, "no_context"},
        {!params.single_segment, "single_segment"},
        {params.offset_ms == 0, "offset_ms"},
        {params.duration_ms == 0, "duration_ms"},
        {!params.translate, "translate"},
        {!params.detect_language, "detect_language"},
        {!params.token_timestamps, "token_timestamps"},
        {!params.tdrz_enable, "tdrz_enable"},
        {!params.print_progress, "print_progress"},
        {!params.print_realtime, "print_realtime"},
        {!params.print_timestamps, "print_timestamps"},
        {!params.print_special, "print_special"},
    }};
    return pins | std::views::filter([](const auto& pin) { return !pin.first; }) |
           std::views::transform([](const auto& pin) { return std::string(pin.second); }) |
           std::ranges::to<std::vector>();
}

} // namespace

TEST(DecodeParams, FixedFieldsHoldForEveryOptionVariant) {
    std::ranges::for_each(every_variant(), [](const lws::DecoderOptions& options) {
        lws::AbortHook hook;
        EXPECT_EQ(unpinned_fields(lws::build_decode_params(options, hook)), Lines{});
    });
}

TEST(DecodeParams, LanguageAndThreadCountAreTheOptions) {
    lws::AbortHook hook;
    const lws::DecoderOptions german = with_language("de");
    EXPECT_EQ(std::string_view(lws::build_decode_params(german, hook).language), "de");
    const lws::DecoderOptions defaults;
    EXPECT_EQ(std::string_view(lws::build_decode_params(defaults, hook).language),
              std::string_view(defaults.language));
    EXPECT_EQ(lws::build_decode_params(with_threads(1), hook).n_threads, 1);
    EXPECT_EQ(lws::build_decode_params(with_threads(8), hook).n_threads, 8);
}

TEST(DecodeParams, TemperatureStepIsWhispersGreedyDefaultAndPassesThrough) {
    const float whisper_step = whisper_full_default_params(WHISPER_SAMPLING_GREEDY).temperature_inc;
    const float default_step = lws::DecoderOptions{}.temperature_inc;
    EXPECT_FLOAT_EQ(default_step, whisper_step);
    EXPECT_FLOAT_EQ(default_step, 0.2F);

    lws::AbortHook hook;
    EXPECT_FLOAT_EQ(lws::build_decode_params(lws::DecoderOptions{}, hook).temperature_inc,
                    whisper_step);
    EXPECT_FLOAT_EQ(lws::build_decode_params(with_temperature_step(0.0F), hook).temperature_inc,
                    0.0F);
}

TEST(DecodeParams, AbortCallbackFollowsTheHooksToken) {
    const std::stop_source source;
    lws::AbortHook hook{.token = source.get_token()};
    const whisper_full_params params = lws::build_decode_params(lws::DecoderOptions{}, hook);
    EXPECT_EQ(params.abort_callback_user_data, static_cast<void*>(&hook));
    ASSERT_NE(params.abort_callback, nullptr);
    EXPECT_FALSE(params.abort_callback(params.abort_callback_user_data));
    source.request_stop();
    EXPECT_TRUE(params.abort_callback(params.abort_callback_user_data));
}
