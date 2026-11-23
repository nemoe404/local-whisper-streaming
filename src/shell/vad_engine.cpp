#include "shell/vad_engine.hpp"

#include <whisper.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "shell/whisper_log.hpp"

namespace lws {

namespace {

struct VadContextDeleter {
    void operator()(whisper_vad_context* ctx) const noexcept { whisper_vad_free(ctx); }
};

// Both closures of an engine share one owner, so moving the engine or either closure can never
// leave the other holding a freed context.
using SharedVadContext = std::shared_ptr<whisper_vad_context>;

std::expected<std::span<const float>, Error> detect_no_reset(whisper_vad_context& ctx,
                                                             std::span<const float> samples) {
    // The sample count crosses the C boundary as int.
    if (samples.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(Error::VadFailed);
    }
    if (!whisper_vad_detect_speech_no_reset(&ctx, samples.data(),
                                            static_cast<int>(samples.size()))) {
        return std::unexpected(Error::VadFailed);
    }
    // whisper.cpp replaces this buffer on every call, so read it now.
    const int count = whisper_vad_n_probs(&ctx);
    const float* probs = whisper_vad_probs(&ctx);
    if (count < 0 || (count > 0 && probs == nullptr)) {
        return std::unexpected(Error::VadFailed);
    }
    return std::span<const float>(probs, static_cast<std::size_t>(count));
}

} // namespace

whisper_vad_context_params vad_context_params() noexcept {
    auto params = whisper_vad_default_context_params();
    // CPU and one thread in every build, CUDA included: Silero is tiny, and a GPU round trip per
    // 32 ms frame costs more than it saves.
    params.n_threads = 1;
    params.use_gpu = false;
    params.gpu_device = 0;
    return params;
}

std::expected<VadEngine, Error> VadEngine::open(const std::filesystem::path& model) {
    // First, so the loader's own lines are routed too.
    install_whisper_log_route();

    const std::string path = model.string();
    whisper_vad_context* raw =
        whisper_vad_init_from_file_with_params(path.c_str(), vad_context_params());
    if (raw == nullptr) {
        return std::unexpected(Error::ModelLoad);
    }
    SharedVadContext ctx{raw, VadContextDeleter{}};

    return VadEngine{
        .detect = [ctx](std::span<const float> samples) { return detect_no_reset(*ctx, samples); },
        .reset = [ctx = std::move(ctx)] { whisper_vad_reset_state(ctx.get()); },
    };
}

} // namespace lws
