#include "shell/whisper_log.hpp"

#include <whisper.h>

#include <atomic>
#include <cstdint>

#include <fmt/format.h>

namespace lws {

namespace {

std::atomic<std::uint64_t>& dropped_lines() noexcept {
    static std::atomic<std::uint64_t> count{0};
    return count;
}

void count_dropped() noexcept { dropped_lines().fetch_add(1, std::memory_order_relaxed); }

// A continuation line inherits the level of the line it continues, which is the last level this
// thread logged: a dropped line's continuation is dropped too, a written line's continuation is
// written.
void route_line(ggml_log_level level, const char* text, void* /*user_data*/) noexcept {
    thread_local ggml_log_level last_level = GGML_LOG_LEVEL_NONE;
    const bool continuation = level == GGML_LOG_LEVEL_CONT;
    const ggml_log_level effective = continuation ? last_level : level;
    if (!continuation) {
        last_level = level;
    }
    if (text == nullptr || effective < GGML_LOG_LEVEL_WARN) {
        count_dropped();
        return;
    }
    // This is a C callback: nothing may propagate out of it. A failed write counts as dropped.
    try {
        if (continuation) {
            fmt::print(stderr, "{}", text);
        } else {
            fmt::print(stderr, "lws: whisper: {}", text);
        }
    } catch (...) {
        count_dropped();
    }
}

} // namespace

void install_whisper_log_route() noexcept { whisper_log_set(route_line, nullptr); }

std::uint64_t whisper_log_lines_dropped() noexcept {
    return dropped_lines().load(std::memory_order_relaxed);
}

} // namespace lws
