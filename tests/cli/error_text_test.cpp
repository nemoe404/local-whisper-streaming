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
    {lws::Error::InvalidThreshold, 2},
    {lws::Error::InvalidDuration, 2},
    {lws::Error::CeilingTooLong, 2},
    {lws::Error::CeilingTooShort, 2},
    {lws::Error::Overrun, 1},
    {lws::Error::VadFailed, 1},
    {lws::Error::ModelLoad, 1},
    {lws::Error::FrameSizeMismatch, 1},
    {lws::Error::DecodeFailed, 1},
    {lws::Error::InvalidDecoderOptions, 2},
    {lws::Error::NotRunning, 1},
    {lws::Error::AlreadyStarted, 1},
    {lws::Error::InvalidStreamerOptions, 2},
}};

bool mentions(lws::Error error, std::string_view needle) {
    return lws_cli::error_text(error).message.contains(needle);
}

TEST(CliErrorText, EveryErrorHasAMessageAndAnExitCode) {
    static_assert(kRows.front().error == lws::Error::InvalidThreshold);
    static_assert(kRows.back().error == lws::Error::InvalidStreamerOptions);
    static_assert(std::to_underlying(lws::Error::InvalidStreamerOptions) + 1U == kRows.size());

    std::ranges::for_each(kRows, [](const Row& row) {
        const auto text = lws_cli::error_text(row.error);
        EXPECT_FALSE(text.message.empty()) << "error " << static_cast<int>(row.error);
        EXPECT_EQ(text.exit_code, row.exit_code) << "error " << static_cast<int>(row.error);
    });

    EXPECT_TRUE(mentions(lws::Error::InvalidStreamerOptions, "--ring-seconds"));
    EXPECT_TRUE(mentions(lws::Error::InvalidDecoderOptions, "--threads"));
    EXPECT_TRUE(mentions(lws::Error::InvalidDecoderOptions, "--language"));
    EXPECT_FALSE(mentions(lws::Error::ModelLoad, "/"));
    EXPECT_FALSE(mentions(lws::Error::ModelLoad, ".bin"));
}

} // namespace
