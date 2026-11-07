#include "robolink/codec.hpp"

namespace robolink {

void to_proto(const Packet& in, proto::Packet* out) {
  out->set_seq(in.seq);
  out->set_stamp_ns(in.stamp_ns);
  out->set_kind(static_cast<uint32_t>(in.kind));
  out->set_linear(in.linear);
  out->set_angular(in.angular);
  out->set_payload(in.payload);
}

Packet from_proto(const proto::Packet& in) {
  Packet p;
  p.seq = in.seq();
  p.stamp_ns = in.stamp_ns();
  p.kind = static_cast<Kind>(in.kind());
  p.linear = in.linear();
  p.angular = in.angular();
  p.payload = in.payload();
  return p;
}

std::string encode(const Packet& p) {
  proto::Packet m;
  to_proto(p, &m);
  return m.SerializeAsString();
}

std::optional<Packet> decode(const void* data, std::size_t size) {
  proto::Packet m;
  if (!m.ParseFromArray(data, static_cast<int>(size))) return std::nullopt;
  return from_proto(m);
}

}  // namespace robolink
