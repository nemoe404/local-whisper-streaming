#pragma once

#include "cli/run.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <variant>

// The command line of lws-cli as a pure value: arguments in, a command or a usage error out. No
// I/O, so every refusal is table-tested without a process.
namespace lws_cli {

// `--help` asks for the usage text.
struct ShowHelp {};

using Command = std::variant<RunConfig, ShowHelp>;

// The flag the problem is about and a plain-words reason. An empty flag means the problem is not
// tied to one option.
struct UsageError {
    std::string flag;
    std::string reason;
};

// `args` excludes the program name. Unknown, repeated and value-less flags are refused, and a
// number must be a whole number or decimal with nothing after it.
[[nodiscard]] std::expected<Command, UsageError> parse_args(std::span<const std::string_view> args);

// One line per flag with its default, what stdin must hold, and the exit codes.
[[nodiscard]] std::string usage_text();

} // namespace lws_cli
