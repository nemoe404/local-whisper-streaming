#pragma once

#include "lws/text_event.hpp"

#include <cstdint>
#include <string>

// The text of every event line the command-line tool prints. Transcript lines go to stdout, loss
// lines to stderr, and a loss line never carries transcript text. Pure values: no I/O.
namespace lws_cli {

// `HH:MM:SS.mmm` for a count of 16 kHz samples. Milliseconds are samples / 16 in integer
// arithmetic. The hours field has at least two digits and grows past 99.
[[nodiscard]] std::string format_stream_time(std::uint64_t samples);

// One newline-terminated line. Text reads `[<start> --> <end>]  <text>` with the text's leading
// whitespace trimmed. A loss reads `[<start> --> <end>]  <what was lost>`.
[[nodiscard]] std::string format_event(const lws::TextEvent& event);

// True for Text only: the one kind that belongs on stdout.
[[nodiscard]] bool goes_to_stdout(lws::EventKind kind) noexcept;

} // namespace lws_cli
