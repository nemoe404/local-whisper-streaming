// The fork's Silero probabilities against the reference trace made with the ONNX model, on the
// recorded fixture: frame-by-frame difference, its effect on segment boundaries, and the verdict
// the frozen rule gives.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "fixture_io.hpp"
#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "lws/vad_windower.hpp"
#include "shell/vad_engine.hpp"

namespace {

constexpr const char* kMissingModelHint =
    "model missing: run scripts/fetch-models.sh silero-v5.1.2";

constexpr std::size_t kFrames = 336;
constexpr std::size_t kScoredSamples = kFrames * lws::kVadFrameSamples;
constexpr std::size_t kOnsetFrames = 8;
constexpr float kThreshold = 0.5F;

bool model_present() { return std::filesystem::exists(LWS_SILERO_MODEL_PATH); }

// The fork's probabilities for the first kFrames frames: one reset, then one no-reset call.
std::vector<float> fork_probabilities(lws::VadEngine& engine, std::span<const float> signal) {
    engine.reset();
    const auto probs = engine.detect(signal.first(kScoredSamples));
    return probs ? std::vector<float>(probs->begin(), probs->end()) : std::vector<float>{};
}

// One no-reset call per frame after a reset. A failed call scores NaN, which no bit comparison
// against a real run can match.
std::vector<float> fork_frame_by_frame(lws::VadEngine& engine, std::span<const float> signal) {
    engine.reset();
    return std::views::iota(std::size_t{0}, kFrames) | std::views::transform([&](std::size_t i) {
               const auto probs =
                   engine.detect(signal.subspan(i * lws::kVadFrameSamples, lws::kVadFrameSamples));
               return probs && probs->size() == 1 ? probs->front()
                                                  : std::numeric_limits<float>::quiet_NaN();
           }) |
           std::ranges::to<std::vector>();
}

std::vector<std::uint32_t> bits_of(const std::vector<float>& values) {
    return values | std::views::transform([](float v) { return std::bit_cast<std::uint32_t>(v); }) |
           std::ranges::to<std::vector>();
}

std::vector<float> abs_differences(std::span<const float> a, std::span<const float> b) {
    return std::views::zip_transform([](float x, float y) { return std::fabs(x - y); }, a, b) |
           std::ranges::to<std::vector>();
}

double mean_of(std::span<const float> values) {
    return values.empty() ? 0.0
                          : std::accumulate(values.begin(), values.end(), 0.0) /
                                static_cast<double>(values.size());
}

bool all_finite_probabilities(std::span<const float> values) {
    return std::ranges::all_of(values,
                               [](float p) { return std::isfinite(p) && p >= 0.0F && p <= 1.0F; });
}

// One closed segment reduced to what the boundary comparison reads.
struct Closed {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    lws::CloseReason reason = lws::CloseReason::Hangover;
};

// Default options, the same frames, one probability stream, then flush.
std::vector<Closed> windowed(std::span<const float> signal, std::span<const float> probs) {
    auto windower = lws::VadWindower::create(lws::Options{});
    std::vector<Closed> closed;
    if (!windower) {
        return closed;
    }
    const auto keep = [&closed](const std::optional<lws::Segment>& segment) {
        if (segment) {
            closed.push_back({.start = segment->start,
                              .end = segment->start + segment->samples.size(),
                              .reason = segment->reason});
        }
    };
    std::ranges::for_each(std::views::iota(std::size_t{0}, probs.size()), [&](std::size_t i) {
        keep(windower->feed(signal.subspan(i * lws::kVadFrameSamples, lws::kVadFrameSamples),
                            probs[i]));
    });
    keep(windower->flush());
    return closed;
}

bool above(float p) { return p >= kThreshold; }

// Frames where the two streams fall on opposite sides of the threshold.
std::vector<std::size_t> flips_of(std::span<const float> fork, std::span<const float> ref) {
    return std::views::iota(std::size_t{0}, ref.size()) |
           std::views::filter([&](std::size_t k) { return above(fork[k]) != above(ref[k]); }) |
           std::ranges::to<std::vector>();
}

// Frames where the reference crosses the threshold; the frame before the first one counts as 0.
std::vector<std::size_t> edges_of(std::span<const float> ref) {
    return std::views::iota(std::size_t{0}, ref.size()) | std::views::filter([&](std::size_t k) {
               return above(ref[k]) != (k > 0 && above(ref[k - 1]));
           }) |
           std::ranges::to<std::vector>();
}

bool near_an_edge(std::size_t flip, std::span<const std::size_t> edges) {
    return std::ranges::any_of(
        edges, [flip](std::size_t edge) { return flip + 1 >= edge && flip <= edge + 1; });
}

std::uint64_t distance(std::uint64_t a, std::uint64_t b) { return a > b ? a - b : b - a; }

// Largest pairwise difference of start (or end) over the segments both runs have.
std::uint64_t max_shift(std::span<const Closed> a, std::span<const Closed> b,
                        std::uint64_t Closed::* member) {
    const std::size_t shared = std::min(a.size(), b.size());
    const auto shifts =
        std::views::iota(std::size_t{0}, shared) |
        std::views::transform([&](std::size_t i) { return distance(a[i].*member, b[i].*member); });
    return std::ranges::fold_left(shifts, std::uint64_t{0},
                                  [](std::uint64_t x, std::uint64_t y) { return std::max(x, y); });
}

bool same_reasons(std::span<const Closed> a, std::span<const Closed> b) {
    return a.size() == b.size() && std::ranges::equal(a, b, {}, &Closed::reason, &Closed::reason);
}

// Everything the frozen rule and the record need, from one set of three probability streams.
struct Measured {
    double max_abs = 0.0;
    std::size_t argmax = 0;
    double mean_abs = 0.0;
    double onset8_max = 0.0;
    double onset8_mean = 0.0;
    std::size_t flips = 0;
    std::size_t flips_off_edge = 0;
    std::size_t segments_fork = 0;
    std::size_t segments_ref = 0;
    std::uint64_t start_shift_samples = 0;
    std::uint64_t end_shift_samples = 0;
    bool reasons_equal = false;
    double attribution_max_abs = 0.0;
    double attribution_mean_abs = 0.0;
    std::vector<std::size_t> flip_frames;
    std::vector<bool> flip_near_edge;
};

// The boundary part of the rule: same segments, same reasons, ends within one frame.
[[nodiscard]] bool boundaries_hold(const Measured& m) {
    return m.segments_fork == m.segments_ref && m.reasons_equal &&
           m.start_shift_samples <= lws::kVadFrameSamples &&
           m.end_shift_samples <= lws::kVadFrameSamples;
}

// The flip part of the rule: every threshold flip sits within one frame of a reference edge.
[[nodiscard]] bool flips_hold(const Measured& m) { return m.flips_off_edge == 0; }

[[nodiscard]] bool accepted(const Measured& m) { return boundaries_hold(m) && flips_hold(m); }

Measured measure(std::span<const float> signal, std::span<const float> fork,
                 std::span<const float> ref, std::span<const float> reflect) {
    Measured m;
    const auto diffs = abs_differences(fork, ref);
    const auto peak = std::ranges::max_element(diffs);
    m.max_abs = static_cast<double>(*peak);
    m.argmax = static_cast<std::size_t>(peak - diffs.begin());
    m.mean_abs = mean_of(diffs);
    const std::span<const float> onset(diffs.data(), std::min(kOnsetFrames, diffs.size()));
    m.onset8_max = static_cast<double>(*std::ranges::max_element(onset));
    m.onset8_mean = mean_of(onset);

    m.flip_frames = flips_of(fork, ref);
    const auto edges = edges_of(ref);
    m.flip_near_edge =
        m.flip_frames |
        std::views::transform([&](std::size_t k) { return near_an_edge(k, edges); }) |
        std::ranges::to<std::vector>();
    m.flips = m.flip_frames.size();
    m.flips_off_edge = static_cast<std::size_t>(std::ranges::count(m.flip_near_edge, false));

    const auto from_fork = windowed(signal, fork);
    const auto from_ref = windowed(signal, ref);
    m.segments_fork = from_fork.size();
    m.segments_ref = from_ref.size();
    m.start_shift_samples = max_shift(from_fork, from_ref, &Closed::start);
    m.end_shift_samples = max_shift(from_fork, from_ref, &Closed::end);
    m.reasons_equal = same_reasons(from_fork, from_ref);

    const auto attribution = abs_differences(fork, reflect);
    m.attribution_max_abs = static_cast<double>(*std::ranges::max_element(attribution));
    m.attribution_mean_abs = mean_of(attribution);
    return m;
}

std::string nctx_line(const Measured& m) {
    std::ostringstream out;
    out.precision(9);
    out << "NCTX frames=" << kFrames << " max_abs=" << m.max_abs << " argmax=" << m.argmax
        << " mean_abs=" << m.mean_abs << " onset8_max=" << m.onset8_max
        << " onset8_mean=" << m.onset8_mean << " flips=" << m.flips
        << " flips_off_edge=" << m.flips_off_edge << " segments_fork=" << m.segments_fork
        << " segments_ref=" << m.segments_ref
        << " start_shift_frames=" << m.start_shift_samples / lws::kVadFrameSamples
        << " end_shift_frames=" << m.end_shift_samples / lws::kVadFrameSamples
        << " reasons_equal=" << (m.reasons_equal ? 1 : 0)
        << " attribution_max_abs=" << m.attribution_max_abs
        << " attribution_mean_abs=" << m.attribution_mean_abs
        << " verdict=" << (accepted(m) ? "accept" : "fork-commit");
    return out.str();
}

// What every test here reads: the signal, the fork and the two reference traces.
struct Streams {
    std::vector<float> signal;
    std::vector<float> fork;
    std::vector<float> ref;
    std::vector<float> reflect;
};

std::expected<Streams, std::string> load_streams(lws::VadEngine& engine) {
    auto wav = lws::test::load_fixture_wav("clean_en_sample.wav");
    if (!wav) {
        return std::unexpected(wav.error());
    }
    auto ref = lws::test::load_probability_trace("clean_en_sample.silero-ref.txt");
    if (!ref) {
        return std::unexpected(ref.error());
    }
    auto reflect = lws::test::load_probability_trace("clean_en_sample.silero-ref-reflect.txt");
    if (!reflect) {
        return std::unexpected(reflect.error());
    }
    Streams streams{.signal = std::move(*wav),
                    .fork = {},
                    .ref = std::move(*ref),
                    .reflect = std::move(*reflect)};
    streams.fork = fork_probabilities(engine, streams.signal);
    return streams;
}

// Empty when every stream holds kFrames finite probabilities in [0, 1]; otherwise names the one
// that does not.
std::string integrity_failure(const Streams& streams) {
    const auto bad = [](const std::vector<float>& values) {
        return values.size() != kFrames || !all_finite_probabilities(values);
    };
    if (bad(streams.fork)) {
        return "fork probabilities";
    }
    if (bad(streams.ref)) {
        return "reference probabilities";
    }
    return bad(streams.reflect) ? "reflect probabilities" : "";
}

void print_measurement(const Streams& streams, const Measured& m) {
    std::cout << nctx_line(m) << '\n';
    std::ranges::for_each(std::views::iota(std::size_t{0}, m.flips), [&](std::size_t i) {
        const std::size_t k = m.flip_frames.at(i);
        std::cout << "NCTX-FLIP frame=" << k << " p_fork=" << streams.fork.at(k)
                  << " p_ref=" << streams.ref.at(k)
                  << " near_edge=" << (m.flip_near_edge.at(i) ? 1 : 0) << '\n';
    });
}

} // namespace

TEST(VadReferenceModel, FrameCountsMatch) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    const auto wav = lws::test::load_fixture_wav("clean_en_sample.wav");
    ASSERT_TRUE(wav.has_value()) << wav.error();
    ASSERT_EQ(wav->size(), 172320U);
    const auto reference = lws::test::load_probability_trace("clean_en_sample.silero-ref.txt");
    ASSERT_TRUE(reference.has_value()) << reference.error();
    ASSERT_EQ(reference->size(), kFrames);

    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    ASSERT_TRUE(engine.has_value());
    const auto fork = fork_probabilities(*engine, *wav);
    ASSERT_EQ(fork.size(), kFrames);

    const auto diffs = abs_differences(fork, *reference);
    std::cout << "NCTX-TRACER frames=" << fork.size()
              << " max_abs=" << *std::ranges::max_element(diffs) << " mean_abs=" << mean_of(diffs)
              << '\n';
}

TEST(VadReferenceModel, ChunkedNoResetEqualsOneCall) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    const auto wav = lws::test::load_fixture_wav("clean_en_sample.wav");
    ASSERT_TRUE(wav.has_value()) << wav.error();
    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    ASSERT_TRUE(engine.has_value());

    const auto one_call = fork_probabilities(*engine, *wav);
    const auto chunked = fork_frame_by_frame(*engine, *wav);

    ASSERT_EQ(one_call.size(), kFrames);
    ASSERT_EQ(chunked.size(), kFrames);
    EXPECT_TRUE(all_finite_probabilities(chunked));
    EXPECT_EQ(bits_of(one_call), bits_of(chunked));
}

TEST(VadReferenceModel, Measure) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    ASSERT_TRUE(engine.has_value());
    const auto streams = load_streams(*engine);
    ASSERT_TRUE(streams.has_value()) << streams.error();
    ASSERT_EQ(integrity_failure(*streams), "");

    const Measured m = measure(streams->signal, streams->fork, streams->ref, streams->reflect);
    print_measurement(*streams, m);
}

// Pins the accepted outcome: the fork's reflect padding stays while its boundaries and threshold
// flips remain within one frame of the reference. A change to the fork, the model or the reference
// trace that breaks either half reddens this test on every leg that runs the real model.
TEST(VadReferenceModel, AcceptedBoundsHold) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    ASSERT_TRUE(engine.has_value());
    const auto streams = load_streams(*engine);
    ASSERT_TRUE(streams.has_value()) << streams.error();
    ASSERT_EQ(integrity_failure(*streams), "");

    const Measured m = measure(streams->signal, streams->fork, streams->ref, streams->reflect);
    std::cout << nctx_line(m) << '\n';

    EXPECT_TRUE(boundaries_hold(m));
    EXPECT_TRUE(flips_hold(m));
}
