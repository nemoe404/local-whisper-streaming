#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Turns a byte stream of little-endian 32-bit floats into whole samples. A read from a pipe can end
// inside a float, so the assembler keeps the 0 to 3 bytes of an unfinished float until the next
// feed. A pure value: no I/O.
namespace lws_cli {

class PcmAssembler {
public:
    // Returns every float completed by `bytes` and the bytes carried from earlier feeds. A sample
    // that is not finite comes back as 0.0F and is counted. The span stays valid until the next
    // feed.
    [[nodiscard]] std::span<const float> feed(std::span<const std::byte> bytes);

    // Bytes of an unfinished float, 0 to 3.
    [[nodiscard]] std::size_t pending_bytes() const noexcept { return carry_length_; }

    // How many non-finite samples were replaced with 0.0F so far.
    [[nodiscard]] std::uint64_t replaced_samples() const noexcept { return replaced_; }

    // True once more than 160 of the first 16000 samples were non-finite or louder than 1.0: the
    // input does not look like 16 kHz mono f32le.
    [[nodiscard]] bool looks_wrong() const noexcept;

private:
    std::vector<float> samples_;
    std::array<std::byte, 3> carry_{};
    std::size_t carry_length_ = 0;
    std::uint64_t decoded_ = 0;
    std::uint64_t replaced_ = 0;
    std::uint64_t suspicious_ = 0;
};

} // namespace lws_cli
