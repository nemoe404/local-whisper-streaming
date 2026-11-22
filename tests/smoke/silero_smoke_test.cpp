// Load-and-run check of the fetched Silero v5.1.2 model through whisper.cpp's VAD API. It proves
// the model loads and one frame scores to a probability; it does not measure accuracy.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

#include <gtest/gtest.h>

#include "whisper.h"

namespace {

// One Silero frame: a model constant read by whisper.cpp from the model file, not a tuning value.
constexpr std::size_t kVadFrameSamples = 512;

struct VadContextDeleter {
    void operator()(whisper_vad_context* ctx) const noexcept { whisper_vad_free(ctx); }
};
using VadContext = std::unique_ptr<whisper_vad_context, VadContextDeleter>;

void score_one_silent_frame(std::int32_t n_threads) {
    if (!std::filesystem::exists(LWS_SILERO_MODEL_PATH)) {
        GTEST_SKIP() << "model missing: run scripts/fetch-models.sh silero-v5.1.2";
    }

    auto params = whisper_vad_default_context_params();
    params.n_threads = static_cast<int>(n_threads); // C boundary
    params.use_gpu = false;

    const VadContext ctx{whisper_vad_init_from_file_with_params(LWS_SILERO_MODEL_PATH, params)};
    ASSERT_NE(ctx, nullptr);

    const std::array<float, kVadFrameSamples> frame{};
    ASSERT_TRUE(whisper_vad_detect_speech(ctx.get(), frame.data(), static_cast<int>(frame.size())));
    ASSERT_EQ(whisper_vad_n_probs(ctx.get()), 1);

    // Read immediately: whisper.cpp replaces this buffer on every detect call.
    const std::span<const float> probs{whisper_vad_probs(ctx.get()), 1};
    const float p = probs.front();
    EXPECT_TRUE(std::isfinite(p));
    EXPECT_GE(p, 0.0F);
    EXPECT_LE(p, 1.0F);
}

} // namespace

TEST(SileroSmoke, ScoresOneSilentFrame) {
    constexpr std::int32_t kSingleThread = 1; // the production invariant for the VAD
    score_one_silent_frame(kSingleThread);
}

// whisper.cpp's default VAD thread count; it exercises ggml's thread pool.
TEST(SileroSmokeMt, ScoresOneSilentFrameFourThreads) {
    constexpr std::int32_t kGgmlPoolThreads = 4;
    score_one_silent_frame(kGgmlPoolThreads);
}
