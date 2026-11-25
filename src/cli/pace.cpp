#include "cli/pace.hpp"

#include "lws/options.hpp"

#include <cassert>
#include <cmath>

namespace lws_cli {

std::chrono::nanoseconds pace_due(std::uint64_t samples_pushed, double speed) noexcept {
    assert(std::isfinite(speed) && speed >= 0.0);
    if (speed == 0.0) {
        return std::chrono::nanoseconds{0};
    }
    // long double has a 64-bit mantissa on this target, so a sample count of hundreds of hours
    // times 10^9 is exact before the one division, and the result is correctly rounded.
    constexpr long double kNanosPerSecond = 1.0e9L;
    const long double samples_per_second =
        static_cast<long double>(lws::kSampleRateHz) * static_cast<long double>(speed);
    const long double nanos =
        static_cast<long double>(samples_pushed) * kNanosPerSecond / samples_per_second;
    return std::chrono::nanoseconds{static_cast<std::int64_t>(std::llroundl(nanos))};
}

} // namespace lws_cli
