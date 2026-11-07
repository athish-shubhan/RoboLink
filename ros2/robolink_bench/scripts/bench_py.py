#!/usr/bin/env python3
"""ROS 2 Python (rclpy) endpoint with the same CLI/JSON contract as bench_cpp."""
import argparse
import json
import sys
import time

import rclpy
from rclpy.qos import QoSProfile, ReliabilityPolicy, HistoryPolicy
from robolink_msgs.msg import Packet


def now_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def qos(mode):
    if mode == "throughput":
        return QoSProfile(history=HistoryPolicy.KEEP_ALL, reliability=ReliabilityPolicy.RELIABLE)
    return QoSProfile(history=HistoryPolicy.KEEP_LAST, depth=10, reliability=ReliabilityPolicy.RELIABLE)


def make(seq, kind, payload):
    p = Packet()
    p.seq = seq
    p.stamp_ns = now_ns()
    p.kind = kind
    p.linear = 1.0
    p.payload = bytes(payload)
    return p


def server(a):
    node = rclpy.create_node("rl_server")
    pub = node.create_publisher(Packet, "/rl/echo", qos(a.mode))
    st = {"n": 0, "first": 0, "last": 0, "cpu0": 0.0, "done": False}

    def cb(m):
        st["last"] = now_ns()
        if m.kind == 2:
            pub.publish(m)  # end marker is echoed in both modes so the client knows the stream drained
            st["done"] = True
            return
        if st["n"] == 0:
            st["first"], st["cpu0"] = st["last"], time.process_time()
        st["n"] += 1
        if a.mode == "latency":
            pub.publish(m)

    node.create_subscription(Packet, "/rl/cmd", cb, qos(a.mode))
    while not st["done"] and rclpy.ok():
        rclpy.spin_once(node, timeout_sec=0.1)
    print(json.dumps({"role": "server", "received": st["n"], "first_ns": st["first"],
                      "last_ns": st["last"], "cpu_s": time.process_time() - st["cpu0"]}), flush=True)


def client(a):
    node = rclpy.create_node("rl_client")
    pub = node.create_publisher(Packet, "/rl/cmd", qos(a.mode))
    got = {"seq": -1}

    def cb(m):
        got["seq"] = m.seq

    sub = node.create_subscription(Packet, "/rl/echo", cb, qos(a.mode))
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline and (pub.get_subscription_count() == 0 or
                                           (a.mode == "latency" and sub.get_publisher_count() == 0)):
        rclpy.spin_once(node, timeout_sec=0.05)
    time.sleep(0.3)
    payload = b"x" * a.payload
    rtt, sent, recv = [], 0, 0
    cpu0 = time.process_time()
    t0 = now_ns() + 50_000_000
    if a.mode == "latency":
        total = a.warmup + a.count
        period = int(1e9 / a.rate)
        for i in range(total):
            sched = t0 + i * period
            while now_ns() < sched:
                rclpy.spin_once(node, timeout_sec=max(sched - now_ns(), 0) / 1e9)
            p = make(i, 0, payload)
            t = now_ns()
            pub.publish(p)
            sent += 1
            limit = t + 2_000_000_000
            while got["seq"] != i and now_ns() < limit:
                rclpy.spin_once(node, timeout_sec=0.01)
            if got["seq"] == i:
                recv += 1
                if i >= a.warmup:
                    rtt.append(now_ns() - t)
        pub.publish(make(total, 2, b""))
        rclpy.spin_once(node, timeout_sec=0.1)
    else:
        end = now_ns() + int(a.duration * 1e9)
        while now_ns() < end:
            pub.publish(make(sent, 0, payload))
            sent += 1
        pub.publish(make(sent, 2, b""))
        # keep the publisher alive until the subscriber has drained and echoed the end marker
        limit = now_ns() + 300_000_000_000
        while got["seq"] != sent and now_ns() < limit:
            rclpy.spin_once(node, timeout_sec=0.05)
    print(json.dumps({"role": "client", "transport": "ros2", "lang": "python", "mode": a.mode,
                      "sent": sent, "received": recv, "duration_s": (now_ns() - t0) / 1e9,
                      "cpu_s": time.process_time() - cpu0, "rtt_ns": rtt}, separators=(",", ":")), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--role", default="client")
    ap.add_argument("--mode", default="latency")
    ap.add_argument("--count", type=int, default=5000)
    ap.add_argument("--warmup", type=int, default=200)
    ap.add_argument("--payload", type=int, default=64)
    ap.add_argument("--rate", type=float, default=1000)
    ap.add_argument("--duration", type=float, default=3)
    ap.add_argument("--port", type=int, default=0)  # CLI parity; unused
    a, _ = ap.parse_known_args()
    rclpy.init(args=sys.argv)
    (server if a.role == "server" else client)(a)
    rclpy.shutdown()


if __name__ == "__main__":
    main()
