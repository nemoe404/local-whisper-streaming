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

// CORE-07: a two-word run.
TEST(SeamMerge, TwoWordDuplicateIsDropped) {
    EXPECT_EQ(lws::seam_merge("in the morning", "the morning is cold"), "is cold");
}

// CORE-07: a three-word run.
TEST(SeamMerge, ThreeWordDuplicateIsDropped) {
    EXPECT_EQ(lws::seam_merge("we left the old house", "the old house was empty"), "was empty");
}

// CORE-07 upper bound: k = 4 matches the last four previous words against the first four.
TEST(SeamMerge, FourWordDuplicateIsDropped) {
    EXPECT_EQ(lws::seam_merge("we saw the big", "we saw the big dog"), "dog");
}

// CORE-07 upper bound: the previous text is exactly the four-word run.
TEST(SeamMerge, FourWordRunIsDroppedWhenItIsTheWholeRun) {
    EXPECT_EQ(lws::seam_merge("one two three four", "one two three four five"), "five");
}

// CORE-07 cap: a five-word repeat is never removed.
TEST(SeamMerge, FiveWordDuplicateIsNotDropped) {
    EXPECT_EQ(lws::seam_merge("a b c d e", "a b c d e f"), "a b c d e f");
}

// Pitfall 15: the largest run is tried first, so "b b" goes as two words, not one.
TEST(SeamMerge, LargestRunIsTriedFirst) { EXPECT_EQ(lws::seam_merge("b b", "b b x"), "x"); }

// CORE-07, ROADMAP SC2: a seam without a duplicate is returned unchanged.
TEST(SeamMerge, UnchangedWhenThereIsNoDuplicate) {
    EXPECT_EQ(lws::seam_merge("hello there", "general kenobi"), "general kenobi");
}

// D9: a fully duplicated continuation is empty.
TEST(SeamMerge, FullyDuplicatedContinuationReturnsEmpty) {
    EXPECT_EQ(lws::seam_merge("x y", "x y"), "");
}

// An empty side never matches; an empty continuation stays empty.
TEST(SeamMerge, EmptyInputsAreUnchanged) {
    EXPECT_EQ(lws::seam_merge("", "abc"), "abc");
    EXPECT_EQ(lws::seam_merge("abc", ""), "");
}

// The continuation is shorter than the previous text: the run is bounded by its word count.
TEST(SeamMerge, ContinuationShorterThanTheRunIsHandled) {
    EXPECT_EQ(lws::seam_merge("a b c", "c"), "");
}

// D8, D-09a: boundary punctuation and case are ignored for matching.
TEST(SeamMergeNormalise, CaseFoldAndBoundaryPunctuationAreIgnoredForMatching) {
    EXPECT_EQ(lws::seam_merge("we met at Noon,", "noon. Then we left"), "Then we left");
}

// D-09, Pitfall 14: the kept words are cut from the original text.
TEST(SeamMergeNormalise, KeptTextKeepsItsOriginalCasingAndPunctuation) {
    EXPECT_EQ(lws::seam_merge("a b", "b Hello, world"), "Hello, world");
}

// D8b: a word that is only punctuation has an empty key and never matches.
TEST(SeamMergeNormalise, PunctuationOnlyWordsNeverMatch) {
    EXPECT_EQ(lws::seam_merge("wait --", "-- go"), "-- go");
    EXPECT_EQ(lws::seam_merge("hi …", "… there"), "… there");
    EXPECT_EQ(lws::seam_merge("wait —", "— go"), "— go");
}

// D8: only the word edges are stripped, so an inner apostrophe stays in the key.
TEST(SeamMergeNormalise, InnerPunctuationIsPartOfTheKey) {
    EXPECT_EQ(lws::seam_merge("don't stop", "dont stop now"), "dont stop now");
}

// Runs of spaces are separators, not empty words.
TEST(SeamMergeNormalise, MultipleSpacesDoNotCreateEmptyWords) {
    EXPECT_EQ(lws::seam_merge("a  b", "b   c"), "c");
}

// D-09a: Cyrillic capitals fold (U+0410..U+042F).
TEST(SeamMergeUnicode, CyrillicCapitalisedContinuationMatches) {
    EXPECT_EQ(lws::seam_merge("… мир", "Мир …"), "…");
    EXPECT_EQ(lws::seam_merge("привет мир", "Мир снова"), "снова");
}

// D-09a, odd-uppercase block: Ł is U+0141 (odd), so a single even-uppercase rule would miss it.
TEST(SeamMergeUnicode, PolishCapitalLWithStrokeFolds) {
    EXPECT_EQ(lws::seam_merge("idziemy do Łodzi", "łodzi jest piękna"), "jest piękna");
}

// D-09a: Ź is U+0179 (odd), the second odd-uppercase block.
TEST(SeamMergeUnicode, PolishCapitalZWithAcuteFoldsInTheOddBlock) {
    EXPECT_EQ(lws::seam_merge("w Źródle", "źródle jest"), "jest");
}

// D-09a: U+0490 folds to U+0491.
TEST(SeamMergeUnicode, UkrainianCapitalGheWithUpturnFolds) {
    EXPECT_EQ(lws::seam_merge("до Ґанку", "ґанку вийшов"), "вийшов");
}

// D-09a, Latin-1 Supplement block.
TEST(SeamMergeUnicode, LatinOneCapitalUmlautFolds) {
    EXPECT_EQ(lws::seam_merge("Ünter", "ünter x"), "x");
}

// D-09a: U+0130 folds to plain i, not to its parity neighbour U+0131. (U+0178, the other
// exception, folds to U+00FF.)
TEST(SeamMergeUnicode, DottedCapitalIAndYDiaeresisAreTheExceptions) {
    EXPECT_EQ(lws::seam_merge("İstanbul", "istanbul güzel"), "güzel");
    EXPECT_EQ(lws::seam_merge("Ÿ", "ÿ x"), "x");
}

// Scope: Greek is outside the D-09a fold table in v0.1.0, so capital and small sigma differ.
TEST(SeamMergeUnicode, GreekIsOutsideTheFoldTable) {
    EXPECT_EQ(lws::seam_merge("Σ", "σ x"), "σ x");
}

// D-09a: an invalid byte passes through byte for byte.
TEST(SeamMergeInvalid, InvalidBytesPassThroughByteForByte) {
    EXPECT_EQ(lws::seam_merge("x", "a \xC3 b"), "a \xC3 b");
}

// An invalid byte equals only the same byte.
TEST(SeamMergeInvalid, InvalidSequenceMatchesOnlyTheSameBytes) {
    EXPECT_EQ(lws::seam_merge("\xFF\xFE", "\xFF\xFE tail"), "tail");
}

// A truncated sequence is two invalid bytes; it does not equal the complete character.
TEST(SeamMergeInvalid, TruncatedSequenceDoesNotMatchTheCompleteCharacter) {
    EXPECT_EQ(lws::seam_merge("\xE2\x80", "\xE2\x80\xA6x next"), "\xE2\x80\xA6x next");
}

// D8: a near-miss is not a duplicate.
TEST(SeamMergeScope, NoFuzzyMatch) { EXPECT_EQ(lws::seam_merge("wrold", "world x"), "world x"); }
