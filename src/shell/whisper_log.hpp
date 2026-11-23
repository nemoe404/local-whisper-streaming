#pragma once

#include <cstdint>

// Shell header, no whisper header. whisper.cpp and ggml log every detect call at info level (about
// four lines per 32 ms frame), which would flood stderr on a live stream. The route below drops and
// counts debug and info lines and passes warnings and errors through.
namespace lws {

// Points whisper_log_set (and through it ggml's log) at the lws route. This writes a process-wide
// hook with no synchronisation: install it before any second thread uses whisper.
void install_whisper_log_route() noexcept;

// Lines the route has dropped since process start.
[[nodiscard]] std::uint64_t whisper_log_lines_dropped() noexcept;

} // namespace lws
