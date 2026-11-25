#include "cli/event_format.hpp"

namespace lws_cli {

std::string format_stream_time(std::uint64_t /*samples*/) { return {}; }

std::string format_event(const lws::TextEvent& /*event*/) { return {}; }

bool goes_to_stdout(lws::EventKind /*kind*/) noexcept { return false; }

} // namespace lws_cli
