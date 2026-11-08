// Transport benchmark endpoint for ZeroMQ and gRPC (C++).
// Roles: server (echo / sink) and client (paced latency or max-rate throughput).
// The client prints one JSON object with raw RTT samples; analysis lives in
// bench/run_bench.py so every transport/language is summarised identically.
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <zmq.hpp>

#include "robolink.grpc.pb.h"
#include "bench_util.hpp"
#include "robolink/codec.hpp"

using namespace robolink;

struct Args {
  std::string transport = "zmq", role = "client", mode = "latency";
  int port = 5601, count = 5000, warmup = 200, payload = 64;
  double rate = 1000, duration = 3;
};

static Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i + 1 < argc + 1; ++i) {
    std::string k = argv[i];
    auto v = [&] { return i + 1 < argc ? argv[++i] : (char*)"0"; };
    if (k == "--transport") a.transport = v();
    else if (k == "--role") a.role = v();
    else if (k == "--mode") a.mode = v();
    else if (k == "--port") a.port = atoi(v());
    else if (k == "--count") a.count = atoi(v());
    else if (k == "--warmup") a.warmup = atoi(v());
    else if (k == "--payload") a.payload = atoi(v());
    else if (k == "--rate") a.rate = atof(v());
    else if (k == "--duration") a.duration = atof(v());
  }
  return a;
}

static void print_client(const Args& a, uint64_t sent, uint64_t received, double dur_s,
                         const std::vector<int64_t>& rtt, double cpu,
                         const std::vector<int64_t>& lag = {}) {
  std::printf("{\"role\":\"client\",\"transport\":\"%s\",\"lang\":\"cpp\",\"mode\":\"%s\","
              "\"sent\":%llu,\"received\":%llu,\"duration_s\":%.6f,\"cpu_s\":%.6f,\"rtt_ns\":[",
              a.transport.c_str(), a.mode.c_str(), (unsigned long long)sent,
              (unsigned long long)received, dur_s, cpu);
  for (size_t i = 0; i < rtt.size(); ++i) std::printf(i ? ",%lld" : "%lld", (long long)rtt[i]);
  std::printf("]");
  // lag_ns[i] = actual send time minus intended send time. rtt + lag is the
  // latency a client that wanted to send on schedule would have seen, which
  // avoids coordinated omission (a slow reply delays later sends in a closed loop).
  if (!lag.empty()) {
    std::printf(",\"lag_ns\":[");
    for (size_t i = 0; i < lag.size(); ++i) std::printf(i ? ",%lld" : "%lld", (long long)lag[i]);
    std::printf("]");
  }
  std::printf("}\n");
}

static void print_server(uint64_t received, int64_t first, int64_t last, double cpu) {
  std::printf("{\"role\":\"server\",\"received\":%llu,\"first_ns\":%lld,\"last_ns\":%lld,\"cpu_s\":%.6f}\n",
              (unsigned long long)received, (long long)first, (long long)last, cpu);
}

// Sleep until an absolute monotonic time.
static void sleep_until_ns(int64_t t) {
  int64_t d = t - now_ns();
  if (d > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(d));
}

// ---------------------------------------------------------------- ZeroMQ
static int run_zmq_server(const Args& a) {
  zmq::context_t ctx(1);
  const std::string ep = "tcp://127.0.0.1:" + std::to_string(a.port);
  if (a.mode == "latency") {
    zmq::socket_t s(ctx, zmq::socket_type::rep);
    s.set(zmq::sockopt::rcvtimeo, 20000);  // a dead client must not hang the harness
    s.bind(ep);
    uint64_t n = 0;
    int64_t first = 0;
    double cpu0 = 0;
    for (;;) {
      zmq::message_t m;
      if (!s.recv(m, zmq::recv_flags::none)) break;
      if (n++ == 0) { first = now_ns(); cpu0 = cpu_s(); }
      auto p = decode(m.data(), m.size());
      s.send(m, zmq::send_flags::none);
      if (p && p->kind == Kind::End) break;
    }
    print_server(n, first, now_ns(), cpu_s() - cpu0);
    return 0;
  }
  zmq::socket_t s(ctx, zmq::socket_type::pull);
  s.set(zmq::sockopt::rcvhwm, 100000);
  s.set(zmq::sockopt::rcvtimeo, 20000);  // a lost End marker must not hang the harness
  s.bind(ep);
  uint64_t n = 0;
  int64_t first = 0, last = 0;
  double cpu0 = 0;
  for (;;) {
    zmq::message_t m;
    if (!s.recv(m, zmq::recv_flags::none)) break;
    auto p = decode(m.data(), m.size());
    if (!p || p->kind == Kind::End) break;
    last = now_ns();
    if (n++ == 0) { first = last; cpu0 = cpu_s(); }
  }
  print_server(n, first, last, cpu_s() - cpu0);
  return 0;
}

static int run_zmq_client(const Args& a) {
  zmq::context_t ctx(1);
  const std::string ep = "tcp://127.0.0.1:" + std::to_string(a.port);
  std::string pay(a.payload, 'x');
  if (a.mode == "latency") {
    zmq::socket_t s(ctx, zmq::socket_type::req);
    s.connect(ep);
    std::vector<int64_t> rtt, lag;
    const int total = a.warmup + a.count;
    const int64_t period = static_cast<int64_t>(1e9 / a.rate), t0 = now_ns() + 50'000'000;
    uint64_t recv = 0;
    const double cpu0 = cpu_s();
    for (int i = 0; i < total; ++i) {
      sleep_until_ns(t0 + i * period);
      Packet p = make_packet(i, Kind::Command, 1.f, 0.f);
      p.payload = pay;
      auto bytes = encode(p);
      const int64_t t = now_ns();
      s.send(zmq::buffer(bytes), zmq::send_flags::none);
      zmq::message_t m;
      (void)s.recv(m, zmq::recv_flags::none);
      const int64_t r = now_ns() - t;
      ++recv;
      if (i >= a.warmup) { rtt.push_back(r); lag.push_back(t - (t0 + i * period)); }
    }
    Packet e = make_packet(total, Kind::End, 0, 0);
    auto eb = encode(e);
    s.send(zmq::buffer(eb), zmq::send_flags::none);
    zmq::message_t m;
    (void)s.recv(m, zmq::recv_flags::none);
    print_client(a, total, recv, (now_ns() - t0) / 1e9, rtt, cpu_s() - cpu0, lag);
    return 0;
  }
  zmq::socket_t s(ctx, zmq::socket_type::push);
  s.set(zmq::sockopt::sndhwm, 100000);
  s.set(zmq::sockopt::linger, 5000);
  s.connect(ep);
  uint64_t sent = 0;
  const double cpu0 = cpu_s();
  const int64_t t0 = now_ns(), end = t0 + static_cast<int64_t>(a.duration * 1e9);
  while (now_ns() < end) {
    Packet p = make_packet(sent, Kind::Command, 1.f, 0.f);
    p.payload = pay;
    auto b = encode(p);
    s.send(zmq::buffer(b), zmq::send_flags::none);
    ++sent;
  }
  auto eb = encode(make_packet(sent, Kind::End, 0, 0));
  s.send(zmq::buffer(eb), zmq::send_flags::none);
  print_client(a, sent, 0, (now_ns() - t0) / 1e9, {}, cpu_s() - cpu0);
  return 0;
}

// ------------------------------------------------------------------ gRPC
class LinkImpl final : public proto::Link::Service {
 public:
  std::promise<void> done;
  uint64_t n = 0;
  int64_t first = 0;
  double cpu0 = 0;
  grpc::Status Echo(grpc::ServerContext*, const proto::Packet* in, proto::Packet* out) override {
    *out = *in;
    if (n++ == 0) { first = now_ns(); cpu0 = cpu_s(); }
    if (in->kind() == static_cast<uint32_t>(Kind::End)) {
      print_server(n, first, now_ns(), cpu_s() - cpu0);
      std::fflush(stdout);
      done.set_value();
    }
    return grpc::Status::OK;
  }
  grpc::Status Stream(grpc::ServerContext*, grpc::ServerReader<proto::Packet>* r,
                      proto::StreamSummary* sum) override {
    proto::Packet p;
    uint64_t n = 0;
    int64_t first = 0, last = 0;
    double c0 = 0;
    while (r->Read(&p)) {
      last = now_ns();
      if (n++ == 0) { first = last; c0 = cpu_s(); }
    }
    sum->set_received(n);
    sum->set_first_ns(first);
    sum->set_last_ns(last);
    print_server(n, first, last, cpu_s() - c0);
    std::fflush(stdout);
    done.set_value();
    return grpc::Status::OK;
  }
};

static int run_grpc_server(const Args& a) {
  LinkImpl svc;
  grpc::ServerBuilder b;
  b.AddListeningPort("127.0.0.1:" + std::to_string(a.port), grpc::InsecureServerCredentials());
  b.RegisterService(&svc);
  auto server = b.BuildAndStart();
  svc.done.get_future().wait();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  server->Shutdown();
  return 0;
}

static int run_grpc_client(const Args& a) {
  auto ch = grpc::CreateChannel("127.0.0.1:" + std::to_string(a.port), grpc::InsecureChannelCredentials());
  ch->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(10));
  auto stub = proto::Link::NewStub(ch);
  std::string pay(a.payload, 'x');
  if (a.mode == "latency") {
    std::vector<int64_t> rtt, lag;
    const int total = a.warmup + a.count;
    const int64_t period = static_cast<int64_t>(1e9 / a.rate), t0 = now_ns() + 50'000'000;
    uint64_t recv = 0;
    const double cpu0 = cpu_s();
    for (int i = 0; i < total; ++i) {
      sleep_until_ns(t0 + i * period);
      Packet p = make_packet(i, Kind::Command, 1.f, 0.f);
      p.payload = pay;
      proto::Packet req, resp;
      to_proto(p, &req);
      grpc::ClientContext cc;
      const int64_t t = now_ns();
      auto st = stub->Echo(&cc, req, &resp);
      const int64_t r = now_ns() - t;
      if (st.ok()) ++recv;
      if (i >= a.warmup) { rtt.push_back(r); lag.push_back(t - (t0 + i * period)); }
    }
    proto::Packet req, resp;
    to_proto(make_packet(total, Kind::End, 0, 0), &req);
    grpc::ClientContext cc;
    stub->Echo(&cc, req, &resp);
    print_client(a, total, recv, (now_ns() - t0) / 1e9, rtt, cpu_s() - cpu0, lag);
    return 0;
  }
  grpc::ClientContext cc;
  proto::StreamSummary sum;
  auto w = stub->Stream(&cc, &sum);
  uint64_t sent = 0;
  const double cpu0 = cpu_s();
  const int64_t t0 = now_ns(), end = t0 + static_cast<int64_t>(a.duration * 1e9);
  while (now_ns() < end) {
    Packet p = make_packet(sent, Kind::Command, 1.f, 0.f);
    p.payload = pay;
    proto::Packet req;
    to_proto(p, &req);
    if (!w->Write(req)) break;
    ++sent;
  }
  w->WritesDone();
  w->Finish();
  print_client(a, sent, 0, (now_ns() - t0) / 1e9, {}, cpu_s() - cpu0);
  return 0;
}

int main(int argc, char** argv) {
  Args a = parse(argc, argv);
  if (a.transport == "zmq") return a.role == "server" ? run_zmq_server(a) : run_zmq_client(a);
  if (a.transport == "grpc") return a.role == "server" ? run_grpc_server(a) : run_grpc_client(a);
  std::fprintf(stderr, "unknown transport\n");
  return 2;
}
