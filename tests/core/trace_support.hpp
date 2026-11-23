#pragma once

#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "lws/vad_windower.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <utility>
#include <vector>

// Helpers for replay tests. Not matched by the tests/core/*_test.cpp glob.
namespace lws::test {

// A frame whose every sample equals id, so a segment's contents show which frames it holds.
inline std::array<float, kVadFrameSamples> frame_of(float id) {
    std::array<float, kVadFrameSamples> frame{};
    frame.fill(id);
    return frame;
}

inline std::size_t frame_count(const Segment& segment) {
    return segment.samples.size() / kVadFrameSamples;
}

inline float frame_id(const Segment& segment, std::size_t index) {
    return segment.samples[index * kVadFrameSamples];
}

// Windower from options that must validate; a test with invalid options throws and fails loudly.
inline VadWindower make_windower(const Options& options = Options{}) {
    return VadWindower::create(options).value();
}

// Feeds count frames with ids first_id, first_id + 1, ... at prob. Returns each emitted segment.
inline std::vector<Segment> feed_run(VadWindower& windower, float first_id, std::size_t count,
                                     float prob) {
    std::vector<Segment> emitted;
    std::ranges::for_each(std::views::iota(std::size_t{0}, count), [&](std::size_t index) {
        const auto frame = frame_of(first_id + static_cast<float>(index));
        if (auto segment = windower.feed(frame, prob)) {
            emitted.push_back(std::move(*segment));
        }
    });
    return emitted;
}

} // namespace lws::test
