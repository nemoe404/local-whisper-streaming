#pragma once

#include <string>
#include <string_view>

namespace lws {

// previous: text of the segment already emitted. continuation: text of the next segment.
// Returns the continuation with its duplicated leading words (1 to 4 words, normalised) removed.
// Original casing and inner punctuation of the kept words are preserved. Pure: no I/O, no threads.
[[nodiscard]] std::string seam_merge(std::string_view previous, std::string_view continuation);

} // namespace lws
