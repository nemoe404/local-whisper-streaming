#include "smoke/lws_smoke.hpp"

#include <gtest/gtest.h>

#include <string_view>

// The linked whisper must be the pinned submodule (1.9.5 base), not a system copy.
TEST(LinkSmoke, ReportsPinnedForkVersion) {
    const std::string_view version = lws::smoke::whisper_version_string();
    EXPECT_FALSE(version.empty());
    EXPECT_TRUE(version.starts_with("1.9.5"));
}
