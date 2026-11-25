#include "cli/pace.hpp"

namespace lws_cli {

std::chrono::nanoseconds pace_due(std::uint64_t /*samples_pushed*/, double /*speed*/) noexcept {
    return std::chrono::nanoseconds::max();
}

} // namespace lws_cli
