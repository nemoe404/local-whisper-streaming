// A quiet word onset planted ahead of the first over-threshold frame of the recorded fixture,
// through the real Silero engine and the Segmenter: the pre-roll must keep it in the segment for
// every onset-frame count, and without pre-roll it must be cut.
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "fixture_io.hpp"
#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "shell/chunk_reader.hpp"
#include "shell/segmenter.hpp"
#include "shell/vad_engine.hpp"

namespace {

constexpr const char* kMissingModelHint =
    "model missing: run scripts/fetch-models.sh silero-v5.1.2";

constexpr std::size_t kFeedChunk = lws::kVadFrameSamples;
constexpr std::size_t kLeadingZeros = 32 * lws::kVadFrameSamples;
constexpr std::size_t kPlantFrames = 6;
constexpr std::size_t kPlantSamples = kPlantFrames * lws::kVadFrameSamples; // 192 ms
constexpr std::size_t kPlantSourceBegin =
    88064; // frame 172 of the clip, the word after the first pause
constexpr std::size_t kRunFrames = 8;
constexpr float kThreshold = 0.5F;
constexpr std::int64_t kLeadInFloorMs = 300;
constexpr std::uint32_t kSamplesPerMs = lws::kSampleRateHz / 1000;

// The largest power of two, from the sweep 0.25, 0.125, 0.0625, 0.03125, 0.015625, 0.0078125, for
// which every planted frame stays under the threshold and the first frame at or above it is the one
// the plant ends on. Measured: first frame at or above 0.5 in the base stream is 44; at this gain
// the planted frames 38 to 43 peak at 0.1886 and the first frame at or above 0.5 in the planted
// stream is 44, followed by a run of 156. At 0.03125 the planted frames peak at 0.6758, so a
// planted frame is itself the first speech frame. The test asserts these conditions on every run.
constexpr float kPlantGain = 0.015625F;

using Segments = std::vector<lws::Segment>;

bool model_present() { return std::filesystem::exists(LWS_SILERO_MODEL_PATH); }

// 32 frames of zeros, then the clip.
std::vector<float> base_stream(std::span<const float> wav) {
    std::vector<float> stream(kLeadingZeros, 0.0F);
    stream.insert(stream.end(), wav.begin(), wav.end());
    return stream;
}

// Consecutive pieces of `chunk` samples; the last one may be shorter.
auto pieces_of(std::span<const float> stream, std::size_t chunk) {
    const std::size_t count = (stream.size() + chunk - 1) / chunk;
    return std::views::iota(std::size_t{0}, count) | std::views::transform([=](std::size_t i) {
               return stream.subspan(i * chunk, std::min(chunk, stream.size() - (i * chunk)));
           });
}

// Every probability the real engine gives over the stream, read once through the chunk reader.
std::expected<std::vector<float>, std::string> probabilities_of(std::span<const float> stream) {
    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    if (!engine) {
        return std::unexpected("model load failed");
    }
    auto reader = lws::ChunkReader::create(std::move(*engine));
    if (!reader) {
        return std::unexpected("reader create failed");
    }
    std::vector<float> probs;
    std::optional<std::string> failure;
    std::ranges::for_each(pieces_of(stream, kFeedChunk), [&](std::span<const float> piece) {
        if (failure) {
            return;
        }
        const auto scored = reader->feed(piece);
        if (!scored) {
            failure = "feed failed";
            return;
        }
        probs.insert(probs.end(), scored->probs.begin(), scored->probs.end());
    });
    if (failure) {
        return std::unexpected(*failure);
    }
    return probs;
}

std::expected<Segments, std::string> segment_stream(std::span<const float> stream,
                                                    const lws::Options& options) {
    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    if (!engine) {
        return std::unexpected("model load failed");
    }
    auto segmenter = lws::Segmenter::create(std::move(*engine), options);
    if (!segmenter) {
        return std::unexpected("segmenter create failed");
    }
    Segments segments;
    std::optional<std::string> failure;
    std::ranges::for_each(pieces_of(stream, kFeedChunk), [&](std::span<const float> piece) {
        if (failure) {
            return;
        }
        auto closed = segmenter->push(piece);
        if (!closed) {
            failure =
                "push failed with error " + std::to_string(static_cast<int>(closed.error().error));
            return;
        }
        std::ranges::move(*closed, std::back_inserter(segments));
    });
    if (failure) {
        return std::unexpected(*failure);
    }
    if (auto last = segmenter->flush()) {
        segments.push_back(std::move(*last));
    }
    return segments;
}

bool is_speech(float p) { return p >= kThreshold; }

std::optional<std::size_t> first_speech_frame(std::span<const float> probs) {
    const auto it = std::ranges::find_if(probs, is_speech);
    return it == probs.end()
               ? std::nullopt
               : std::optional<std::size_t>(static_cast<std::size_t>(it - probs.begin()));
}

// The planted samples: the clip's own speech, scaled by a power of two so every value is exact.
std::vector<float> plant_values(std::span<const float> wav, float gain) {
    return wav.subspan(kPlantSourceBegin, kPlantSamples) |
           std::views::transform([gain](float v) { return v * gain; }) |
           std::ranges::to<std::vector>();
}

// The base stream with the plant written over the kPlantSamples ending where frame `first` begins.
std::vector<float> planted_stream(std::span<const float> base, std::span<const float> wav,
                                  std::size_t first, float gain) {
    std::vector<float> stream(base.begin(), base.end());
    const auto values = plant_values(wav, gain);
    std::ranges::copy(
        values, stream.begin() +
                    static_cast<std::ptrdiff_t>((first * lws::kVadFrameSamples) - kPlantSamples));
    return stream;
}

// What the plant conditions read for one gain.
struct PlantReport {
    float gain = 0.0F;
    std::size_t first_in_base = 0;    // F
    std::size_t first_in_planted = 0; // F'
    float max_planted = 0.0F;
    std::size_t run_from_first = 0; // frames at or above the threshold starting at F'
    bool quiet = false;
    bool near_first = false;
    bool run_holds = false;
};

struct PlantedRun {
    PlantReport report;
    std::vector<float> stream;
};

std::size_t run_length(std::span<const float> probs, std::size_t from) {
    const auto rest = probs.subspan(from);
    return static_cast<std::size_t>(std::ranges::find_if_not(rest, is_speech) - rest.begin());
}

std::expected<PlantedRun, std::string> plant_at(std::span<const float> wav,
                                                std::span<const float> base,
                                                std::span<const float> base_probs, float gain) {
    const auto first = first_speech_frame(base_probs);
    if (!first || *first * lws::kVadFrameSamples < kPlantSamples) {
        return std::unexpected("no room for the plant ahead of the first speech frame");
    }
    auto stream = planted_stream(base, wav, *first, gain);
    const auto probs = probabilities_of(stream);
    if (!probs) {
        return std::unexpected(probs.error());
    }
    const auto first_planted = first_speech_frame(*probs);
    if (!first_planted) {
        return std::unexpected("no frame reaches the threshold in the planted stream");
    }
    const auto plant_probs =
        std::span<const float>(*probs).subspan(*first - kPlantFrames, kPlantFrames);
    const float max_planted = *std::ranges::max_element(plant_probs);
    const std::size_t run = run_length(*probs, *first_planted);
    return PlantedRun{
        .report = {.gain = gain,
                   .first_in_base = *first,
                   .first_in_planted = *first_planted,
                   .max_planted = max_planted,
                   .run_from_first = run,
                   .quiet = max_planted < kThreshold,
                   .near_first = *first_planted >= *first && *first_planted <= *first + 2,
                   .run_holds = run >= kRunFrames},
        .stream = std::move(stream)};
}

const char* flag(bool value) { return value ? "1" : "0"; }

std::string describe(const PlantReport& r) {
    return "gain=" + std::to_string(r.gain) + " max_planted=" + std::to_string(r.max_planted) +
           " F=" + std::to_string(r.first_in_base) + " Fp=" + std::to_string(r.first_in_planted) +
           " run=" + std::to_string(r.run_from_first) + " quiet=" + flag(r.quiet) +
           " near=" + flag(r.near_first) + " run8=" + flag(r.run_holds);
}

// What one onset count did to the plant.
struct Survival {
    std::uint32_t onset_frames = 0;
    std::int64_t lead_in_ms = 0;
    bool starts_at_or_before_plant = false;
    bool holds_plant = false;
};

lws::Options options_with(std::uint32_t min_speech_ms, std::uint32_t pre_roll_ms) {
    lws::Options options;
    options.min_speech_duration_ms = min_speech_ms;
    options.pre_roll_ms = pre_roll_ms;
    return options;
}

// Segments the planted stream and reads the first segment against the plant.
std::expected<Survival, std::string> survival_of(const PlantedRun& run, std::span<const float> wav,
                                                 const lws::Options& options) {
    const auto segments = segment_stream(run.stream, options);
    if (!segments) {
        return std::unexpected(segments.error());
    }
    if (segments->empty()) {
        return std::unexpected("no segment");
    }
    const lws::Segment& first = segments->front();
    const std::uint64_t plant_begin =
        (run.report.first_in_base * lws::kVadFrameSamples) - kPlantSamples;
    const bool before = first.start <= plant_begin;
    const auto values = plant_values(wav, run.report.gain);
    const auto bits = [](std::span<const float> v) {
        return v | std::views::transform([](float x) { return std::bit_cast<std::uint32_t>(x); }) |
               std::ranges::to<std::vector>();
    };
    const auto offset = static_cast<std::size_t>(plant_begin - first.start);
    const bool holds =
        before && offset + kPlantSamples <= first.samples.size() &&
        bits(std::span<const float>(first.samples).subspan(offset, kPlantSamples)) == bits(values);
    const auto onset_start =
        static_cast<std::int64_t>(run.report.first_in_planted * lws::kVadFrameSamples);
    return Survival{
        .onset_frames =
            static_cast<std::uint32_t>(lws::ms_to_frames_ceil(options.min_speech_duration_ms)),
        .lead_in_ms = (onset_start - static_cast<std::int64_t>(first.start)) / kSamplesPerMs,
        .starts_at_or_before_plant = before,
        .holds_plant = holds};
}

// The clip, the base stream and the base probabilities, once per test.
struct Inputs {
    std::vector<float> wav;
    std::vector<float> base;
    std::vector<float> base_probs;
};

std::expected<Inputs, std::string> load_inputs() {
    auto wav = lws::test::load_fixture_wav("clean_en_sample.wav");
    if (!wav) {
        return std::unexpected(wav.error());
    }
    auto base = base_stream(*wav);
    auto probs = probabilities_of(base);
    if (!probs) {
        return std::unexpected(probs.error());
    }
    return Inputs{.wav = std::move(*wav), .base = std::move(base), .base_probs = std::move(*probs)};
}

} // namespace

TEST(PrerollModel, PlantIsQuietAndAheadOfTheFirstOverThresholdFrame) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    const auto in = load_inputs();
    ASSERT_TRUE(in.has_value()) << in.error();
    const auto run = plant_at(in->wav, in->base, in->base_probs, kPlantGain);
    ASSERT_TRUE(run.has_value()) << run.error();
    std::cout << "PLANT " << describe(run->report) << "\n";
    EXPECT_TRUE(run->report.quiet);
    EXPECT_TRUE(run->report.near_first);
    EXPECT_TRUE(run->report.run_holds);
}

TEST(PrerollModel, QuietOnsetSurvivesForEveryOnsetFrameCount) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    const auto in = load_inputs();
    ASSERT_TRUE(in.has_value()) << in.error();
    const auto run = plant_at(in->wav, in->base, in->base_probs, kPlantGain);
    ASSERT_TRUE(run.has_value()) << run.error();

    constexpr std::array<std::uint32_t, 4> kMinSpeechMs{32, 64, 128, 250};
    const auto failing = kMinSpeechMs | std::views::filter([&](std::uint32_t ms) {
                             const auto s = survival_of(*run, in->wav, options_with(ms, 300));
                             if (!s) {
                                 std::cout << "SURVIVAL-ERROR " << s.error() << "\n";
                                 return true;
                             }
                             std::cout
                                 << "LEAD-IN onset_frames=" << s->onset_frames
                                 << " lead_in_ms=" << s->lead_in_ms
                                 << " plant_ms=192 expected_ms=320 consigliere_300ms_pad_ms=76\n";
                             return !s->starts_at_or_before_plant || !s->holds_plant ||
                                    s->lead_in_ms < kLeadInFloorMs;
                         }) |
                         std::ranges::to<std::vector>();
    EXPECT_EQ(failing, std::vector<std::uint32_t>{});
}

TEST(PrerollModel, WithoutPreRollTheOnsetIsCut) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    const auto in = load_inputs();
    ASSERT_TRUE(in.has_value()) << in.error();
    const auto run = plant_at(in->wav, in->base, in->base_probs, kPlantGain);
    ASSERT_TRUE(run.has_value()) << run.error();
    const auto s = survival_of(*run, in->wav, options_with(250, 0));
    ASSERT_TRUE(s.has_value()) << s.error();
    std::cout << "NO-PRE-ROLL onset_frames=" << s->onset_frames << " lead_in_ms=" << s->lead_in_ms
              << " starts_at_or_before_plant=" << s->starts_at_or_before_plant << "\n";
    EXPECT_FALSE(s->starts_at_or_before_plant);
    EXPECT_FALSE(s->holds_plant);
}
