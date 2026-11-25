#pragma once

#include "lws/options.hpp"

#include <string_view>

// The words and the process exit code for each library error. Pure values: no I/O.
namespace lws_cli {

struct ErrorText {
    std::string_view message;
    // 2 when the user can fix the call by changing an option, 1 for everything else.
    int exit_code;
};

// The message names no file path: the caller knows which path it was loading and adds it.
[[nodiscard]] ErrorText error_text(lws::Error error) noexcept;

} // namespace lws_cli
