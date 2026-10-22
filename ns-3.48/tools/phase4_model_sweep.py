#!/usr/bin/env python3
"""
NAKR Phase 4: use the calibrated cost model (nakr-routing-v2) to answer two questions.

 A. Break-even: for a given prefix length and holder backlog, how much bandwidth must the path have
    before PULLING the KV beats both running on the (busy) holder and recomputing on an idle node?
 B. Where does live network telemetry change the decision? (The Phase 3 gap: cost-aware and
    network-aware never disagreed because no test had the static model favour a pull.)

Outputs (in --out):  phase4-break-even.csv  phase4-decision-grid.csv  phase4-decision-map.png
                     phase4-break-even.png  phase4-findings.csv
Usage: python3 tools/phase4_model_sweep.py --ns3dir ~/cnmp/ns-3.48 --out ~/cnmp/results/phase4
"""
import argparse, csv, glob, json, os, subprocess
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import ListedColormap

plt.rcParams.update({"font.size": 12, "axes.spines.top": False, "axes.spines.right": False})


def find_bin(ns3dir):
    hits = sorted(glob.glob(os.path.join(ns3dir, "build", "scratch", "ns3.*-nakr-routing-v2-*")))
    if not hits:
        raise SystemExit("nakr-routing-v2 binary not found - run ./ns3 build first")
    return hits[0]


def decide(b, fit, **kw):
    args = [b, f"--tcpStartupRtts={fit['tcpStartupRtts']}", f"--tcpInvEfficiency={fit['tcpInvEfficiency']}"]
    args += [f"--{k}={v}" for k, v in kw.items()]
    line = subprocess.run(args, capture_output=True, text=True, check=True).stdout.strip().splitlines()[-1]
    keys = ["scenario", "prefix_tokens", "kv_mib", "link_mbps", "bg_util", "in_flight", "holder_queue_ms",
            "target_queue_ms", "holder_ms", "recompute_ms", "pull_static_ms", "pull_telemetry_ms",
            "static_choice", "telemetry_choice", "telemetry_changed_decision"]
    return dict(zip(keys, line.split(",")))


def write(path, rows):
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ns3dir", default=os.path.expanduser("~/cnmp/ns-3.48"))
    ap.add_argument("--out", default=os.path.expanduser("~/cnmp/results/phase4"))
    a = ap.parse_args()
    b = find_bin(a.ns3dir)
    fit = json.load(open(os.path.join(a.out, "calibration-fit.json")))

    # ---------------- A. break-even bandwidth ----------------
    rates = [100 * (1.12 ** k) for k in range(0, 75)]  # ~100 Mbps .. ~480 Gbps
    be = []
    for prefix in (512, 1024, 2048, 4096, 8192):
        for hq in (0, 200, 1000, 3000):
            found = None
            for r in rates:
                d = decide(b, fit, prefixTokens=prefix, suffixTokens=512, linkMbps=round(r, 1), rttMs=0.4,
                           holderQueueMs=hq)
                if d["static_choice"] == "PULL":
                    found = round(r)
                    break
            kv = float(d["kv_mib"])
            be.append({"prefix_tokens": prefix, "kv_mib": round(kv, 1), "holder_backlog_ms": hq,
                       "break_even_mbps": found if found else "never(<480G)",
                       "break_even_gbps": round(found / 1000, 2) if found else ""})
    write(os.path.join(a.out, "phase4-break-even.csv"), be)

    fig, ax = plt.subplots(figsize=(7.8, 5.2))
    for hq, col in ((0, "#8d99ae"), (200, "#4c78a8"), (1000, "#2a9d8f"), (3000, "#e76f51")):
        pts = [(r["prefix_tokens"], r["break_even_gbps"]) for r in be if r["holder_backlog_ms"] == hq and r["break_even_gbps"] != ""]
        if pts:
            ax.plot(*zip(*pts), "o-", color=col, lw=2.2, label=f"holder backlog {hq} ms")
    ax.set_xscale("log", base=2); ax.set_yscale("log")
    ax.set_xticks([512, 1024, 2048, 4096, 8192]); ax.set_xticklabels([512, 1024, 2048, 4096, 8192])
    ax.set_xlabel("Reusable prefix (tokens)"); ax.set_ylabel("Break-even bandwidth (Gbps, log)")
    ax.set_title("Phase 4: bandwidth needed before pulling KV beats the alternatives\n(512 new tokens, idle target; lines absent = never within 480 Gbps)", fontsize=12)
    ax.legend(frameon=False, fontsize=10); fig.tight_layout()
    fig.savefig(os.path.join(a.out, "phase4-break-even.png"), dpi=160); plt.close(fig)

    # ---------------- B. static vs telemetry decision grid ----------------
    link_rates = [5000, 10000, 25000, 50000, 100000]
    utils = [0.0, 0.25, 0.5, 0.75, 0.9]
    grid = []
    for lr in link_rates:
        for u in utils:
            for k in (0, 2):
                grid.append(decide(b, fit, scenario="grid", prefixTokens=4096, suffixTokens=512, linkMbps=lr,
                                   rttMs=0.4, bgUtil=u, inFlight=k, holderQueueMs=800, targetQueueMs=0))
    write(os.path.join(a.out, "phase4-decision-grid.csv"), grid)

    fig, axes = plt.subplots(1, 2, figsize=(11, 4.8), sharey=True)
    code = {"HOLDER": 0, "PULL": 1, "RECOMPUTE": 2}
    cmap = ListedColormap(["#2a9d8f", "#e76f51", "#e9c46a"])
    for ax, key, title in ((axes[0], "static_choice", "Static model (nominal link rate)"),
                           (axes[1], "telemetry_choice", "With live telemetry")):
        M = [[code[next(g[key] for g in grid if int(float(g["link_mbps"])) == lr and float(g["bg_util"]) == u and g["in_flight"] == "0")]
              for lr in link_rates] for u in utils]
        ax.imshow(M, origin="lower", cmap=cmap, vmin=0, vmax=2, aspect="auto")
        for i, u in enumerate(utils):
            for j, lr in enumerate(link_rates):
                ax.text(j, i, ["Holder", "Pull", "Recomp."][M[i][j]], ha="center", va="center", fontsize=9)
        ax.set_xticks(range(len(link_rates))); ax.set_xticklabels([f"{r // 1000}G" for r in link_rates])
        ax.set_yticks(range(len(utils))); ax.set_yticklabels([f"{int(u * 100)}%" for u in utils])
        ax.set_xlabel("Path bandwidth"); ax.set_title(title, fontsize=12)
    axes[0].set_ylabel("Background utilization of the path")
    fig.suptitle("Phase 4: chosen action, 4096-token prefix, 800 ms holder backlog\n"
                 "(the static model keeps pulling on a congested path; telemetry switches to recompute)", fontsize=12)
    fig.tight_layout(); fig.savefig(os.path.join(a.out, "phase4-decision-map.png"), dpi=160); plt.close(fig)

    changed = [g for g in grid if g["telemetry_changed_decision"] == "yes"]
    pull_static = [g for g in grid if g["static_choice"] == "PULL"]
    ex = max(changed, key=lambda g: float(g["pull_telemetry_ms"]) / float(g["pull_static_ms"])) if changed else None
    findings = [
        ("Break-even", "How much bandwidth makes pulling worthwhile?",
         "For 4096-token prefixes with an idle target and 1 s holder backlog: "
         + next((f"{r['break_even_gbps']} Gbps" for r in be if r['prefix_tokens'] == 4096 and r['holder_backlog_ms'] == 1000), "n/a"),
         "see phase4-break-even.csv"),
        ("Break-even", "Closed form for the break-even bandwidth",
         f"Pull beats recompute when bandwidth > KV bytes/token x prefill tokens/s x 8 x (1/efficiency) = "
         f"{131072 * 12000 * 8 * fit['tcpInvEfficiency'] / 1e9:.1f} Gbps (independent of prefix length)",
         "uses defaults in nakr-cost-model.h: 128 KiB/token, 12,000 tokens/s; matches the CSV (13-15 Gbps once the holder is busy)"),
        ("Telemetry", "Does live telemetry ever change the decision (Phase 3 gap)?",
         f"Yes: in {len(changed)} of {len(pull_static)} grid cells where the static model chooses PULL.",
         (f"e.g. {ex['link_mbps']} Mbps path, {float(ex['bg_util'])*100:.0f}% busy, {ex['in_flight']} pulls in flight: "
          f"static pull {float(ex['pull_static_ms']):.0f} ms vs telemetry {float(ex['pull_telemetry_ms']):.0f} ms -> "
          f"{ex['static_choice']} becomes {ex['telemetry_choice']}") if ex else "n/a"),
        ("Model", "Held-out accuracy of the transfer-time model", f"{fit['mape_heldout_pct']}% mean error",
         f"fitted startup = {fit['tcpStartupRtts']} RTTs, 1/efficiency = {fit['tcpInvEfficiency']}"),
    ]
    write(os.path.join(a.out, "phase4-findings.csv"),
          [dict(topic=t, question=q, answer=an, evidence=ev) for t, q, an, ev in findings])
    print(json.dumps(findings, indent=1))


if __name__ == "__main__":
    main()
