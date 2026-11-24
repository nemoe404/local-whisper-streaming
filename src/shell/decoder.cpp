#include "shell/decoder.hpp"

#include <whisper.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "shell/whisper_log.hpp"

namespace lws {

namespace {

struct ContextDeleter {
    void operator()(whisper_context* ctx) const noexcept { whisper_free(ctx); }
};

// Both closures of a backend share one owner, so moving the decoder or either closure can never
// leave the other holding a freed context.
using SharedContext = std::shared_ptr<whisper_context>;

std::vector<RawSpan> read_spans(whisper_context& ctx) {
    const int count = whisper_full_n_segments(&ctx);
    return std::views::iota(0, count) | std::views::transform([&ctx](int i) {
               const char* text = whisper_full_get_segment_text(&ctx, i);
               return RawSpan{.t0_cs = whisper_full_get_segment_t0(&ctx, i),
                              .t1_cs = whisper_full_get_segment_t1(&ctx, i),
                              .text = text == nullptr ? std::string{} : std::string{text}};
           }) |
           std::ranges::to<std::vector>();
}

std::int32_t run_whisper(whisper_context& ctx, const DecoderOptions& options,
                         std::span<const float> samples, std::stop_token stop) {
    AbortHook hook{.token = std::move(stop)};
    return whisper_full(&ctx, build_decode_params(options, hook), samples.data(),
                        static_cast<int>(samples.size()));
}

} // namespace

Decoder::Decoder(DecodeBackend backend) noexcept : backend_(std::move(backend)) {
    assert(backend_.run && backend_.spans);
}

std::expected<DecodeResult, Error> Decoder::decode(const Segment& segment,
                                                   const std::stop_token& stop) {
    const std::size_t n_samples = segment.samples.size();
    if (!is_decodable(n_samples)) {
        return DecodeResult{.status = DecodeStatus::TooShort, .spans = {}};
    }
    // The sample count crosses the C boundary as int.
    if (n_samples > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(Error::DecodeFailed);
    }
    ++calls_;
    const std::int32_t rc = backend_.run(segment.samples, stop);
    switch (classify_run(rc, stop.stop_requested())) {
    case RunOutcome::Ok: {
        const std::vector<RawSpan> raw = backend_.spans();
        return DecodeResult{.status = DecodeStatus::Decoded, .spans = map_spans(raw, n_samples)};
    }
    case RunOutcome::Cancelled:
        return DecodeResult{.status = DecodeStatus::Cancelled, .spans = {}};
    case RunOutcome::Failed:
        break;
    }
    return std::unexpected(Error::DecodeFailed);
}

std::uint64_t Decoder::whisper_calls() const noexcept { return calls_; }

// whisper's abort callback type takes a non-const void*, so the parameter cannot be const.
// cppcheck-suppress constParameterCallback ; the pointer type is fixed by whisper_full_params
bool abort_poll(void* hook) noexcept {
    return static_cast<const AbortHook*>(hook)->token.stop_requested();
}

whisper_full_params build_decode_params(const DecoderOptions& options, AbortHook& hook) noexcept {
    auto params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads = options.n_threads;
    // An explicit language skips whisper's language-detect encoder pass.
    params.language = options.language.c_str();
    params.detect_language = false;
    params.translate = false;
    // Whisper retries a window at a higher temperature by itself; the step is the caller's.
    params.temperature_inc = options.temperature_inc;
    // Speech detection is ours alone: whisper's own VAD stays off.
    params.vad = false;
    params.vad_model_path = nullptr;
    // Text from one segment stays out of the next.
    params.no_context = true;
    // single_segment would stretch a trailing segment to the 30 s window.
    params.single_segment = false;
    params.offset_ms = 0;
    params.duration_ms = 0;
    params.token_timestamps = false;
    params.tdrz_enable = false;
    // print_realtime would write to stdout.
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.print_special = false;
    params.abort_callback = &abort_poll;
    params.abort_callback_user_data = &hook;
    return params;
}

std::expected<Decoder, Error> Decoder::open(const std::filesystem::path& model,
                                            const DecoderOptions& options) {
    if (const auto valid = validate(options); !valid) {
        return std::unexpected(valid.error());
    }
    // Before the load, so the loader's own lines are routed too.
    install_whisper_log_route();

    auto context_params = whisper_context_default_params();
    context_params.use_gpu = options.use_gpu;
    const std::string path = model.string();
    whisper_context* raw = whisper_init_from_file_with_params(path.c_str(), context_params);
    if (raw == nullptr) {
        return std::unexpected(Error::ModelLoad);
    }
    SharedContext ctx{raw, ContextDeleter{}};

    return Decoder{DecodeBackend{
        .run =
            [ctx, options](std::span<const float> samples, std::stop_token stop) {
                return run_whisper(*ctx, options, samples, std::move(stop));
            },
        .spans = [ctx = std::move(ctx)] { return read_spans(*ctx); },
    }};
}

} // namespace lws
