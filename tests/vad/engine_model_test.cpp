// VadEngine over the fetched Silero v5.1.2 model: opening, scoring and the parameters it runs with.
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

#include "shell/vad_engine.hpp"

namespace {

constexpr const char* kMissingModelHint =
    "model missing: run scripts/fetch-models.sh silero-v5.1.2";

bool model_present() { return std::filesystem::exists(LWS_SILERO_MODEL_PATH); }

} // namespace

TEST(VadEngineModel, OpenScoresOneFrame) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    ASSERT_TRUE(engine.has_value());

    engine->reset();
    const std::vector<float> frame(lws::kVadFrameSamples, 0.0F);
    const auto probs = engine->detect(frame);
    ASSERT_TRUE(probs.has_value());
    ASSERT_EQ(probs->size(), 1U);
    const float p = probs->front();
    EXPECT_TRUE(std::isfinite(p));
    EXPECT_GE(p, 0.0F);
    EXPECT_LE(p, 1.0F);
}

TEST(VadEngineModel, MissingModelIsModelLoad) {
    const auto missing =
        std::filesystem::temp_directory_path() / "lws-no-such-directory" / "ggml-silero-v5.1.2.bin";
    const auto engine = lws::VadEngine::open(missing);
    ASSERT_FALSE(engine.has_value());
    EXPECT_EQ(engine.error(), lws::Error::ModelLoad);
}
