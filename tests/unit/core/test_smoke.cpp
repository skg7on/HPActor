#include <gtest/gtest.h>

TEST(SmokeTest, GTestWorks) {
    EXPECT_EQ(1, 1);
}

TEST(SmokeTest, CTestDiscoversThis) {
    EXPECT_STRNE("hello", "world");
}
