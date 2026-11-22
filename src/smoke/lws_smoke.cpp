#include "smoke/lws_smoke.hpp"

#include <whisper.h>

namespace lws::smoke {

std::string_view whisper_version_string() noexcept { return ::whisper_version(); }

} // namespace lws::smoke
