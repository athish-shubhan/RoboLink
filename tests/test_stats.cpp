#include <gtest/gtest.h>

#include "robolink/stats.hpp"

using namespace robolink;

TEST(Stats, NearestRankPercentiles) {
  std::vector<int64_t> v;
  for (int i = 1; i <= 100; ++i) v.push_back(i);
  EXPECT_DOUBLE_EQ(percentile(v, 50), 50);
  EXPECT_DOUBLE_EQ(percentile(v, 95), 95);
  EXPECT_DOUBLE_EQ(percentile(v, 99), 99);
  EXPECT_DOUBLE_EQ(percentile(v, 100), 100);
  EXPECT_DOUBLE_EQ(percentile({}, 50), 0);
}

TEST(Stats, SummaryConvertsToMicrosecondsAndComputesJitter) {
  auto s = summarize_ns({1000, 3000, 1000, 3000});
  EXPECT_EQ(s.count, 4u);
  EXPECT_DOUBLE_EQ(s.max_us, 3.0);
  EXPECT_DOUBLE_EQ(s.jitter_us, 2.0);  // |diff| is always 2000 ns
}

TEST(Stats, WindowKeepsOnlyNewestSamples) {
  LatencyWindow w(3);
  for (int i = 1; i <= 5; ++i) w.add(i * 1000);
  auto s = w.samples();
  ASSERT_EQ(s.size(), 3u);
  EXPECT_EQ(s.front(), 3000);
  EXPECT_EQ(w.summary().count, 3u);
}
