#include "lws/options.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>

namespace lws {

std::expected<Options, Error> validate(const Options& options) {
    // Positive predicates, negated once, so NaN fails the check. Written as a negated
    // conjunction on purpose: De Morgan's rewrite (`<= 0 || >= 1`) would let NaN through.
    const bool threshold_inside = options.threshold > 0.0F && options.threshold < 1.0F;
    if (!threshold_inside) {
        return std::unexpected(Error::InvalidThreshold);
    }
    const bool ceiling_positive_finite =
        options.max_speech_duration_s > 0.0F && std::isfinite(options.max_speech_duration_s);
    if (!ceiling_positive_finite) {
        return std::unexpected(Error::InvalidDuration);
    }
    if (options.max_speech_duration_s > 25.0F) {
        return std::unexpected(Error::CeilingTooLong);
    }

    // Convert to integers only after the checks above.
    const auto ceiling = static_cast<std::uint64_t>(
        std::ceil(static_cast<double>(options.max_speech_duration_s) * 1000.0 / 32.0));
    const std::uint64_t pre = ms_to_frames_ceil(options.pre_roll_ms);
    const std::uint64_t onset =
        std::max<std::uint64_t>(1U, ms_to_frames_ceil(options.min_speech_duration_ms));
    if (ceiling <= pre + onset) {
        return std::unexpected(Error::CeilingTooShort);
    }
    return options;
}

bool seam_merge_enabled(const Options& options) noexcept { return options.overlap_ms > 0; }

} // namespace lws
