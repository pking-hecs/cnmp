#!/usr/bin/env python3
"""
NAKR phases 1-3: turn raw results into readable CSVs + figures with the inference spelled out.

NOTE (fixed in Phase 4): nakr-concurrency.csv's last_byte_time_s is an absolute timestamp that includes the
1.0 s sender start offset, so the transfer duration is last_byte_time_s - 1.0. Earlier versions of this
script treated it as a duration (overstating every completion time by 1 s).

Inputs (repo results/ folder): nakr-concurrency.csv, phase2-test-matrix.csv, phase3-test-cases.csv
Phase 2/3 decisions come from running the repo's own scratch/nakr-routing.cc (nothing re-implemented),
so the numbers always match the C++ code.

Usage:
  python3 analyze_phases.py --repo ~/cnmp --out ~/cnmp/results/phase1-3
  NAKR_BIN=/path/to/compiled/nakr-routing   (optional; otherwise uses ./ns3 run in ns-3.48)
"""
import argparse, csv, os, re, subprocess
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SHORT = {"EXECUTE_AT_HOLDER": "Holder", "PULL_KV": "Pull", "RECOMPUTE": "Recompute"}
START_OFFSET_S = 1.0  # nakr-smoke.cc starts every sender at t = 1.0 s; last_byte_time_s is an ABSOLUTE timestamp
ACT = {"EXECUTE_AT_HOLDER": "holder", "PULL_KV": "pull", "RECOMPUTE": "recompute"}
ARG_COLS = ["kvBytes", "thresholdBytes", "linkRateMbps", "linkDelayMs", "recomputeMs", "holderLoad",
            "inFlightPulls", "destinationLoad", "observedThroughputMbps", "telemetryAgeMs"]
C = {"holder": "#2a9d8f", "pull": "#e76f51", "pull_aware": "#9d2f1a", "recompute": "#e9c46a",
     "fixed": "#8d99ae", "cost": "#4c78a8", "network": "#f58518"}
plt.rcParams.update({"font.size": 12, "axes.spines.top": False, "axes.spines.right": False})


def read_csv(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def write_csv(path, rows):
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)


# ---------------------------------------------------------------- Phase 1
def phase1(rows, out):
    data = [{k: (v if k == "all_complete" else float(v)) for k, v in r.items()} for r in rows]
    for d in data:
        d["duration_s"] = d["last_byte_time_s"] - START_OFFSET_S  # real transfer time
    base = {d["link_rate_mbps"]: d["duration_s"] for d in data if d["senders"] == 1}
    res = []
    for d in data:
        link, n = d["link_rate_mbps"], int(d["senders"])
        total_mbit = d["aggregate_bytes"] * 8 / 1e6
        util = d["aggregate_throughput_mbps"] / link * 100
        slow = d["duration_s"] / base[link]
        state = "SATURATED" if util >= 85 else "UNDERUSED"
        if state == "SATURATED":
            inf = (f"Link is full ({util:.0f}%). Each sender gets only {d['aggregate_throughput_mbps']/n:.1f} Mbps, "
                   f"so {n} transfers take {slow:.1f}x as long as one.")
        else:
            inf = (f"Link only {util:.0f}% used. A single flow cannot fill it, so {n} transfers take "
                   f"{slow:.2f}x the single-transfer time while moving {n}x the data.")
        res.append({
            "link_rate_mbps": int(link), "senders": n,
            "total_data_mib": round(d["aggregate_bytes"] / 2**20, 1),
            "aggregate_throughput_mbps": round(d["aggregate_throughput_mbps"], 2),
            "link_utilization_pct": round(util, 1),
            "per_sender_throughput_mbps": round(d["aggregate_throughput_mbps"] / n, 2),
            "completion_time_s": round(d["duration_s"], 2),
            "ideal_time_s_if_link_full": round(total_mbit / link, 2),
            "slowdown_vs_single_transfer": round(slow, 2),
            "link_state": state, "all_transfers_complete": d["all_complete"],
            "inference": inf})
    write_csv(os.path.join(out, "phase1-tcp-analysis.csv"), res)

    fig, ax = plt.subplots(figsize=(8, 5.2))
    for link, col in ((10, "#4c78a8"), (100, "#e76f51")):
        pts = [r for r in res if r["link_rate_mbps"] == link]
        ax.plot([p["senders"] for p in pts], [p["aggregate_throughput_mbps"] for p in pts], "o-", color=col, lw=2.5,
                label=f"{link} Mbps bottleneck")
        ax.axhline(link, color=col, ls="--", lw=1, alpha=.6)
        for p in pts:
            ax.annotate(f"{p['link_utilization_pct']:.0f}%", (p["senders"], p["aggregate_throughput_mbps"]),
                        textcoords="offset points", xytext=(0, -17 if link == 100 else 8), ha="center",
                        fontsize=10, color=col)
    ax.set_xscale("log", base=2); ax.set_xticks([1, 2, 4, 8]); ax.set_xticklabels([1, 2, 4, 8])
    ax.set_yscale("log"); ax.set_yticks([10, 20, 50, 100]); ax.set_yticklabels([10, 20, 50, 100])
    ax.set_xlabel("Concurrent senders (10 MiB each)"); ax.set_ylabel("Aggregate throughput (Mbps, log scale)")
    ax.set_title("Phase 1: how much of the link is actually used\n(dashed = link capacity, labels = utilization)", fontsize=13)
    ax.legend(loc="center right", frameon=False); fig.tight_layout()
    fig.savefig(os.path.join(out, "phase1-throughput.png"), dpi=160); plt.close(fig)

    fig, axes = plt.subplots(1, 2, figsize=(10, 4.8))
    for ax, link in zip(axes, (10, 100)):
        pts = [r for r in res if r["link_rate_mbps"] == link]
        xs = range(len(pts)); w = 0.38
        ax.bar([x - w / 2 for x in xs], [p["ideal_time_s_if_link_full"] for p in pts], w, color="#bdbdbd",
               label="Ideal (link 100% full)")
        ax.bar([x + w / 2 for x in xs], [p["completion_time_s"] for p in pts], w,
               color="#4c78a8" if link == 10 else "#e76f51", label="Measured")
        for x, p in zip(xs, pts):
            ax.text(x + w / 2, p["completion_time_s"],
                    f"{p['completion_time_s']:.1f}s\n({p['slowdown_vs_single_transfer']:.1f}x)",
                    ha="center", va="bottom", fontsize=9)
        ax.set_xticks(list(xs)); ax.set_xticklabels([p["senders"] for p in pts])
        ax.set_ylim(0, max(p["completion_time_s"] for p in pts) * 1.25)
        ax.set_xlabel("Concurrent senders"); ax.set_title(f"{link} Mbps bottleneck", fontsize=12)
    axes[0].set_ylabel("Time until last byte arrives (s)"); axes[0].legend(frameon=False, loc="upper left", fontsize=9)
    fig.suptitle("Phase 1: transfer time, measured vs ideal (x = slowdown vs one transfer)", fontsize=13)
    fig.tight_layout(); fig.savefig(os.path.join(out, "phase1-completion-time.png"), dpi=160); plt.close(fig)
    return res


# ---------------------------------------------------------------- Phases 2 & 3
def run_case(row, repo):
    args = [f"--{k}={row[k]}" for k in ARG_COLS] + ["--policy=all"]
    binpath = os.environ.get("NAKR_BIN")
    if binpath:
        txt = subprocess.run([binpath] + args, capture_output=True, text=True, check=True).stdout
    else:
        cmd = ["./ns3", "run", "scratch/nakr-routing " + " ".join(args)]
        txt = subprocess.run(cmd, cwd=os.path.join(repo, "ns-3.48"), capture_output=True, text=True, check=True).stdout
    pat = r"holder=([\d.]+) pull=([\d.]+) recompute=([\d.]+)"
    nom = re.search(r"Nominal costs \(ms\): " + pat, txt).groups()
    awr = re.search(r"Network-aware costs \(ms\): " + pat, txt).groups()
    pol = {m.group(1): m.group(2)
           for m in re.finditer(r"^(fixed_threshold|cost_aware|network_aware),(\w+),", txt, re.M)}
    f = lambda t: dict(zip(("holder", "pull", "recompute"), map(float, t)))
    return f(nom), f(awr), pol


def decision_rows(cases, repo, label_col):
    res = []
    for c in cases:
        nom, awr, pol = run_case(c, repo)
        real = lambda a: awr[ACT[a]]          # cost of a choice under the observed network
        ranked = sorted(awr.items(), key=lambda kv: kv[1])
        best_name, best = ranked[0]
        runner_name, runner = ranked[1]
        fixed_extra = real(pol["fixed_threshold"]) - best
        cost_extra = real(pol["cost_aware"]) - best
        net_extra = real(pol["network_aware"]) - best

        parts = [f"Best option is {best_name} ({best:.0f} ms), {runner - best:.0f} ms ahead of {runner_name}."]
        if fixed_extra > 0.5:
            parts.append(f"Fixed-threshold chose {SHORT[pol['fixed_threshold']]} and pays {fixed_extra:.0f} ms extra "
                         f"({real(pol['fixed_threshold'])/best:.1f}x the best).")
        else:
            parts.append("Fixed-threshold also reaches the best option.")
        if pol["cost_aware"] != pol["network_aware"]:
            parts.append("Telemetry CHANGED the decision: cost-aware chose pull, but live network state made it too costly.")
        elif pol["cost_aware"] == "PULL_KV":
            parts.append(f"Pull still wins after the network penalty ({awr['pull'] - nom['pull']:+.2f} ms).")
        else:
            parts.append(f"Cost-aware already rejects pull (it is {nom['pull']/best:.1f}x the best on nominal numbers). "
                         "Telemetry can only make pull costlier, so network-awareness cannot change this decision.")

        res.append({
            "case": c["case"], label_col: c[label_col],
            "kv_mib": round(float(c["kvBytes"]) / 2**20, 4), "link_mbps": c["linkRateMbps"],
            "observed_mbps": c["observedThroughputMbps"], "pulls_in_flight": c["inFlightPulls"],
            "holder_load": c["holderLoad"],
            "holder_ms": awr["holder"], "recompute_ms": awr["recompute"],
            "pull_nominal_ms": nom["pull"], "pull_network_aware_ms": awr["pull"],
            "pull_underestimated_by_x": round(awr["pull"] / nom["pull"], 2),
            "fixed_threshold_choice": SHORT[pol["fixed_threshold"]],
            "cost_aware_choice": SHORT[pol["cost_aware"]],
            "network_aware_choice": SHORT[pol["network_aware"]],
            "best_option": best_name.capitalize(), "best_cost_ms": round(best, 3),
            "fixed_extra_ms_vs_best": round(fixed_extra, 3),
            "cost_aware_extra_ms_vs_best": round(cost_extra, 3),
            "network_aware_extra_ms_vs_best": round(net_extra, 3),
            "telemetry_changed_decision": "yes" if pol["cost_aware"] != pol["network_aware"] else "no",
            "inference": " ".join(parts)})
    return res


def fig_phase2(rows, out):
    fig, ax = plt.subplots(figsize=(9, 5.4))
    series = [("holder_ms", "Execute at holder", C["holder"]), ("recompute_ms", "Recompute", C["recompute"]),
              ("pull_nominal_ms", "Pull (nominal model)", C["pull"]),
              ("pull_network_aware_ms", "Pull (network-aware)", C["pull_aware"])]
    n = len(rows); w = 0.2
    for i, (key, lab, col) in enumerate(series):
        ax.bar([j + (i - 1.5) * w for j in range(n)], [r[key] for r in rows], w, color=col, label=lab)
    ax.set_yscale("log"); ax.set_ylim(10, 20000)
    ax.set_xticks(range(n)); ax.set_xticklabels([r["case"].replace("-", "\n", 1) + f"\nBEST: {r['best_option']}" for r in rows], fontsize=10)
    ax.set_ylabel("Estimated cost (ms, log scale)")
    ax.set_title("Phase 2: estimated cost of each option per scenario\n(lowest bar wins; model is synthetic)", fontsize=13)
    ax.legend(frameon=False, fontsize=9, ncol=2, loc="upper left"); fig.tight_layout()
    fig.savefig(os.path.join(out, "phase2-cost-comparison.png"), dpi=160); plt.close(fig)


def fig_phase3(rows, out):
    fig, ax = plt.subplots(figsize=(10, 5.4))
    pols = [("fixed_threshold_choice", "fixed_extra_ms_vs_best", "Fixed threshold", C["fixed"]),
            ("cost_aware_choice", "cost_aware_extra_ms_vs_best", "Cost-aware", C["cost"]),
            ("network_aware_choice", "network_aware_extra_ms_vs_best", "Network-aware", C["network"])]
    n = len(rows); w = 0.26
    for i, (chk, ek, lab, col) in enumerate(pols):
        xs = [j + (i - 1) * w for j in range(n)]
        vals = [r["best_cost_ms"] + r[ek] for r in rows]
        ax.bar(xs, vals, w, color=col, label=lab)
        for x, v, r in zip(xs, vals, rows):
            ax.text(x, v, f"{r[chk].replace('Recompute', 'Recomp.')}\n{v:.0f}", ha="center", va="bottom", fontsize=8)
    ax.set_xticks(range(n)); ax.set_xticklabels([r["case"].replace("-", "\n") for r in rows], fontsize=9)
    ax.set_ylim(0, max(r["best_cost_ms"] + r["fixed_extra_ms_vs_best"] for r in rows) * 1.3)
    ax.set_ylabel("Cost of the chosen action (ms)")
    ax.set_title("Phase 3: what each policy chose and what it costs\n(cost-aware and network-aware coincide in all cases)", fontsize=13)
    ax.legend(frameon=False, loc="upper right"); fig.tight_layout()
    fig.savefig(os.path.join(out, "phase3-policy-comparison.png"), dpi=160); plt.close(fig)


def findings(p1, p2, p3, out):
    s10 = [r for r in p1 if r["link_rate_mbps"] == 10]
    f100 = [r for r in p1 if r["link_rate_mbps"] == 100]
    f = []
    f.append(("1", "Does a shared link get saturated?", "10 Mbps: yes, always. 100 Mbps: only with 4+ senders.",
              f"10 Mbps utilization {min(r['link_utilization_pct'] for r in s10):.0f}-{max(r['link_utilization_pct'] for r in s10):.0f}%; "
              f"100 Mbps goes {f100[0]['link_utilization_pct']:.0f}% -> {f100[-1]['link_utilization_pct']:.0f}% for 1 -> 8 senders"))
    f.append(("1", "What does concurrency cost a transfer?", "On a saturated link, N pulls take about N times as long.",
              f"10 Mbps: 8 senders finish in {s10[-1]['completion_time_s']}s vs {s10[0]['completion_time_s']}s alone ({s10[-1]['slowdown_vs_single_transfer']}x)"))
    f.append(("1", "Why does one flow underuse the 100 Mbps link?", "Resolved in Phase 4: ns-3's default 128 KiB TCP buffer caps a flow at about 48 Mbps on a 22 ms RTT path.",
              f"1 sender reaches {f100[0]['aggregate_throughput_mbps']} Mbps; with a 512 KiB+ buffer it reaches ~85 Mbps (results/phase4/anomaly-factorial.csv)"))
    c2 = sum(r["telemetry_changed_decision"] == "yes" for r in p2)
    f.append(("2", "Do telemetry penalties raise pull cost?", "Yes, in every scenario.",
              "Pull estimate rises by " + ", ".join(f"{r['pull_underestimated_by_x']}x" for r in p2) + " (scenario order)"))
    f.append(("2", "Did telemetry change any decision?", f"{'Yes' if c2 else 'No'} ({c2} of {len(p2)} scenarios).",
              "The nominal model already rejects pull everywhere, and telemetry only makes pull costlier"))
    fx2 = [r for r in p2 if r["fixed_extra_ms_vs_best"] > 0.5]
    f.append(("2", "Does the fixed-size threshold choose well?", f"No, it picks the wrong option in {len(fx2)} of {len(p2)} scenarios.",
              "; ".join(f"{r['case']}: +{r['fixed_extra_ms_vs_best']:.0f} ms" for r in fx2)))
    f.append(("2", "Caveat on the saved log", "ns-3.48/results/phase2-decisions.log is stale; it used an older holder-cost formula.",
              "Regenerated here with the current nakr-routing.cc"))
    fx3 = [r for r in p3 if r["fixed_extra_ms_vs_best"] > 0.5]
    f.append(("3", "Does cost-aware beat the fixed-size threshold?", f"Yes, in {len(fx3)} of {len(p3)} cases.",
              "; ".join(f"{r['case']}: fixed pays {r['fixed_extra_ms_vs_best']:.0f} ms extra" for r in fx3)))
    c3 = sum(r["telemetry_changed_decision"] == "yes" for r in p3)
    f.append(("3", "Does network-aware beat cost-aware?", f"Not shown yet ({c3} of {len(p3)} cases differ).",
              "Telemetry only raises pull cost, so a test must have the nominal model favour pull while congestion makes it a bad choice"))
    f.append(("all", "Caveat", "Phase 2/3 numbers are synthetic model estimates, not measurements.",
              "Cost coefficients are placeholders (see README)"))
    write_csv(os.path.join(out, "findings-summary.csv"),
              [dict(phase=a, question=b, answer=c, evidence=d) for a, b, c, d in f])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.path.expanduser("~/cnmp"))
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    out = a.out or os.path.join(a.repo, "results", "phase1-3")
    os.makedirs(out, exist_ok=True)
    res = os.path.join(a.repo, "results")
    p1 = phase1(read_csv(os.path.join(res, "nakr-concurrency.csv")), out)
    p2 = decision_rows(read_csv(os.path.join(res, "phase2-test-matrix.csv")), a.repo, "expected_stress")
    write_csv(os.path.join(out, "phase2-decisions-analysis.csv"), p2); fig_phase2(p2, out)
    p3 = decision_rows(read_csv(os.path.join(res, "phase3-test-cases.csv")), a.repo, "purpose")
    write_csv(os.path.join(out, "phase3-policy-analysis.csv"), p3); fig_phase3(p3, out)
    findings(p1, p2, p3, out)
    print("wrote", sorted(os.listdir(out)))


if __name__ == "__main__":
    main()
