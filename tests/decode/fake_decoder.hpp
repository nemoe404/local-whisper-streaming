#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

#include "lws/decode_policy.hpp"
#include "shell/decoder.hpp"

// A decode backend that runs no model. The log records what the decoder asked of it and the script
// says what it answers. Not matched by the tests/decode/*_test.cpp glob, and outside the clang-tidy
// header filter like the other support headers. The test owns the log and outlives the backend.
namespace lws::test {

struct FakeDecodeLog {
    std::vector<std::size_t> run_sizes; // the sample count of every run call, in order
    std::size_t span_reads = 0;         // how many times the spans were read
};

struct FakeScript {
    std::int32_t rc = 0;        // what run returns
    std::vector<RawSpan> spans; // what spans returns
    const std::stop_source* raise_stop =
        nullptr;                     // when set, run requests stop here before it returns
    bool abort_when_stopped = false; // a stop seen by the time run returns makes it return -6
};

inline DecodeBackend fake_backend(FakeDecodeLog& log, FakeScript script) {
    constexpr std::int32_t kAborted = -6;
    return DecodeBackend{
        .run =
            [&log, script](std::span<const float> samples, std::stop_token stop) {
                log.run_sizes.push_back(samples.size());
                if (script.raise_stop != nullptr) {
                    script.raise_stop->request_stop();
                }
                return script.abort_when_stopped && stop.stop_requested() ? kAborted : script.rc;
            },
        .spans =
            [&log, script] {
                ++log.span_reads;
                return script.spans;
            },
    };
}

} // namespace lws::test
