#!/usr/bin/env python3
"""
NAKR Phase 4: calibrate the KV-transfer cost model against ns-3 TCP.

Step 1  anomaly   Find out why one TCP flow only reached 36% of a 100 Mbps link in Phase 1.
                  Factorial test: TCP buffer x queue discipline x device queue length.
Step 2  grid      Sweep size x bandwidth x delay x concurrency with the tuned transport that
                  Phase 5 uses (8 MiB buffers, 9000-byte MTU, no CoDel).
Step 3  fit       Fit  T = a*RTT + b*(n*bits/rate)  on a training split, validate on held-out runs.

Outputs (in --out):
  anomaly-factorial.csv       tcp-calibration-grid.csv     calibration-fit.json
  phase4-anomaly.png          phase4-transfer-model-fit.png

Usage:
  python3 tools/phase4_calibrate.py --ns3dir ~/cnmp/ns-3.48 --out ~/cnmp/results/phase4
Needs the ns-3 project built first (./ns3 build) so build/scratch/ns3.48-nakr-tcp-calibration-* exists.
"""
import argparse, csv, glob, itertools, json, math, os, subprocess
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

COLS = ["senders", "bytes", "link_mbps", "delay_ms", "rtt_ms", "buf_bytes", "mss", "qdisc",
        "queue_pkts", "completion_s", "mean_flow_s", "goodput_mbps", "all_complete"]
plt.rcParams.update({"font.size": 12, "axes.spines.top": False, "axes.spines.right": False})


def find_bin(ns3dir, name):
    hits = sorted(glob.glob(os.path.join(ns3dir, "build", "scratch", f"ns3.*-{name}-*")))
    if not hits:
        raise SystemExit(f"binary for {name} not found under {ns3dir}/build/scratch - run ./ns3 build first")
    return hits[0]


def run(binpath, **kw):
    args = [binpath] + [f"--{k}={v}" for k, v in kw.items()]
    out = subprocess.run(args, capture_output=True, text=True, check=True).stdout.strip().splitlines()[-1]
    row = dict(zip(COLS, out.split(",")))
    for k in COLS:
        if k not in ("qdisc", "all_complete"):
            row[k] = float(row[k])
    return row


def write(path, rows):
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=COLS)
        w.writeheader()
        w.writerows(rows)


# ------------------------------------------------------------------ step 1
def anomaly(b, out):
    rows = []
    for buf, qd, q in itertools.product([131072, 524288, 4194304], ["default", "none"], [100, 1000]):
        r = run(b, senders=1, bytes=10 * 2**20, linkMbps=100, delayMs=10, bufBytes=buf, qdisc=qd, queuePackets=q)
        rows.append(r)
        print(f"  buf={buf:>8} qdisc={qd:<7} queue={q:>4} -> {r['goodput_mbps']:.1f} Mbps")
    write(os.path.join(out, "anomaly-factorial.csv"), rows)
    # figure: best goodput per buffer size (default qdisc, deep queue) so only the buffer varies
    fig, ax = plt.subplots(figsize=(7.5, 4.8))
    sel = [r for r in rows if r["qdisc"] == "default" and r["queue_pkts"] == 1000]
    labels = [f"{int(r['buf_bytes'] / 1024)} KiB" + ("\n(ns-3 default)" if r["buf_bytes"] == 131072 else "") for r in sel]
    vals = [r["goodput_mbps"] for r in sel]
    bars = ax.bar(labels, vals, color=["#e76f51", "#4c78a8", "#4c78a8"])
    ax.axhline(100, color="k", ls="--", lw=1)
    rtt = sel[0]["rtt_ms"]
    cap = 131072 * 8 / (rtt / 1000) / 1e6
    ax.axhline(cap, color="#e76f51", ls=":", lw=1.5)
    ax.text(2.45, cap + 2, f"128 KiB / {rtt:.0f} ms RTT = {cap:.0f} Mbps cap", ha="right", fontsize=9, color="#e76f51")
    for bar, v in zip(bars, vals):
        ax.text(bar.get_x() + bar.get_width() / 2, v + 1.5, f"{v:.0f}", ha="center")
    ax.set_ylim(0, 115); ax.set_ylabel("Single-flow goodput (Mbps)"); ax.set_xlabel("TCP send/receive buffer")
    ax.set_title("Phase 4: why one flow only used 36% of the 100 Mbps link\n(10 MiB transfer, dashed = link rate)", fontsize=12)
    fig.tight_layout(); fig.savefig(os.path.join(out, "phase4-anomaly.png"), dpi=160); plt.close(fig)
    return rows


# ------------------------------------------------------------------ step 2
def grid(b, out):
    rows = []
    for rate, d, size, n in itertools.product([100, 1000, 10000], [0.05, 1, 5], [4, 32, 256], [1, 4]):
        r = run(b, senders=n, bytes=size * 2**20, linkMbps=rate, delayMs=d, accessMbps=100000, accessDelayMs=0.05,
                bufBytes=8 * 2**20, mss=8948, mtu=9000, qdisc="none", queuePackets=512)
        rows.append(r)
        print(f"  {rate:>5} Mbps d={d:<4} {size:>3} MiB x{n}: {r['completion_s']:.3f}s  ({r['goodput_mbps']:.0f} Mbps)")
    write(os.path.join(out, "tcp-calibration-grid.csv"), rows)
    return rows


# ------------------------------------------------------------------ step 3
def features(r):
    n = r["senders"]
    ideal_ms = n * r["bytes"] * 8 / (r["link_mbps"] * 1000.0)
    return [r["rtt_ms"], ideal_ms]


def fit(rows):
    test = [r for i, r in enumerate(rows) if i % 3 == 0]
    train = [r for i, r in enumerate(rows) if i % 3 != 0]
    X = np.array([features(r) for r in train])
    y = np.array([r["completion_s"] * 1000 for r in train])
    w = 1.0 / y  # relative-error fit
    coef, *_ = np.linalg.lstsq(X * w[:, None], y * w, rcond=None)

    def pred(r):
        return float(np.dot(coef, features(r)))

    def mape(rs):
        return float(np.mean([abs(pred(r) - r["completion_s"] * 1000) / (r["completion_s"] * 1000) for r in rs]) * 100)

    return coef, mape(train), mape(test), pred, train, test


def plot_fit(rows, pred, coef, out):
    fig, ax = plt.subplots(figsize=(7.5, 6))
    colors = {100: "#4c78a8", 1000: "#2a9d8f", 10000: "#e76f51"}
    for rate in (100, 1000, 10000):
        rs = [r for r in rows if r["link_mbps"] == rate]
        ax.scatter([r["completion_s"] * 1000 for r in rs], [pred(r) for r in rs], s=34, color=colors[rate], label=f"{rate} Mbps")
    lo, hi = 3, 3e5
    ax.plot([lo, hi], [lo, hi], "k-", lw=1)
    ax.fill_between([lo, hi], [lo * .8, hi * .8], [lo * 1.2, hi * 1.2], color="grey", alpha=.15, label="+/-20%")
    ax.set_xscale("log"); ax.set_yscale("log"); ax.set_xlim(lo, hi); ax.set_ylim(lo, hi)
    ax.set_xlabel("Measured in ns-3 (ms)"); ax.set_ylabel("Model prediction (ms)")
    ax.set_title(f"Phase 4: transfer-time model vs ns-3\nT = {coef[0]:.2f} x RTT + {coef[1]:.3f} x (bytes / rate)", fontsize=12)
    ax.legend(frameon=False, loc="upper left"); fig.tight_layout()
    fig.savefig(os.path.join(out, "phase4-transfer-model-fit.png"), dpi=160); plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ns3dir", default=os.path.expanduser("~/cnmp/ns-3.48"))
    ap.add_argument("--out", default=os.path.expanduser("~/cnmp/results/phase4"))
    ap.add_argument("--skip-runs", action="store_true", help="reuse the CSVs already in --out")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    b = find_bin(a.ns3dir, "nakr-tcp-calibration")

    if a.skip_runs:
        rows = [dict(r) for r in csv.DictReader(open(os.path.join(a.out, "tcp-calibration-grid.csv")))]
        for r in rows:
            for k in COLS:
                if k not in ("qdisc", "all_complete"):
                    r[k] = float(r[k])
    else:
        print("Step 1: Phase 1 anomaly (single 100 Mbps flow)")
        anomaly(b, a.out)
        print("Step 2: calibration grid")
        rows = grid(b, a.out)
    print("Step 3: fit")
    coef, mtr, mte, pred, train, test = fit(rows)
    by_size = {}
    for r in rows:
        by_size.setdefault(int(r["bytes"] // 2**20), []).append(
            abs(pred(r) - r["completion_s"] * 1000) / (r["completion_s"] * 1000) * 100)
    res = {"model": "T_ms = tcpStartupRtts*RTT_ms + tcpInvEfficiency*(n*bytes*8/(rate_mbps*1000))",
           "tcpStartupRtts": round(float(coef[0]), 4), "tcpInvEfficiency": round(float(coef[1]), 4),
           "mape_train_pct": round(mtr, 2), "mape_heldout_pct": round(mte, 2),
           "mape_by_size_mib_pct": {k: round(float(np.mean(v)), 2) for k, v in sorted(by_size.items())},
           "n_train": len(train), "n_heldout": len(test),
           "transport": "8 MiB TCP buffers, MTU 9000 / MSS 8948, no queue disc, 512-packet device queue"}
    json.dump(res, open(os.path.join(a.out, "calibration-fit.json"), "w"), indent=2)
    plot_fit(rows, pred, coef, a.out)
    print(json.dumps(res, indent=2))


if __name__ == "__main__":
    main()
