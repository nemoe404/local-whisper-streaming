#include "cli/spawn.hpp"
#include "decode/decode_support.hpp"
#include "vad/fixture_io.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

// The real lws-cli binary through a real pipe, on the real models. These tests run on the release
// leg only: they load both models in a separate process, which the sanitizer legs' time budget does
// not cover.
namespace {

using lws::test::Spawned;
using lws::test::TempDir;

constexpr std::size_t kFixtureSamples = 172320;
// An odd piece size, so the CLI's reads end inside a float.
constexpr std::size_t kPieceBytes = 4099;
constexpr auto kRunLimit = std::chrono::seconds{60};

// The two stdout shapes a transcript line may take: its range, two spaces, then the words.
const std::regex& transcript_line() {
    static const std::regex line(
        R"(^\[[0-9]{2,}:[0-5][0-9]:[0-5][0-9]\.[0-9]{3} --> [0-9]{2,}:[0-5][0-9]:[0-5][0-9]\.[0-9]{3}\]  .+$)");
    return line;
}

std::vector<std::string> split_lines(std::string_view text) {
    return text | std::views::split('\n') |
           std::views::transform([](auto part) { return std::string(part.begin(), part.end()); }) |
           std::views::filter([](const std::string& line) { return !line.empty(); }) |
           std::ranges::to<std::vector>();
}

std::vector<std::byte> bytes_of(std::span<const float> samples) {
    const auto raw = std::as_bytes(samples);
    return {raw.begin(), raw.end()};
}

std::vector<std::byte> fixture_bytes() {
    const auto clip = lws::test::load_fixture_wav("clean_en_sample.wav");
    EXPECT_TRUE(clip.has_value());
    return clip ? bytes_of(*clip) : std::vector<std::byte>{};
}

// The words after the range on every transcript line, joined with spaces.
std::string transcript_words(const std::vector<std::string>& lines) {
    constexpr std::string_view kSeparator = "]  ";
    std::string joined;
    std::ranges::for_each(lines, [&](const std::string& line) {
        joined += line.substr(line.find(kSeparator) + kSeparator.size());
        joined += ' ';
    });
    return joined;
}

void expect_within_error_bound(const std::vector<std::string>& lines) {
    const auto reference = lws::test::reference_words();
    ASSERT_TRUE(reference.has_value());
    const std::size_t errors =
        lws::test::word_errors(lws::test::normalised_words(transcript_words(lines)), *reference);
    EXPECT_LE(errors, std::size_t{LWS_DECODE_MAX_WORD_ERRORS});
}

void expect_every_line_is_a_transcript_line(const std::vector<std::string>& lines) {
    std::ranges::for_each(lines, [](const std::string& line) {
        EXPECT_TRUE(std::regex_match(line, transcript_line())) << line;
    });
}

// Writes `bytes` to the descriptor in pieces of kPieceBytes. A failed write ends the writer: the
// child may have exited.
void write_in_pieces(int fd, std::span<const std::byte> bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const std::size_t take = std::min(kPieceBytes, bytes.size() - done);
        const ssize_t count = ::write(fd, bytes.subspan(done).data(), take);
        if (count <= 0) {
            return;
        }
        done += static_cast<std::size_t>(count);
    }
}

std::vector<std::string> model_arguments() {
    return {LWS_CLI_EXE, "--model", LWS_WHISPER_MODEL_PATH, "--vad-model", LWS_SILERO_MODEL_PATH};
}

struct CliResult {
    std::optional<int> status;
    std::string out;
    std::string err;
};

// Starts lws-cli with `arguments`, feeds it `input` through a pipe, closes the pipe, and waits for
// it.
CliResult run_cli(const std::vector<std::string>& arguments, std::span<const std::byte> input) {
    static_cast<void>(std::signal(SIGPIPE, SIG_IGN));
    const TempDir dir;
    const auto out_file = dir.path() / "out.txt";
    const auto err_file = dir.path() / "err.txt";
    lws::test::Pipe pipe = lws::test::make_pipe();
    // The writer is declared before the child, so a child that stops reading is killed first and
    // the blocked write fails, instead of the join waiting for a write that never completes.
    const std::jthread writer([&] {
        write_in_pieces(pipe.write_end.get(), input);
        pipe.write_end.reset();
    });
    Spawned child(arguments, out_file, err_file, pipe.read_end.get());
    pipe.read_end.reset();
    CliResult result;
    result.status = child.wait_for(kRunLimit);
    result.out = lws::test::read_text_file(out_file);
    result.err = lws::test::read_text_file(err_file);
    return result;
}

} // namespace

TEST(CliProcess, FixtureThroughAPipePrintsTheTranscriptAndDrains) {
    const std::vector<std::byte> input = fixture_bytes();
    ASSERT_EQ(input.size(), kFixtureSamples * sizeof(float));

    const CliResult result = run_cli(model_arguments(), input);

    ASSERT_TRUE(result.status.has_value()) << "lws-cli did not exit in time";
    EXPECT_EQ(result.status, std::optional<int>{0}) << result.err;
    const std::vector<std::string> lines = split_lines(result.out);
    EXPECT_FALSE(lines.empty());
    expect_every_line_is_a_transcript_line(lines);
    expect_within_error_bound(lines);
    EXPECT_EQ(result.err.find("overrun:"), std::string::npos) << result.err;
}
