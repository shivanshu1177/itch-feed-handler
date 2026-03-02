#include <gtest/gtest.h>
#include "protocol/sequence_tracker.hpp"

using namespace itch;

TEST(SequenceTracker, InOrder) {
    SequenceTracker st(0);
    EXPECT_TRUE(st.try_advance(1));
    EXPECT_EQ(st.current(), 1u);
    EXPECT_TRUE(st.try_advance(2));
    EXPECT_TRUE(st.try_advance(3));
    EXPECT_EQ(st.gap_count(), 0u);
    EXPECT_EQ(st.dup_count(), 0u);
}

TEST(SequenceTracker, GapDetected) {
    SequenceTracker st(0);
    EXPECT_TRUE(st.try_advance(1));
    EXPECT_FALSE(st.try_advance(5));  // gap: 2,3,4 missing
    EXPECT_EQ(st.gap_count(), 1u);
    GapInfo g = st.last_gap();
    EXPECT_EQ(g.start_seq, 2u);
    EXPECT_EQ(g.end_seq, 4u);
}

TEST(SequenceTracker, DuplicateIgnored) {
    SequenceTracker st(5);
    st.try_advance(6);
    EXPECT_FALSE(st.try_advance(6)); // dup
    EXPECT_FALSE(st.try_advance(3)); // old seq
    EXPECT_EQ(st.dup_count(), 2u);
    EXPECT_EQ(st.gap_count(), 0u);
}

TEST(SequenceTracker, MultipleGaps) {
    SequenceTracker st(0);
    st.try_advance(1);
    st.try_advance(10);   // gap 2-9
    st.try_advance(20);   // gap 11-19
    EXPECT_EQ(st.gap_count(), 2u);
}
