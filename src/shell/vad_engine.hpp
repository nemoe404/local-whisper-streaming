#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <span>

#include "lws/options.hpp"

// Shell header: the VAD engine adapter over whisper.cpp's Silero API. It includes no whisper
// header, so code that only injects or fakes an engine never pulls whisper or ggml into its binary.
// An engine's two members are used together and are never empty.

// whisper.cpp's C struct. It is complete only where whisper.h is included.
struct whisper_vad_context_params;

namespace lws {

// Scores whole kVadFrameSamples chunks: one probability per chunk. The returned span is borrowed
// from the engine and valid only until the next call, so a caller copies what it keeps before
// calling again.
using VadDetectFn =
    std::move_only_function<std::expected<std::span<const float>, Error>(std::span<const float>)>;

// Clears the state the model carries between detect calls.
using VadResetFn = std::move_only_function<void()>;

struct VadEngine {
    VadDetectFn detect;
    VadResetFn reset;

    // Loads the model on the CPU with one thread. Error::ModelLoad if it cannot be loaded.
    [[nodiscard]] static std::expected<VadEngine, Error> open(const std::filesystem::path& model);
};

// use_gpu=false, n_threads=1 and gpu_device=0 on top of whisper's defaults. The values do not
// depend on the build: a CUDA build gets the same ones.
[[nodiscard]] whisper_vad_context_params vad_context_params() noexcept;

// detect(512 zeros) must give 1 probability and detect(513 zeros) 2, otherwise
// Error::FrameSizeMismatch. A detect error is returned unchanged. Never resets: the caller's
// session-start reset follows.
[[nodiscard]] std::expected<void, Error> probe_frame_size(VadEngine& engine);

} // namespace lws
