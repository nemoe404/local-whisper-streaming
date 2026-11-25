// A consumer of the installed library, using the public header only.
//
// Usage: consumer MODEL VAD_MODEL INPUT.f32
//   Decodes the raw 16 kHz mono float32 file through lws::Streamer and prints the joined text on
//   stdout. Exit codes: 0 text was produced, 1 an error or empty text (a message goes to stderr),
//   2 bad arguments.

#include <lws/streamer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 2;
constexpr std::size_t kPushSamples = 4096;

// The file's bytes decoded as floats. An empty vector means the file could not be read.
[[nodiscard]] std::vector<float> read_floats(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in),
                                  std::istreambuf_iterator<char>()};
    std::vector<float> samples(bytes.size() / sizeof(float));
    std::memcpy(samples.data(), bytes.data(), samples.size() * sizeof(float));
    return samples;
}

[[nodiscard]] std::expected<void, lws::Error>
decode(const std::vector<float>& samples, lws::StreamerOptions options, std::string& text) {
    // The ring holds the whole file, so a push never overruns.
    options.ring_capacity_samples = std::max(options.ring_capacity_samples, samples.size());
    auto streamer = lws::Streamer::create(options);
    if (!streamer) {
        return std::unexpected(streamer.error());
    }
    // The callback runs on the decoder thread. The string is read only after stop returns, which
    // joins that thread.
    const auto callback_set = streamer->on_text([&text](const lws::TextEvent& event) noexcept {
        if (event.kind == lws::EventKind::Text) {
            text += event.text;
            text += ' ';
        }
    });
    if (!callback_set) {
        return std::unexpected(callback_set.error());
    }
    if (const auto started = streamer->start(); !started) {
        return std::unexpected(started.error());
    }
    std::expected<void, lws::Error> pushed;
    std::ranges::for_each(samples | std::views::chunk(kPushSamples), [&](auto piece) {
        if (!pushed) {
            return;
        }
        const std::vector<float> copy(piece.begin(), piece.end());
        pushed = streamer->push(copy);
    });
    if (!pushed) {
        return pushed;
    }
    return streamer->stop(lws::StopMode::Drain);
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    if (args.size() != 4) {
        std::fputs("usage: consumer MODEL VAD_MODEL INPUT.f32\n", stderr);
        return kUsage;
    }
    const std::vector<float> samples = read_floats(args[3]);
    if (samples.empty()) {
        std::fputs("consumer: the input file is empty or unreadable\n", stderr);
        return kFailed;
    }
    lws::StreamerOptions options;
    options.model_path = args[1];
    options.vad_model_path = args[2];
    std::string text;
    const auto result = decode(samples, options, text);
    if (!result) {
        std::fprintf(stderr, "consumer: the library reported error %u\n",
                     static_cast<unsigned>(result.error()));
        return kFailed;
    }
    if (text.empty()) {
        std::fputs("consumer: no text was produced\n", stderr);
        return kFailed;
    }
    std::fputs(text.c_str(), stdout);
    std::fputc('\n', stdout);
    return kOk;
}
