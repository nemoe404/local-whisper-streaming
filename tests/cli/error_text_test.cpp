#include "cli/error_text.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace {

struct Row {
    lws::Error error;
    int exit_code;
};

// Every enumerator, in declaration order. The first and the last bound the table.
constexpr std::array<Row, 13> kRows{{
    {.error = lws::Error::InvalidThreshold, .exit_code = 2},
    {.error = lws::Error::InvalidDuration, .exit_code = 2},
    {.error = lws::Error::CeilingTooLong, .exit_code = 2},
    {.error = lws::Error::CeilingTooShort, .exit_code = 2},
    {.error = lws::Error::Overrun, .exit_code = 1},
    {.error = lws::Error::VadFailed, .exit_code = 1},
    {.error = lws::Error::ModelLoad, .exit_code = 1},
    {.error = lws::Error::FrameSizeMismatch, .exit_code = 1},
    {.error = lws::Error::DecodeFailed, .exit_code = 1},
    {.error = lws::Error::InvalidDecoderOptions, .exit_code = 2},
    {.error = lws::Error::NotRunning, .exit_code = 1},
    {.error = lws::Error::AlreadyStarted, .exit_code = 1},
    {.error = lws::Error::InvalidStreamerOptions, .exit_code = 2},
}};

bool mentions(lws::Error error, std::string_view needle) {
    return lws_cli::error_text(error).message.contains(needle);
}

void expect_row(const Row& row) {
    const auto text = lws_cli::error_text(row.error);
    EXPECT_FALSE(text.message.empty()) << "error " << static_cast<int>(row.error);
    EXPECT_EQ(text.exit_code, row.exit_code) << "error " << static_cast<int>(row.error);
}

TEST(CliErrorText, EveryErrorHasAMessageAndAnExitCode) {
    static_assert(kRows.front().error == lws::Error::InvalidThreshold);
    static_assert(kRows.back().error == lws::Error::InvalidStreamerOptions);
    static_assert(std::to_underlying(lws::Error::InvalidStreamerOptions) + 1U == kRows.size());

    std::ranges::for_each(kRows, expect_row);

    EXPECT_TRUE(mentions(lws::Error::InvalidStreamerOptions, "--ring-seconds"));
    EXPECT_TRUE(mentions(lws::Error::InvalidDecoderOptions, "--threads"));
    EXPECT_TRUE(mentions(lws::Error::InvalidDecoderOptions, "--language"));
    EXPECT_FALSE(mentions(lws::Error::ModelLoad, "/"));
    EXPECT_FALSE(mentions(lws::Error::ModelLoad, ".bin"));
}

} // namespace
