#include "lws/streamer_options.hpp"

#include <chrono>
#include <expected>

#include "lws/decode_policy.hpp"
#include "lws/options.hpp"

namespace lws {

std::expected<StreamerOptions, Error> validate(const StreamerOptions& options) {
    if (const auto vad = validate(options.vad); !vad) {
        return std::unexpected(vad.error());
    }
    if (const auto decoder = validate(options.decoder); !decoder) {
        return std::unexpected(decoder.error());
    }
    const bool poll_usable =
        options.poll >= std::chrono::milliseconds{1} && options.poll <= std::chrono::seconds{1};
    const bool fields_hold = options.ring_capacity_samples >= kVadFrameSamples &&
                             options.queue_segments >= 1 && poll_usable;
    if (!fields_hold) {
        return std::unexpected(Error::InvalidStreamerOptions);
    }
    return options;
}

} // namespace lws
