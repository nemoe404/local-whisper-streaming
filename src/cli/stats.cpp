#include "cli/stats.hpp"

namespace lws_cli {

std::optional<LagSummary> lag_summary(std::span<const PushMark> /*marks*/,
                                      std::span<const Delivery> /*deliveries*/) {
    return std::nullopt;
}

std::string format_stats(const StatsLine& /*line*/) { return {}; }

} // namespace lws_cli
