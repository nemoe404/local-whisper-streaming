#include "cli/pcm_assembler.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <ranges>

// A big-endian or non-IEEE build would print noise, so it does not compile.
static_assert(std::endian::native == std::endian::little);
static_assert(std::numeric_limits<float>::is_iec559);

namespace lws_cli {

namespace {

constexpr std::size_t kFloatBytes = sizeof(float);
// The input check looks at the first second only: 16 kHz samples.
constexpr std::uint64_t kCheckedSamples = 16000;
// More than one percent of the checked samples being wrong means the format is wrong.
constexpr std::uint64_t kSuspiciousLimit = 160;

float decode(std::span<const std::byte, kFloatBytes> word) noexcept {
    float value = 0.0F;
    std::memcpy(&value, word.data(), kFloatBytes);
    return value;
}

bool is_suspicious(float value) noexcept {
    return !std::isfinite(value) || std::fabs(value) > 1.0F;
}

} // namespace

std::span<const float> PcmAssembler::feed(std::span<const std::byte> bytes) {
    samples_.clear();

    // Finish the float left unfinished by the previous feed, if this feed has the rest of it.
    if (carry_length_ > 0) {
        const std::size_t take = std::min(kFloatBytes - carry_length_, bytes.size());
        std::array<std::byte, kFloatBytes> word{};
        const auto carried = std::ranges::copy_n(
            carry_.begin(), static_cast<std::ptrdiff_t>(carry_length_), word.begin());
        std::ranges::copy_n(bytes.begin(), static_cast<std::ptrdiff_t>(take), carried.out);
        if (carry_length_ + take == kFloatBytes) {
            samples_.push_back(decode(word));
            carry_length_ = 0;
        } else {
            // Still short of a whole float: keep every byte seen so far.
            carry_length_ += take;
            std::ranges::copy_n(word.begin(), static_cast<std::ptrdiff_t>(carry_length_),
                                carry_.begin());
        }
        bytes = bytes.subspan(take);
    }

    const std::size_t whole = bytes.size() / kFloatBytes;
    samples_.reserve(samples_.size() + whole);
    std::ranges::transform(std::views::iota(std::size_t{0}, whole), std::back_inserter(samples_),
                           [&](std::size_t index) {
                               return decode(
                                   bytes.subspan(index * kFloatBytes).first<kFloatBytes>());
                           });

    // Keep the 0 to 3 bytes of the next float. carry_length_ is 0 here unless this feed was too
    // short to finish the carried float, in which case there are no bytes left to keep.
    const auto leftover = bytes.subspan(whole * kFloatBytes);
    if (!leftover.empty()) {
        std::ranges::copy(leftover, carry_.begin());
        carry_length_ = leftover.size();
    }

    // Count what looks wrong in the first second before the bad samples are replaced.
    const std::uint64_t unchecked = decoded_ < kCheckedSamples ? kCheckedSamples - decoded_ : 0;
    const auto checked = std::span<const float>{samples_}.first(
        static_cast<std::size_t>(std::min<std::uint64_t>(unchecked, samples_.size())));
    suspicious_ += static_cast<std::uint64_t>(std::ranges::count_if(checked, is_suspicious));

    const auto not_finite = [](float value) { return !std::isfinite(value); };
    replaced_ += static_cast<std::uint64_t>(std::ranges::count_if(samples_, not_finite));
    std::ranges::replace_if(samples_, not_finite, 0.0F);
    decoded_ += samples_.size();
    return samples_;
}

bool PcmAssembler::looks_wrong() const noexcept { return suspicious_ > kSuspiciousLimit; }

} // namespace lws_cli
