#include "cli/args.hpp"

namespace lws_cli {

std::expected<Command, UsageError> parse_args(std::span<const std::string_view> /*args*/) {
    return std::unexpected(UsageError{.flag = "", .reason = "not implemented"});
}

std::string usage_text() { return {}; }

} // namespace lws_cli
