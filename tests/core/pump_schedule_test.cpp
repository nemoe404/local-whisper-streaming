#include "core/pump_schedule.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace {

using lws::DueMark;
using lws::MarkKind;
using lws::PumpSchedule;

std::optional<MarkKind> kind_at(PumpSchedule& schedule, std::uint64_t popped) {
    const std::optional<DueMark> due = schedule.take_due(popped);
    return due.transform([](const DueMark& mark) { return mark.kind; });
}

// The mark a test expects. When nothing is due the stand-in is a Finish with no drop count, which
// no caller of this helper expects, so the comparison that follows fails.
DueMark mark_of(const std::optional<DueMark>& due) {
    return due.value_or(DueMark{.kind = MarkKind::Finish, .dropped_total = 0});
}

} // namespace

TEST(PumpSchedule, WithNoMarksTheQuotaIsTheSmallerOfAvailableAndPiece) {
    const PumpSchedule schedule;
    EXPECT_EQ(schedule.quota(5000, 2048, 0), 2048U);
    EXPECT_EQ(schedule.quota(100, 2048, 7), 100U);
    EXPECT_EQ(schedule.quota(0, 2048, 0), 0U);
}

TEST(PumpSchedule, AFlushStopsTheQuotaAtItsTarget) {
    PumpSchedule schedule;
    schedule.add_flush(1000, 0);
    EXPECT_EQ(schedule.quota(5000, 2048, 0), 1000U);
    EXPECT_EQ(schedule.quota(5000, 2048, 400), 600U);
    EXPECT_EQ(kind_at(schedule, 999), std::nullopt);
    EXPECT_EQ(kind_at(schedule, 1000), MarkKind::Flush);
    EXPECT_EQ(kind_at(schedule, 1000), std::nullopt);
    EXPECT_EQ(schedule.quota(5000, 2048, 1000), 2048U);
}

TEST(PumpSchedule, TwoFlushesComeDueInOrder) {
    PumpSchedule schedule;
    schedule.add_flush(1000, 0);
    schedule.add_flush(3000, 0);
    EXPECT_EQ(kind_at(schedule, 1000), MarkKind::Flush);
    EXPECT_EQ(schedule.quota(9000, 4096, 1000), 2000U);
    EXPECT_EQ(kind_at(schedule, 3000), MarkKind::Flush);
    EXPECT_EQ(kind_at(schedule, 3000), std::nullopt);
}

TEST(PumpSchedule, AtOnePositionTheOrderIsFlushGapFinish) {
    PumpSchedule schedule;
    schedule.set_finish(2000, 0);
    schedule.add_gap(2000, 77);
    schedule.add_flush(2000, 0);
    EXPECT_EQ(kind_at(schedule, 2000), MarkKind::Flush);
    const DueMark gap = mark_of(schedule.take_due(2000));
    EXPECT_EQ(gap.kind, MarkKind::Gap);
    EXPECT_EQ(gap.dropped_total, 77U);
    EXPECT_EQ(kind_at(schedule, 2000), MarkKind::Finish);
}

TEST(PumpSchedule, AFlushTargetBelowThePoppedCountIsDueAtThePoppedCount) {
    PumpSchedule schedule;
    schedule.add_flush(500, 800);
    EXPECT_EQ(schedule.quota(5000, 2048, 800), 0U);
    EXPECT_EQ(kind_at(schedule, 800), MarkKind::Flush);
}

TEST(PumpSchedule, TheQuotaIsZeroWhileAMarkIsDue) {
    PumpSchedule schedule;
    schedule.add_gap(300, 5);
    EXPECT_EQ(schedule.quota(5000, 2048, 300), 0U);
    EXPECT_EQ(schedule.quota(5000, 2048, 100), 200U);
}

TEST(PumpSchedule, AGapBeyondTheFinishIsLoweredToItWhicheverWasAddedFirst) {
    PumpSchedule gap_after;
    gap_after.set_finish(4000, 0);
    gap_after.add_gap(9000, 12);
    EXPECT_EQ(gap_after.quota(9000, 9000, 0), 4000U);
    EXPECT_EQ(kind_at(gap_after, 4000), MarkKind::Gap);
    EXPECT_EQ(kind_at(gap_after, 4000), MarkKind::Finish);

    PumpSchedule gap_before;
    gap_before.add_gap(9000, 12);
    gap_before.set_finish(4000, 0);
    EXPECT_EQ(gap_before.quota(9000, 9000, 0), 4000U);
    EXPECT_EQ(kind_at(gap_before, 4000), MarkKind::Gap);
    EXPECT_EQ(kind_at(gap_before, 4000), MarkKind::Finish);
}

TEST(PumpSchedule, FinishIsRaisedToThePoppedCountAndStaysDue) {
    PumpSchedule schedule;
    EXPECT_FALSE(schedule.finish_set());
    schedule.set_finish(100, 640);
    EXPECT_TRUE(schedule.finish_set());
    EXPECT_EQ(kind_at(schedule, 640), MarkKind::Finish);
    EXPECT_EQ(kind_at(schedule, 640), MarkKind::Finish);
}
