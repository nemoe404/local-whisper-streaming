#include "cli/pcm_assembler.hpp"

namespace lws_cli {

std::span<const float> PcmAssembler::feed(std::span<const std::byte> /*bytes*/) {
    carry_.fill(std::byte{0});
    decoded_ = 0;
    suspicious_ = 0;
    return {};
}

bool PcmAssembler::looks_wrong() const noexcept { return false; }

} // namespace lws_cli
