#!/usr/bin/env python3
"""Benchmark orchestrator. Starts a server and a client per transport, parses the
raw JSON they print and computes every statistic here, so all transports and
languages are summarised by the same code.

  run_bench.py transport --bin build/rl_bench --transports zmq grpc --out results/x.json
  run_bench.py control   --build build --out results/y.json [--stress-cpus N] [--netem "delay 2ms 1ms"]
"""
import argparse
import csv
import json
import math
import os
import platform
import shlex
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


# ----------------------------------------------------------------- statistics
def percentile(vals, p):
    """Nearest-rank percentile (same definition as the C++ core)."""
    if not vals:
        return None
    s = sorted(vals)
    rank = max(1, math.ceil(p / 100.0 * len(s)))
    return s[min(rank, len(s)) - 1]


def summarize(rtt_ns):
    us = [v / 1e3 for v in rtt_ns]
    jit = (sum(abs(us[i] - us[i - 1]) for i in range(1, len(us))) / (len(us) - 1)) if len(us) > 1 else 0.0
    mean = sum(us) / len(us) if us else 0.0
    sd = math.sqrt(sum((v - mean) ** 2 for v in us) / len(us)) if us else 0.0
    return {"n": len(us), "p50_us": percentile(us, 50), "p95_us": percentile(us, 95),
            "p99_us": percentile(us, 99), "max_us": max(us) if us else None,
            "mean_us": mean, "stddev_us": sd, "jitter_us": jit}


# -------------------------------------------------------------------- machine
def _sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=10).stdout.strip()
    except Exception:
        return ""


def machine_info():
    sysname = platform.system()
    if sysname == "Darwin":
        cpu = _sh("sysctl -n machdep.cpu.brand_string")
        ram = int(_sh("sysctl -n hw.memsize") or 0)
        osname = "macOS " + (platform.mac_ver()[0] or _sh("sw_vers -productVersion"))
    else:
        cpu = _sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2") or _sh("lscpu | grep -m1 'Model name' | cut -d: -f2") or platform.processor()
        meminfo = _sh("grep MemTotal /proc/meminfo")
        ram = int(meminfo.split()[1]) * 1024 if meminfo else 0
        osname = _sh("grep PRETTY_NAME /etc/os-release | cut -d= -f2").strip('"') or platform.platform()
    docker = os.path.exists("/.dockerenv") or os.environ.get("ROBOLINK_ENV") == "docker"
    return {"cpu": cpu.strip(), "logical_cpus": os.cpu_count(), "ram_gb": round(ram / 2**30, 1),
            "os": osname, "kernel": platform.release(), "arch": platform.machine(),
            "environment": ("docker-linux (Colima VM)" if docker else
                            ("macos-native" if sysname == "Darwin" else "linux-native")),
            "python": platform.python_version(), "loadavg_start": os.getloadavg()}


# ----------------------------------------------------------------- run helpers
def last_json(text):
    for line in reversed(text.strip().splitlines()):
        line = line.strip()
        if line.startswith("{"):
            return json.loads(line)
    raise RuntimeError("no JSON in output: " + text[-500:])


def run_pair(prefix, role_args_server, role_args_client, env=None, ready_s=1.0, timeout=400, server_wait=400):
    srv = subprocess.Popen(prefix + role_args_server, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           text=True, env=env)
    time.sleep(ready_s)
    cli = subprocess.run(prefix + role_args_client, capture_output=True, text=True, env=env, timeout=timeout)
    if cli.returncode != 0:
        srv.kill()
        raise RuntimeError("client failed: " + cli.stderr[-800:])
    try:
        sout, serr = srv.communicate(timeout=server_wait)
    except subprocess.TimeoutExpired:
        srv.kill()
        sout, serr = srv.communicate()
    return last_json(sout), last_json(cli.stdout)


def write_results(out, doc, raw_rows, raw_header):
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    doc["machine"]["loadavg_end"] = os.getloadavg()
    with open(out, "w") as f:
        json.dump(doc, f, indent=2)
    if raw_rows:
        raw = os.path.splitext(out)[0] + ".raw.csv"
        with open(raw, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(raw_header)
            w.writerows(raw_rows)
    print(f"wrote {out}")


# ------------------------------------------------------------------ transports
def cmd_transport(a):
    cases = {}
    if "zmq" in a.transports:
        cases["zmq_cpp"] = [a.bin, "--transport", "zmq"]
    if "grpc" in a.transports:
        cases["grpc_cpp"] = [a.bin, "--transport", "grpc"]
    if a.ros2_cpp:
        cases["ros2_cpp"] = shlex.split(a.ros2_cpp)
    if a.ros2_py:
        cases["ros2_py"] = shlex.split(a.ros2_py)
    doc = {"command": " ".join(sys.argv), "machine": machine_info(), "note": a.note,
           "params": {"latency_rate_hz": a.rate, "latency_samples": a.count, "warmup": a.warmup,
                      "payload_bytes": a.payload, "throughput_seconds": a.duration, "runs": a.runs},
           "cases": {}}
    raw, port = [], a.port
    for name, prefix in cases.items():
        entry = {"latency": [], "throughput": []}
        for run in range(a.runs):
            port += 1
            common = ["--port", str(port), "--payload", str(a.payload)]
            lat_args = ["--mode", "latency", "--count", str(a.count), "--rate", str(a.rate),
                        "--warmup", str(a.warmup)]
            try:
                s, c = run_pair(prefix, ["--role", "server"] + lat_args + common,
                                ["--role", "client"] + lat_args + common, ready_s=a.ready)
                st = summarize(c["rtt_ns"])
                st["cpu_pct_of_one_core"] = 100.0 * (c["cpu_s"] + s["cpu_s"]) / c["duration_s"]
                st["received"], st["sent"] = c["received"], c["sent"]
                entry["latency"].append(st)
                raw += [[name, run, i, v] for i, v in enumerate(c["rtt_ns"])]
            except Exception as e:  # keep going: report as not measured
                entry["latency"].append({"error": str(e)[-600:]})
            port += 1
            common = ["--port", str(port), "--payload", str(a.payload)]
            thr_args = ["--mode", "throughput", "--duration", str(a.duration)]
            try:
                s, c = run_pair(prefix, ["--role", "server"] + thr_args + common,
                                ["--role", "client"] + thr_args + common, ready_s=a.ready)
                span = (s["last_ns"] - s["first_ns"]) / 1e9
                entry["throughput"].append({
                    "sent": c["sent"], "received": s["received"],
                    "delivered_ratio": s["received"] / c["sent"] if c["sent"] else None,
                    "msgs_per_s": (s["received"] - 1) / span if span > 0 else None,
                    "cpu_pct_of_one_core": 100.0 * (c["cpu_s"] + s["cpu_s"]) / max(span, 1e-9)})
            except Exception as e:
                entry["throughput"].append({"error": str(e)[-600:]})
        doc["cases"][name] = entry
        print(name, json.dumps(entry["latency"][0] if entry["latency"] else {})[:200])
    write_results(a.out, doc, raw, ["case", "run", "i", "rtt_ns"])


# --------------------------------------------------------------------- control
def cmd_control(a):
    build = os.path.abspath(a.build)
    env = dict(os.environ)
    env["ROBOLINK_GEN_PY"] = os.path.join(build, "gen", "python")
    py = a.python or sys.executable
    ctrls = {"cpp": [os.path.join(build, "rl_ctrl")], "python": [py, os.path.join(HERE, "ctrl.py")]}
    doc = {"command": " ".join(sys.argv), "machine": machine_info(), "note": a.note,
           "params": {"rate_hz": a.rate, "cycles": a.cycles, "stress_cpus": a.stress_cpus,
                      "netem": a.netem, "runs": a.runs, "plant_hold_ms": a.hold_ms, "plant_stop_ms": a.stop_ms},
           "cases": {}}
    raw, port = [], a.port
    stress = netem_on = None
    try:
        if a.netem:
            subprocess.run(["tc", "qdisc", "add", "dev", "lo", "root", "netem"] + a.netem.split(), check=True)
            netem_on = True
        if a.stress_cpus:
            stress = subprocess.Popen(["stress-ng", "--cpu", str(a.stress_cpus), "--timeout", "0"],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(2)
        for name, prefix in ctrls.items():
            runs = []
            for run in range(a.runs):
                port += 1
                plant = subprocess.Popen([os.path.join(build, "rl_plant"), "--port", str(port),
                                          "--hold-ms", str(a.hold_ms), "--stop-ms", str(a.stop_ms)],
                                         stdout=subprocess.PIPE, text=True)
                time.sleep(0.5)
                c = subprocess.run(prefix + ["--port", str(port), "--cycles", str(a.cycles), "--rate", str(a.rate)],
                                   capture_output=True, text=True, env=env, timeout=600)
                if c.returncode != 0:
                    plant.kill()
                    runs.append({"error": c.stderr[-300:]})
                    continue
                pout, _ = plant.communicate(timeout=30)
                cj, pj = last_json(c.stdout), last_json(pout)
                st = summarize(cj["rtt_ns"])
                late = summarize(cj["wake_late_ns"])
                st.update({"wake_late_p50_us": late["p50_us"], "wake_late_p99_us": late["p99_us"],
                           "wake_late_max_us": late["max_us"], "overruns": cj["overruns"],
                           "overrun_pct": 100.0 * cj["overruns"] / cj["cycles"], "timeouts": cj["timeouts"],
                           "controller_cpu_pct_of_one_core": 100.0 * cj["cpu_s"] / (cj["cycles"] / a.rate),
                           "plant": pj})
                runs.append(st)
                raw += [[name, run, i, r, w] for i, (r, w) in enumerate(zip(cj["rtt_ns"], cj["wake_late_ns"]))]
            doc["cases"][name] = runs
            print(name, json.dumps(runs[0])[:300])
    finally:
        if stress:
            stress.send_signal(signal.SIGTERM)
            stress.wait()
        if netem_on:
            subprocess.run(["tc", "qdisc", "del", "dev", "lo", "root"])
    write_results(a.out, doc, raw, ["controller", "run", "i", "rtt_ns", "wake_late_ns"])


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("transport")
    t.add_argument("--bin", default="build/rl_bench")
    t.add_argument("--transports", nargs="*", default=["zmq", "grpc"])
    t.add_argument("--ros2-cpp", default="")
    t.add_argument("--ros2-py", default="")
    t.add_argument("--rate", type=float, default=1000)
    t.add_argument("--count", type=int, default=5000)
    t.add_argument("--warmup", type=int, default=200)
    t.add_argument("--payload", type=int, default=64)
    t.add_argument("--duration", type=float, default=3)
    t.add_argument("--runs", type=int, default=3)
    t.add_argument("--port", type=int, default=5600)
    t.add_argument("--ready", type=float, default=1.0)
    t.add_argument("--note", default="")
    t.add_argument("--out", required=True)
    c = sub.add_parser("control")
    c.add_argument("--build", default="build")
    c.add_argument("--python", default="")
    c.add_argument("--rate", type=float, default=100)
    c.add_argument("--cycles", type=int, default=1000)
    c.add_argument("--runs", type=int, default=3)
    c.add_argument("--stress-cpus", type=int, default=0)
    c.add_argument("--netem", default="")
    c.add_argument("--hold-ms", type=float, default=8)
    c.add_argument("--stop-ms", type=float, default=50)
    c.add_argument("--port", type=int, default=5700)
    c.add_argument("--note", default="")
    c.add_argument("--out", required=True)
    a = ap.parse_args()
    (cmd_transport if a.cmd == "transport" else cmd_control)(a)


if __name__ == "__main__":
    main()
