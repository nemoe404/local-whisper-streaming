#pragma once

#include <string_view>

namespace lws::smoke {

// Version string of the linked whisper.cpp fork. Static storage, never null.
// No whisper type crosses this header: later public headers follow the same shape.
[[nodiscard]] std::string_view whisper_version_string() noexcept;

} // namespace lws::smoke
