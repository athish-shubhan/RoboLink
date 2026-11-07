#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <numeric>
#include <thread>
#include <vector>

#include "robolink/bounded_queue.hpp"

using namespace robolink;
using namespace std::chrono_literals;

TEST(BoundedQueue, FifoOrder) {
  BoundedQueue<int> q(4);
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(q.try_push(i));
  for (int i = 0; i < 4; ++i) EXPECT_EQ(*q.try_pop(), i);
  EXPECT_FALSE(q.try_pop().has_value());
}

TEST(BoundedQueue, TryPushRejectsWhenFull) {
  BoundedQueue<int> q(2);
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_FALSE(q.try_push(3));
  EXPECT_EQ(q.size(), 2u);
  EXPECT_EQ(q.rejected(), 1u);
  EXPECT_EQ(*q.try_pop(), 1);  // rejected item never entered
}

TEST(BoundedQueue, DropOldestKeepsNewest) {
  BoundedQueue<int> q(3);
  EXPECT_FALSE(q.push_drop_oldest(1));
  q.push_drop_oldest(2);
  q.push_drop_oldest(3);
  EXPECT_TRUE(q.push_drop_oldest(4));
  EXPECT_TRUE(q.push_drop_oldest(5));
  EXPECT_EQ(q.dropped(), 2u);
  EXPECT_EQ(q.size(), 3u);
  EXPECT_EQ(*q.try_pop(), 3);
  EXPECT_EQ(*q.try_pop(), 4);
  EXPECT_EQ(*q.try_pop(), 5);
}

TEST(BoundedQueue, ZeroCapacityIsClampedToOne) {
  BoundedQueue<int> q(0);
  EXPECT_EQ(q.capacity(), 1u);
  EXPECT_TRUE(q.try_push(1));
  EXPECT_FALSE(q.try_push(2));
}

TEST(BoundedQueue, PopTimesOutOnEmpty) {
  BoundedQueue<int> q(2);
  const auto t0 = std::chrono::steady_clock::now();
  auto v = q.pop_for(30ms);
  const auto dt = std::chrono::steady_clock::now() - t0;
  EXPECT_FALSE(v.has_value());
  EXPECT_GE(dt, 25ms);
}

TEST(BoundedQueue, PopWakesOnPush) {
  BoundedQueue<int> q(2);
  std::thread t([&] { std::this_thread::sleep_for(20ms); q.try_push(7); });
  auto v = q.pop_for(2s);
  t.join();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, 7);
}

TEST(BoundedQueue, CloseWakesBlockedConsumerAndRejectsPush) {
  BoundedQueue<int> q(2);
  std::thread t([&] { std::this_thread::sleep_for(20ms); q.close(); });
  const auto t0 = std::chrono::steady_clock::now();
  auto v = q.pop_for(5s);
  t.join();
  EXPECT_FALSE(v.has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);
  EXPECT_FALSE(q.try_push(1));
  EXPECT_TRUE(q.closed());
}

TEST(BoundedQueue, CloseStillDrainsRemainingItems) {
  BoundedQueue<int> q(4);
  q.try_push(1);
  q.close();
  EXPECT_EQ(*q.pop_for(10ms), 1);
  EXPECT_FALSE(q.pop_for(10ms).has_value());
}

TEST(BoundedQueue, ConcurrentProducersConsumersLoseNothing) {
  BoundedQueue<int> q(16);
  constexpr int kProducers = 4, kPer = 5000;
  std::atomic<long> sum{0};
  std::atomic<int> count{0};
  std::vector<std::thread> prod, cons;
  for (int p = 0; p < kProducers; ++p)
    prod.emplace_back([&] {
      for (int i = 1; i <= kPer; ++i)
        while (!q.try_push(i)) std::this_thread::yield();
    });
  for (int c = 0; c < 2; ++c)
    cons.emplace_back([&] {
      while (count < kProducers * kPer) {
        if (auto v = q.pop_for(5ms)) { sum += *v; ++count; }
      }
    });
  for (auto& t : prod) t.join();
  for (auto& t : cons) t.join();
  EXPECT_EQ(count.load(), kProducers * kPer);
  EXPECT_EQ(sum.load(), static_cast<long>(kProducers) * kPer * (kPer + 1) / 2);
}
