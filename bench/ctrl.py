#!/usr/bin/env python3
"""Python fixed-rate controller: same loop as ctrl.cpp (pyzmq + protobuf)."""
import argparse
import time

import zmq

from rl_common import emit, load_pb2, now_ns

pb = load_pb2()


def packet(seq, kind, linear):
    p = pb.Packet()
    p.seq = seq
    p.stamp_ns = now_ns()
    p.kind = kind
    p.linear = linear
    return p.SerializeToString()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5611)
    ap.add_argument("--cycles", type=int, default=1000)
    ap.add_argument("--rate", type=float, default=100)
    a = ap.parse_args()
    period = int(1e9 / a.rate)
    ctx = zmq.Context(1)

    def connect():
        s = ctx.socket(zmq.REQ)
        s.setsockopt(zmq.LINGER, 0)
        s.setsockopt(zmq.RCVTIMEO, 100)
        s.connect(f"tcp://127.0.0.1:{a.port}")
        return s

    sock = connect()
    rtt, late = [], []
    overruns = timeouts = 0
    x, target = 0.0, 1.0
    t0 = now_ns() + 100_000_000
    cpu0 = time.process_time()
    for i in range(a.cycles):
        sched = t0 + i * period
        d = sched - now_ns()
        if d > 0:
            time.sleep(d / 1e9)
        start = now_ns()
        u = 2.0 * (target - x)
        sock.send(packet(i, 0, u))
        try:
            r = pb.Packet()
            r.ParseFromString(sock.recv())
            x = r.linear
        except zmq.Again:
            timeouts += 1
            sock = connect()
        end = now_ns()
        rtt.append(end - start)
        late.append(start - sched)
        if end > sched + period:
            overruns += 1
    cpu = time.process_time() - cpu0
    sock.send(packet(a.cycles, 2, 0.0))
    try:
        sock.recv()
    except zmq.Again:
        pass
    emit({"role": "controller", "lang": "python", "rate_hz": a.rate, "cycles": a.cycles,
          "overruns": overruns, "timeouts": timeouts, "cpu_s": cpu, "rtt_ns": rtt,
          "wake_late_ns": late})


if __name__ == "__main__":
    main()
