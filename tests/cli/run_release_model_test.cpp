#include "cli/run.hpp"
#include "cli/spawn.hpp"
#include "decode/decode_support.hpp"
#include "vad/fixture_io.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// lws_cli::run in this process, on the real models, with the input from a file. Where the process
// tests start the binary, these call the loop directly with the way the stream ends as a parameter.
namespace {

using File = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;

File open_file(const std::filesystem::path& path, const char* mode) {
    return {std::fopen(path.c_str(), mode), &std::fclose};
}

// The fixture as raw floats in a file.
void write_fixture_floats(const std::filesystem::path& path) {
    const auto clip = lws::test::load_fixture_wav("clean_en_sample.wav");
    ASSERT_TRUE(clip.has_value());
    const File file = open_file(path, "wb");
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(std::fwrite(clip->data(), sizeof(float), clip->size(), file.get()), clip->size());
}

// The fixture read from a file by the loop that ends the stream with `at_end`. Returns what the
// loop printed on stdout.
std::string run_fixture(lws::StopMode at_end) {
    const lws::test::TempDir dir;
    const auto input_path = dir.path() / "input.f32";
    const auto out_path = dir.path() / "out.txt";
    const auto err_path = dir.path() / "err.txt";
    write_fixture_floats(input_path);

    const File input = open_file(input_path, "rb");
    const File out = open_file(out_path, "w");
    const File err = open_file(err_path, "w");
    EXPECT_TRUE(input && out && err);
    lws_cli::RunConfig config;
    config.model_path = LWS_WHISPER_MODEL_PATH;
    config.vad_model_path = LWS_SILERO_MODEL_PATH;
    const lws_cli::RunIo io{.input_fd = ::fileno(input.get()),
                            .out = out.get(),
                            .err = err.get(),
                            .stop_flag = nullptr};
    const auto report = lws_cli::run(config, io, at_end);
    EXPECT_TRUE(report.has_value());
    static_cast<void>(std::fflush(out.get()));
    return lws::test::read_text_file(out_path);
}

std::vector<std::string> lines_of(const std::string& text) {
    return text | std::views::split('\n') |
           std::views::transform([](auto part) { return std::string(part.begin(), part.end()); }) |
           std::views::filter([](const std::string& line) { return !line.empty(); }) |
           std::ranges::to<std::vector>();
}

} // namespace

TEST(CliRun, DrainAtEndOfInputPrintsTheLastUtterance) {
    const std::vector<std::string> lines = lines_of(run_fixture(lws::StopMode::Drain));
    ASSERT_FALSE(lines.empty());
    constexpr std::string_view kSeparator = "]  ";
    std::string joined;
    std::ranges::for_each(lines, [&](const std::string& line) {
        joined += line.substr(line.find(kSeparator) + kSeparator.size()) + ' ';
    });
    const auto reference = lws::test::reference_words();
    ASSERT_TRUE(reference.has_value());
    EXPECT_LE(lws::test::word_errors(lws::test::normalised_words(joined), *reference),
              std::size_t{LWS_DECODE_MAX_WORD_ERRORS});
}

// The proof that the test above can fail: the fixture ends in speech, so the last utterance is
// still open when the input ends. Giving up at that point prints nothing, where draining prints the
// transcript.
TEST(CliRun, ImmediateAtEndOfInputLosesTheLastUtterance) {
    EXPECT_TRUE(lines_of(run_fixture(lws::StopMode::Immediate)).empty());
}
