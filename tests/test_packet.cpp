#include <gtest/gtest.h>

#include <thread>

#include "robolink/codec.hpp"
#include "robolink/packet.hpp"

using namespace robolink;

TEST(Packet, MonotonicClockNeverGoesBackwards) {
  int64_t prev = now_ns();
  for (int i = 0; i < 1000; ++i) {
    int64_t t = now_ns();
    ASSERT_GE(t, prev);
    prev = t;
  }
}

TEST(Packet, MakePacketStampsCreationTime) {
  const int64_t before = now_ns();
  Packet p = make_packet(1, Kind::Command, 0.5f, 0.f);
  const int64_t after = now_ns();
  EXPECT_GE(p.stamp_ns, before);
  EXPECT_LE(p.stamp_ns, after);
}

TEST(Packet, AgeGrowsWithTimeAndExplicitStampIsRespected) {
  Packet p = make_packet(1, Kind::Command, 0, 0, /*stamp=*/1000);
  EXPECT_EQ(p.age_ns(1000), 0);
  EXPECT_EQ(p.age_ns(6000), 5000);
  EXPECT_LT(p.age_ns(500), 0);  // stamped in the future
  Packet q = make_packet(2, Kind::Command, 0, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_GE(q.age_ns(now_ns()), 5'000'000);
}

TEST(Codec, RoundTripPreservesEveryField) {
  Packet p = make_packet(42, Kind::Sensor, 1.25f, -0.5f, 123456789);
  p.payload = std::string("\x00\x01\xff", 3);
  auto bytes = encode(p);
  auto d = decode(bytes.data(), bytes.size());
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->seq, 42u);
  EXPECT_EQ(d->stamp_ns, 123456789);
  EXPECT_EQ(d->kind, Kind::Sensor);
  EXPECT_FLOAT_EQ(d->linear, 1.25f);
  EXPECT_FLOAT_EQ(d->angular, -0.5f);
  EXPECT_EQ(d->payload, p.payload);
}

TEST(Codec, RejectsGarbage) {
  const char junk[] = {'\xff', '\xff', '\xff', '\xff', '\xff'};
  EXPECT_FALSE(decode(junk, sizeof junk).has_value());
}
