#include "lws/seam_merge.hpp"

#include <gtest/gtest.h>

#include <string>

// CORE-07, D7: the continuation loses the duplicated leading word.
TEST(SeamMerge, OneWordDuplicateIsDroppedFromTheContinuation) {
    EXPECT_EQ(lws::seam_merge("we went to the", "the store opened"), "store opened");
}

// D-09, T-02-07: the kept slice starts at a byte index taken from iterators, never a code-point
// index. Both indices are literals written here, not computed from the code under test.
TEST(SeamMergeOffset, KeptWordStartsAtItsLiteralByteIndex) {
    // (a) "the   store": "the" is bytes 0 to 2, three spaces are bytes 3 to 5, "store" starts at 6.
    const std::string ascii = "the   store";
    const std::string kept_ascii = lws::seam_merge("we went to the", ascii);
    EXPECT_EQ(kept_ascii, "store");
    EXPECT_EQ(ascii.size() - kept_ascii.size(), 6U);

    // (b) "мир  снова": "мир" is 3 two-byte letters = bytes 0 to 5, two spaces are bytes 6 and 7,
    // "снова" starts at 8. A code-point offset would give 5 and cut the wrong place.
    const std::string cyrillic = "мир  снова";
    const std::string kept_cyrillic = lws::seam_merge("привет мир", cyrillic);
    EXPECT_EQ(kept_cyrillic, "снова");
    EXPECT_EQ(cyrillic.size() - kept_cyrillic.size(), 8U);
}
