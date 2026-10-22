#!/usr/bin/env python3
"""
NAKR Phase 5: turn results/phase5/cluster-summary.csv (+ per-request logs) into
readable tables, verdicts and figures.

Outputs (in --dir):
  phase5-results-by-scenario.csv   one row per scenario x policy (means over seeds)
  phase5-scenario-verdicts.csv     one row per scenario: who wins, NAKR vs each baseline, plain-English verdict
  phase5-cost-model-validation.csv predicted vs actual KV-pull time for cost_static and nakr
  phase5-findings.csv              the hypothesis questions answered from the numbers
  phase5-ttft-p95.png  phase5-ttft-cdf.png  phase5-pull-prediction.png  phase5-ablation.png
Usage: python3 tools/phase5_analyze.py --dir ~/cnmp/results/phase5
"""
import argparse, os
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

plt.rcParams.update({"font.size": 11, "axes.spines.top": False, "axes.spines.right": False})
POL = ["load_based", "network_only", "cache_aware", "cost_nopull", "cost_static", "nakr"]
LABEL = {"load_based": "Load-based", "network_only": "Network-only", "cache_aware": "KV-cache-aware", "cost_nopull": "Queue-aware, no pulls",
         "cost_static": "Cost-aware (static net)", "nakr": "NAKR (live telemetry)"}
COL = {"load_based": "#b0b7c3", "network_only": "#8d99ae", "cache_aware": "#4c78a8", "cost_nopull": "#2a9d8f",
       "cost_static": "#e9c46a", "nakr": "#e76f51"}
TIE = 5.0  # percent: differences smaller than this are reported as a tie


def pct(new, old):
    return (new - old) / old * 100.0 if old else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.expanduser("~/cnmp/results/phase5"))
    a = ap.parse_args()
    d = pd.read_csv(os.path.join(a.dir, "cluster-summary.csv"))
    metrics = ["ttft_mean_ms", "ttft_p50_ms", "ttft_p95_ms", "ttft_p99_ms", "e2e_p99_ms", "kv_reuse_rate",
               "local_hits", "pulls", "recomputes", "pulled_gib", "pull_actual_ms", "pull_pred_ms",
               "pull_mape_pct", "pull_bias_pct", "gpu_util_mean_pct", "gpu_util_max_pct",
               "net_down_util_max_pct", "throughput_rps"]
    g = d.groupby(["scenario", "policy"])
    m = g[metrics].mean().round(3)
    m["ttft_p95_seed_std_ms"] = g["ttft_p95_ms"].std().fillna(0).round(1)
    m["seeds"] = g.size()
    m = m.reset_index()

    # ---- per-scenario table with inference ----
    rows = []
    for scn, sub in m.groupby("scenario"):
        sub = sub.set_index("policy")
        best = sub["ttft_p95_ms"].idxmin()
        for p, r in sub.iterrows():
            row = r.to_dict()
            row.update(scenario=scn, policy=p, best_p95_policy=best,
                       p95_vs_best_pct=round(pct(r["ttft_p95_ms"], sub.loc[best, "ttft_p95_ms"]), 1))
            if "cache_aware" in sub.index:
                row["p95_vs_cache_aware_pct"] = round(pct(r["ttft_p95_ms"], sub.loc["cache_aware", "ttft_p95_ms"]), 1)
            rows.append(row)
    t1 = pd.DataFrame(rows)
    cols = ["scenario", "policy", "ttft_mean_ms", "ttft_p50_ms", "ttft_p95_ms", "ttft_p99_ms", "ttft_p95_seed_std_ms",
            "p95_vs_best_pct", "p95_vs_cache_aware_pct", "best_p95_policy", "kv_reuse_rate", "local_hits", "pulls",
            "recomputes", "pulled_gib", "gpu_util_mean_pct", "gpu_util_max_pct", "net_down_util_max_pct",
            "throughput_rps", "e2e_p99_ms", "seeds"]
    t1[[c for c in cols if c in t1.columns]].to_csv(os.path.join(a.dir, "phase5-results-by-scenario.csv"), index=False)

    # ---- verdicts ----
    ver = []
    for scn, sub in m.groupby("scenario"):
        sub = sub.set_index("policy")
        if "nakr" not in sub.index:
            continue
        n = sub.loc["nakr"]
        v = {"scenario": scn, "best_p95_policy": sub["ttft_p95_ms"].idxmin()}
        sents = []
        for base in ("cache_aware", "cost_nopull", "cost_static", "load_based"):
            if base in sub.index:
                b = sub.loc[base]
                dm, d95, d99 = (pct(n[k], b[k]) for k in ("ttft_mean_ms", "ttft_p95_ms", "ttft_p99_ms"))
                v[f"vs_{base}_mean_pct"] = round(dm, 1)
                v[f"vs_{base}_p95_pct"] = round(d95, 1)
                v[f"vs_{base}_p99_pct"] = round(d99, 1)
                if base != "load_based":
                    word = "ties" if abs(d95) < TIE else ("beats" if d95 < 0 else "loses to")
                    sents.append(f"NAKR {word} {LABEL[base]} on p95 TTFT ({d95:+.0f}%, p99 {d99:+.0f}%).")
        v["nakr_pulls"] = n["pulls"]
        v["nakr_pulled_gib"] = n["pulled_gib"]
        if sub["ttft_p95_ms"].idxmin() == "nakr":
            sents.append("NAKR has the lowest p95 TTFT.")
        elif sub["ttft_p95_ms"].idxmin() == "cache_aware" and n["pulls"] < 1:
            sents.append("NAKR chose not to pull at all, so it behaves like cache-aware here.")
        v["verdict"] = " ".join(sents)
        ver.append(v)
    ver = pd.DataFrame(ver)
    ver.to_csv(os.path.join(a.dir, "phase5-scenario-verdicts.csv"), index=False)

    # ---- cost-model validation ----
    val = m[m["policy"].isin(["cost_static", "nakr"]) & (m["pulls"] > 0)][
        ["scenario", "policy", "pulls", "pull_pred_ms", "pull_actual_ms", "pull_mape_pct", "pull_bias_pct"]].copy()
    val["reading"] = np.where(val["pull_bias_pct"] < -25, "underestimates pull time (decides too optimistically)",
                      np.where(val["pull_bias_pct"] > 25, "overestimates pull time (too cautious)", "within +/-25% on average"))
    val.round(1).to_csv(os.path.join(a.dir, "phase5-cost-model-validation.csv"), index=False)

    # ---- findings ----
    core = ver[ver["scenario"].str.startswith(("S1", "S2", "S3"))]
    wins_ca = (core["vs_cache_aware_p95_pct"] < -TIE).sum()
    ties_ca = (core["vs_cache_aware_p95_pct"].abs() <= TIE).sum()
    loss_ca = (core["vs_cache_aware_p95_pct"] > TIE).sum()
    wins_cs = (core["vs_cost_static_p95_pct"] < -TIE).sum()
    best_ca = core.loc[core["vs_cache_aware_p95_pct"].idxmin()]
    nk = m[(m.policy == "nakr") & (m.pulls > 0)]
    cs = m[(m.policy == "cost_static") & (m.pulls > 0)]
    wins_np = int((core["vs_cost_nopull_p95_pct"] < -TIE).sum())
    ties_np = int((core["vs_cost_nopull_p95_pct"].abs() <= TIE).sum())
    loss_np = int((core["vs_cost_nopull_p95_pct"] > TIE).sum())
    np_note = ("Most of NAKR's gain over cache-aware routing is already obtained WITHOUT moving any KV over the network "
               "(queue/compute awareness alone). In this setup the pull option, even with live telemetry, "
               "does not add p95 benefit beyond that."
               if (loss_np + ties_np) >= wins_np else
               "Pulling KV over the network adds p95 benefit beyond queue/compute awareness alone.")
    F = [
        ("Ablation", "What does the network-aware PULL option add beyond queue-aware routing (no pulls)?",
         f"NAKR vs queue-aware-no-pulls on p95 TTFT: better in {wins_np}, tie in {ties_np}, worse in {loss_np} of {len(core)}.",
         np_note),
        ("Hypothesis", "Does NAKR beat KV-cache-aware routing?",
         f"p95 TTFT: better in {wins_ca}, tie in {ties_ca}, worse in {loss_ca} of {len(core)} scenarios.",
         f"largest gain: {best_ca['scenario']} ({best_ca['vs_cache_aware_p95_pct']:+.0f}% p95, {best_ca['vs_cache_aware_p99_pct']:+.0f}% p99)"),
        ("Hypothesis", "Does live telemetry beat the static network model?",
         f"NAKR beats cost_static on p95 TTFT in {wins_cs} of {len(core)} scenarios.",
         f"mean pull-time error: static {cs['pull_mape_pct'].mean():.0f}% vs telemetry {nk['pull_mape_pct'].mean():.0f}%"),
        ("Cost model", "Is the pull-time prediction accurate inside the cluster?",
         f"NAKR average signed error {nk['pull_bias_pct'].mean():+.0f}% (negative = predicted too fast).",
         "later pulls compete with earlier ones; the router cannot know future arrivals when it decides"),
    ]
    abl = m[m.scenario.str.startswith("S4-bursty")]
    if len(abl):
        s4 = abl.set_index(["scenario", "policy"])["ttft_p95_ms"]
        parts = [f"{sc.replace('S4-bursty-', '')}: {v:.0f} ms" for (sc, p), v in s4.items() if p == "nakr"]
        F.append(("Prediction", "Does EWMA smoothing of telemetry help under bursty traffic?",
                  "p95 TTFT by alpha (1.0 = no smoothing): " + "; ".join(parts), "see phase5-ablation.png"))
    pd.DataFrame(F, columns=["topic", "question", "answer", "evidence"]).to_csv(
        os.path.join(a.dir, "phase5-findings.csv"), index=False)

    # ---- figure: p95 TTFT per scenario ----
    groups = [("Load (req/s)", ["S1-load-40rps", "S1-load-70rps", "S1-load-100rps"], ["40", "70", "100"]),
              ("Background load on fast links", ["S1-load-100rps", "S2-congestion-bg3000", "S2-congestion-bg6000"],
               ["0", "3 Gbps", "6 Gbps"]),
              ("Link bandwidth (all 4)", ["S3-bandwidth-1000Mbps", "S3-bandwidth-5000Mbps", "S3-bandwidth-10000Mbps"],
               ["1G", "5G", "10G"])]
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.9), sharey=True)
    for ax, (title, scns, labs) in zip(axes, groups):
        w = 0.14
        for i, p in enumerate(POL):
            vals = []
            for s in scns:
                x = m[(m.scenario == s) & (m.policy == p)]["ttft_p95_ms"]
                vals.append(float(x.iloc[0]) if len(x) else np.nan)
            ax.bar(np.arange(len(scns)) + (i - 2.5) * w, vals, w, color=COL[p], label=LABEL[p])
        ax.set_yscale("log"); ax.set_xticks(range(len(scns))); ax.set_xticklabels(labs); ax.set_xlabel(title)
    axes[0].set_ylabel("p95 TTFT (ms, log scale, lower is better)")
    axes[0].legend(frameon=False, fontsize=8, loc="upper left")
    fig.suptitle("Phase 5: tail time-to-first-token by routing policy", fontsize=13)
    fig.tight_layout(); fig.savefig(os.path.join(a.dir, "phase5-ttft-p95.png"), dpi=160); plt.close(fig)

    # ---- figure: TTFT CDF from per-request logs ----
    pr = os.path.join(a.dir, "per-request")
    if os.path.isdir(pr):
        fig, ax = plt.subplots(figsize=(8, 5))
        for p in POL:
            f = os.path.join(pr, p + ".csv")
            if os.path.exists(f):
                x = np.sort(pd.read_csv(f)["ttft_ms"].values)
                ax.plot(x, np.arange(1, len(x) + 1) / len(x), color=COL[p], lw=2.2, label=LABEL[p])
        ax.set_xscale("log"); ax.set_xlabel("TTFT (ms, log scale)"); ax.set_ylabel("Fraction of requests")
        ax.set_title("Phase 5: TTFT distribution, 100 req/s, heterogeneous network\n(further left = faster)", fontsize=12)
        ax.legend(frameon=False, fontsize=9, loc="lower right"); fig.tight_layout()
        fig.savefig(os.path.join(a.dir, "phase5-ttft-cdf.png"), dpi=160); plt.close(fig)

        # ---- figure: predicted vs actual pull time ----
        fig, ax = plt.subplots(figsize=(6.8, 6))
        any_pts = False
        for p in ("cost_static", "nakr"):
            f = os.path.join(pr, p + ".csv")
            if os.path.exists(f):
                r = pd.read_csv(f)
                r = r[r["kind"] == "PULL"]
                if len(r):
                    any_pts = True
                    ax.scatter(r["actual_pull_ms"], r["pred_pull_ms"], s=22, alpha=.7, color=COL[p], label=LABEL[p])
        if any_pts:
            lim = [20, 3e4]
            ax.plot(lim, lim, "k-", lw=1)
            ax.fill_between(lim, [l * .8 for l in lim], [l * 1.2 for l in lim], color="grey", alpha=.15)
            ax.set_xscale("log"); ax.set_yscale("log"); ax.set_xlim(lim); ax.set_ylim(lim)
            ax.set_xlabel("Actual KV-pull time in ns-3 (ms)"); ax.set_ylabel("Router's prediction at decision time (ms)")
            ax.set_title("Phase 5: pull-time prediction inside the cluster\n(below the line = predicted faster than reality)", fontsize=12)
            ax.legend(frameon=False)
            fig.tight_layout(); fig.savefig(os.path.join(a.dir, "phase5-pull-prediction.png"), dpi=160)
        plt.close(fig)

    # ---- figure: smoothing ablation ----
    if len(abl):
        fig, ax = plt.subplots(figsize=(7.8, 4.8))
        names, vals, cols = [], [], []
        for sc, lab, col in (("S4-bursty-ref", None, None),):
            pass
        order = [("S4-bursty-ref", "cache_aware", "KV-cache-aware", COL["cache_aware"]),
                 ("S4-bursty-ref", "cost_static", "Cost-aware (static)", COL["cost_static"]),
                 ("S4-bursty-alpha1.0", "nakr", "NAKR a=1.0\n(no smoothing)", COL["nakr"]),
                 ("S4-bursty-alpha0.5", "nakr", "NAKR a=0.5", COL["nakr"]),
                 ("S4-bursty-alpha0.2", "nakr", "NAKR a=0.2\n(heavy smoothing)", COL["nakr"])]
        for sc, p, lab, col in order:
            x = m[(m.scenario == sc) & (m.policy == p)]["ttft_p95_ms"]
            if len(x):
                names.append(lab); vals.append(float(x.iloc[0])); cols.append(col)
        bars = ax.bar(names, vals, color=cols)
        for b_, v in zip(bars, vals):
            ax.text(b_.get_x() + b_.get_width() / 2, v, f"{v:.0f}", ha="center", va="bottom", fontsize=9)
        ax.set_ylabel("p95 TTFT (ms)")
        ax.set_title("Phase 5: bursty background traffic (0.5 s on / 0.5 s off)\ntelemetry smoothing ablation", fontsize=12)
        plt.setp(ax.get_xticklabels(), fontsize=9); fig.tight_layout()
        fig.savefig(os.path.join(a.dir, "phase5-ablation.png"), dpi=160); plt.close(fig)

    print(ver[["scenario", "best_p95_policy", "verdict"]].to_string())
    print(pd.DataFrame(F, columns=["topic", "question", "answer", "evidence"]).to_string())


if __name__ == "__main__":
    main()
