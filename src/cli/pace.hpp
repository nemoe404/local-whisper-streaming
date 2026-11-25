#pragma once

#include <chrono>
#include <cstdint>

// The schedule for pushing a recorded stream at a multiple of real time. Pure values: no clock.
namespace lws_cli {

// The offset from the first push at which `samples_pushed` 16 kHz samples may have been pushed, at
// `speed` times real time: samples * 10^9 / (16000 * speed) nanoseconds, rounded to the nearest
// nanosecond. Zero when speed is 0, which means no pacing. The caller guarantees that speed is
// finite and not negative.
[[nodiscard]] std::chrono::nanoseconds pace_due(std::uint64_t samples_pushed,
                                                double speed) noexcept;

} // namespace lws_cli
