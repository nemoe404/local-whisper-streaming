// Seed for the asan-ubsan leg, ASan half (tests/gates/leg_matrix.sh). One write one element past
// the end of a four-element vector, through data() and an index read from a volatile. The write
// stays inside the allocator's chunk, so the uninstrumented legs and TSan do not crash; only ASan
// checks the heap bounds.
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

TEST(SeedAsan, WritesOneElementPastTheEnd) {
    std::vector<std::int32_t> values(4);
    const volatile std::size_t index = 4;
    values.data()[index] = 1;
    EXPECT_EQ(values.size(), std::size_t{4});
}
