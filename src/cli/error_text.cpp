#include "cli/error_text.hpp"

namespace lws_cli {

ErrorText error_text(lws::Error /*error*/) noexcept { return {}; }

} // namespace lws_cli
