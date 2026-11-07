#!/usr/bin/env python3
"""Render results/*/*.json into Markdown tables (median across runs) and splice
them into README.md between <!-- RESULTS:BEGIN --> and <!-- RESULTS:END -->."""
import glob
import json
import os
import statistics as st

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def med(runs, key):
    v = [r[key] for r in runs if isinstance(r.get(key), (int, float))]
    return st.median(v) if v else None


def fmt(x, d=0):
    return "n/a" if x is None else f"{x:,.{d}f}"


def machine_line(d):
    m = d["machine"]
    if not m["cpu"] or m["cpu"] == "-":
        m = dict(m, cpu=f"virtual {m['arch']} CPU (VM on an Apple M1 host; guest does not expose the model)")
    la = ", ".join(f"{x:.1f}" for x in m["loadavg_start"])
    return (f"{m['cpu']} ({m['logical_cpus']} logical CPUs, {m['ram_gb']} GB), {m['os']}, "
            f"**{m['environment']}**; load average at start (1/5/15 min): {la}")


def transport_table(path):
    d = json.load(open(path))
    p = d["params"]
    out = [f"Machine: {machine_line(d)}.", "",
           f"Raw: `{os.path.relpath(path, ROOT)}` (+ `.raw.csv`). Command: `{d['command']}`. "
           f"Latency = round-trip time, {p['latency_samples']} samples/run at {p['latency_rate_hz']:.0f} Hz "
           f"after {p['warmup']} warm-up, {p['payload_bytes']} B payload, median of {p['runs']} runs. "
           f"Throughput = {p['throughput_seconds']} s unpaced send, median of runs. "
           "CPU = client + server CPU time / wall time, in % of one core.", "",
           "| Transport | p50 RTT (us) | p95 (us) | p99 (us) | jitter (us) | latency-run CPU (% core) | throughput (msg/s) | delivered | throughput-run CPU (% core) |",
           "|---|---|---|---|---|---|---|---|---|"]
    for name, c in d["cases"].items():
        lat = [r for r in c["latency"] if "error" not in r]
        thr = [r for r in c["throughput"] if "error" not in r]
        if not lat:
            out.append(f"| {name} | not measured: {c['latency'][0].get('error', '')[:60]} |||||||")
            continue
        out.append(f"| {name} | {fmt(med(lat,'p50_us'))} | {fmt(med(lat,'p95_us'))} | {fmt(med(lat,'p99_us'))} | "
                   f"{fmt(med(lat,'jitter_us'),1)} | {fmt(med(lat,'cpu_pct_of_one_core'),1)} | "
                   f"{fmt(med(thr,'msgs_per_s'))} | {fmt(100*(med(thr,'delivered_ratio') or 0),1)}% | "
                   f"{fmt(med(thr,'cpu_pct_of_one_core'),0)} |")
    return "\n".join(out)


def control_table(paths, title):
    out = []
    first = None
    rows = []
    for path in paths:
        d = json.load(open(path))
        first = first or d
        p = d["params"]
        scen = ("CPU stress (stress-ng, %d workers)" % p["stress_cpus"] if p["stress_cpus"] else "no stress")
        scen += (f" + netem `{p['netem']}` on lo" if p["netem"] else "")
        for name, runs in d["cases"].items():
            runs = [r for r in runs if "error" not in r]
            if not runs:
                continue
            pl = [r["plant"] for r in runs]
            rows.append(f"| {scen} | {name} | {fmt(med(runs,'p50_us'))} | {fmt(med(runs,'p95_us'))} | "
                        f"{fmt(med(runs,'p99_us'))} | {fmt(med(runs,'jitter_us'))} | "
                        f"{fmt(med(runs,'wake_late_p99_us'))} | {fmt(med(runs,'overrun_pct'),1)} | "
                        f"{fmt(med(runs,'controller_cpu_pct_of_one_core'),1)} | "
                        f"{fmt(med(pl,'held_or_stopped'))} / {fmt(med(pl,'commands'))} |")
    if not rows:
        return ""
    d = first
    p = d["params"]
    out = [f"Machine: {machine_line(d)}.", "",
           f"Raw: `{os.path.dirname(os.path.relpath(paths[0], ROOT))}/control_*.json` (+ `.raw.csv`). "
           f"{p['cycles']} cycles per run at {p['rate_hz']:.0f} Hz, {p['runs']} runs, medians. Plant watchdog "
           f"thresholds: HOLD {p['plant_hold_ms']} ms, STOP {p['plant_stop_ms']} ms. "
           "Loop RTT = send command until the plant's reply arrives; wake lateness = actual cycle start minus "
           "scheduled start; overrun = cycle finished after its deadline.", "",
           "| Scenario | Controller | RTT p50 (us) | p95 (us) | p99 (us) | jitter (us) | wake-late p99 (us) | overruns (%) | CPU (% core) | cmds held/stopped by plant / total |",
           "|---|---|---|---|---|---|---|---|---|---|"] + rows
    return "\n".join(out)


def build():
    parts = []
    for env, label in (("macos", "macOS native (Apple M1, no VM)"), ("docker", "Docker / Linux (Colima VM, 2 vCPU, 3 GB)")):
        d = os.path.join(ROOT, "results", env)
        sec = [f"#### {label}", ""]
        for t in sorted(glob.glob(os.path.join(d, "transport_*.json"))):
            sec += ["**Transport latency, jitter, throughput, CPU**", "", transport_table(t), ""]
        c = control_table(sorted(glob.glob(os.path.join(d, "control_*.json"))), label)
        if c:
            sec += ["**Python vs C++ control loop**", "", c, ""]
        if len(sec) > 2:
            parts.append("\n".join(sec))
    return "\n".join(parts)


if __name__ == "__main__":
    md = build()
    readme = os.path.join(ROOT, "README.md")
    s = open(readme).read()
    a, b = "<!-- RESULTS:BEGIN -->", "<!-- RESULTS:END -->"
    if a in s:
        s = s[:s.index(a) + len(a)] + "\n\n" + md + "\n" + s[s.index(b):]
        open(readme, "w").write(s)
        print("README updated")
    else:
        print(md)
