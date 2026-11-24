// An abort leaves the segments of earlier windows in whisper's result list, so the Decoder must
// classify the return before it reads anything. This test runs a real whisper context over an
// input of several windows and raises the stop at the start of the second window. whisper then
// returns non-zero while the first window's text is still in its list, and the Decoder must hand
// back a cancelled outcome with no text and without ever reading that list.
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <fmt/base.h>
#include <whisper.h>

#include <gtest/gtest.h>

#include "decode/decode_support.hpp"
#include "lws/decode_policy.hpp"
#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "shell/decoder.hpp"
#include "shell/whisper_log.hpp"

namespace {

// The window at whose start the stop is raised.
constexpr std::int32_t kStopWindow = 2;

struct ContextFree {
    void operator()(whisper_context* ctx) const noexcept { whisper_free(ctx); }
};

// What the encoder callback sees: the windows begun so far and the source it raises the stop on.
struct WindowProbe {
    std::int32_t windows = 0;
    const std::stop_source* source = nullptr;
};

// Everything the test reads after the decode has returned.
struct AbortRun {
    std::expected<lws::DecodeResult, lws::Error> decoded;
    std::uint64_t calls = 0;
    std::int32_t windows = 0;
    std::int32_t rc = 0;
    std::int32_t spans_reads = 0;
    std::int32_t segments_left = 0;
};

bool count_window_and_stop(whisper_context* /*ctx*/, whisper_state* /*state*/, void* user_data) {
    auto* probe = static_cast<WindowProbe*>(user_data);
    ++probe->windows;
    if (probe->windows == kStopWindow) {
        probe->source->request_stop();
    }
    // True means continue: the abort callback is what stops whisper.
    return true;
}

// The segments in whisper's list, read the way the production reader reads them.
std::vector<lws::RawSpan> read_all_segments(whisper_context& ctx) {
    const int count = whisper_full_n_segments(&ctx);
    return std::views::iota(0, count) | std::views::transform([&ctx](int i) {
               const char* text = whisper_full_get_segment_text(&ctx, i);
               return lws::RawSpan{.t0_cs = whisper_full_get_segment_t0(&ctx, i),
                                   .t1_cs = whisper_full_get_segment_t1(&ctx, i),
                                   .text = text == nullptr ? std::string{} : std::string{text}};
           }) |
           std::ranges::to<std::vector>();
}

std::expected<AbortRun, std::string> decode_with_stop_in_second_window() {
    const auto clip = lws::test::load_fixture_wav("clean_en_sample.wav");
    if (!clip) {
        return std::unexpected(clip.error());
    }
    const lws::Segment segment{.samples = lws::test::three_copies(*clip)};

    lws::install_whisper_log_route();
    const std::shared_ptr<whisper_context> ctx{
        whisper_init_from_file_with_params(LWS_WHISPER_MODEL_PATH,
                                           whisper_context_default_params()),
        ContextFree{}};
    if (!ctx) {
        return std::unexpected("model load failed");
    }

    const std::stop_source source;
    WindowProbe probe{.windows = 0, .source = &source};
    AbortRun out;
    lws::Decoder decoder{lws::DecodeBackend{
        .run =
            [ctx, options = lws::DecoderOptions{}, &probe, &out](std::span<const float> samples,
                                                                 std::stop_token stop) {
                lws::AbortHook hook{.token = std::move(stop)};
                whisper_full_params params = lws::build_decode_params(options, hook);
                params.encoder_begin_callback = &count_window_and_stop;
                params.encoder_begin_callback_user_data = &probe;
                out.rc = whisper_full(ctx.get(), params, samples.data(),
                                      static_cast<int>(samples.size()));
                return out.rc;
            },
        .spans =
            [ctx, &out] {
                ++out.spans_reads;
                return read_all_segments(*ctx);
            },
    }};

    out.decoded = decoder.decode(segment, source.get_token());
    out.calls = decoder.whisper_calls();
    out.windows = probe.windows;
    out.segments_left = whisper_full_n_segments(ctx.get());
    return out;
}

} // namespace

TEST(WhisperAbortModel, StopInTheSecondWindowLeavesPartialSegmentsTheDecoderDiscards) {
    const auto run = decode_with_stop_in_second_window();
    ASSERT_TRUE(run.has_value()) << run.error();
    fmt::println("PARTIAL-ABORT model={} rc={} segments_left={}", LWS_WHISPER_MODEL_NAME, run->rc,
                 run->segments_left);

    // The preconditions that make the test mean something: whisper failed in the second window and
    // the first window's text is still in its list.
    EXPECT_EQ(run->windows, kStopWindow);
    EXPECT_NE(run->rc, 0);
    EXPECT_GE(run->segments_left, 1);

    ASSERT_TRUE(run->decoded.has_value());
    EXPECT_EQ(run->decoded->status, lws::DecodeStatus::Cancelled);
    EXPECT_TRUE(run->decoded->spans.empty());
    EXPECT_EQ(run->calls, 1U);
    EXPECT_EQ(run->spans_reads, 0);
}
