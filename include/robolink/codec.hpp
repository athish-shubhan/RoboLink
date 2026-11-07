#pragma once
#include <optional>
#include <string>

#include "robolink/packet.hpp"
#include "robolink.pb.h"

namespace robolink {

void to_proto(const Packet& in, proto::Packet* out);
Packet from_proto(const proto::Packet& in);

// Wire encoding used by the ZeroMQ transport (serialized proto::Packet).
std::string encode(const Packet& p);
std::optional<Packet> decode(const void* data, std::size_t size);

}  // namespace robolink
