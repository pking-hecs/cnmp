#!/usr/bin/env python3
"""
NAKR Phase 5: run the multi-node evaluation campaign on nakr-cluster.

Scenarios (all use 4 instances, Zipf-skewed shared prefixes, real TCP KV pulls):
  S1-load        arrival rate 40/70/100 req/s on a heterogeneous network (10,10,2,2 Gbps)
  S2-congestion  rate 100, background UDP into the two fast links (0 / 3 / 6 Gbps)
  S3-bandwidth   rate 100, homogeneous links of 1 / 5 / 10 Gbps
  S4-bursty      rate 100, bursty background (0.5 s on / 0.5 s off); telemetry smoothing ablation

Policy ladder (each step adds one idea):  cache_aware -> cost_nopull (+queue/compute awareness)
-> cost_static (+KV pulls, static network model) -> nakr (+live network telemetry).

Every (scenario, policy, seed) row is appended to cluster-summary.csv. The run is resumable:
rows that already exist are skipped.

Usage:
  python3 tools/phase5_run_experiments.py --ns3dir ~/cnmp/ns-3.48 --out ~/cnmp/results/phase5
"""
import argparse, csv, glob, json, os, subprocess, sys, time

POLICIES = ["load_based", "cache_aware", "network_only", "cost_nopull", "cost_static", "nakr"]
HET = "10000,10000,2000,2000"


def find_bin(ns3dir):
    hits = sorted(glob.glob(os.path.join(ns3dir, "build", "scratch", "ns3.*-nakr-cluster-*")))
    if not hits:
        sys.exit("nakr-cluster binary not found - run ./ns3 build first")
    return hits[0]


def campaign(requests, seeds):
    jobs = []

    def add(scn, policy, seed, **kw):
        jobs.append(dict(scenario=scn, policy=policy, seed=seed, requests=requests, **kw))

    for seed in seeds:
        for rate in (40, 70, 100):
            for p in POLICIES:
                add(f"S1-load-{rate}rps", p, seed, rate=rate, linkMbps=HET)
        for bg in (3000, 6000):
            for p in POLICIES:
                add(f"S2-congestion-bg{bg}", p, seed, rate=100, linkMbps=HET, bgMbps=f"{bg},{bg},0,0")
        for L in (1000, 5000, 10000):
            for p in POLICIES:
                add(f"S3-bandwidth-{L}Mbps", p, seed, rate=100, linkMbps=",".join([str(L)] * 4))
        burst = dict(rate=100, linkMbps=HET, bgMbps="6000,6000,0,0", bgOnS=0.5, bgOffS=0.5)
        for p in ("cache_aware", "cost_nopull", "cost_static"):
            add("S4-bursty-ref", p, seed, **burst)
        for a in (1.0, 0.5, 0.2):
            add(f"S4-bursty-alpha{a}", "nakr", seed, ewmaAlpha=a, **burst)
    return jobs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ns3dir", default=os.path.expanduser("~/cnmp/ns-3.48"))
    ap.add_argument("--out", default=os.path.expanduser("~/cnmp/results/phase5"))
    ap.add_argument("--requests", type=int, default=250)
    ap.add_argument("--seeds", default="1,2,3,4,5")
    ap.add_argument("--only", default="", help="substring filter on scenario label")
    a = ap.parse_args()
    os.makedirs(os.path.join(a.out, "per-request"), exist_ok=True)
    b = find_bin(a.ns3dir)
    summary = os.path.join(a.out, "cluster-summary.csv")

    done = set()
    if os.path.exists(summary):
        for r in csv.DictReader(open(summary)):
            done.add((r["scenario"], r["policy"], r["seed"]))

    jobs = [j for j in campaign(a.requests, [int(s) for s in a.seeds.split(",")]) if a.only in j["scenario"]]
    t0 = time.time()
    for k, j in enumerate(jobs, 1):
        if (j["scenario"], j["policy"], str(j["seed"])) in done:
            continue
        args = [b, f"--summaryCsv={summary}"] + [f"--{key}={val}" for key, val in j.items()]
        # keep a per-request log for the headline comparison only
        if j["scenario"] == "S1-load-100rps" and j["seed"] == 1:
            args.append(f"--perRequestCsv={os.path.join(a.out, 'per-request', j['policy'] + '.csv')}")
        t = time.time()
        subprocess.run(args, check=True, capture_output=True)
        print(f"[{k}/{len(jobs)}] {j['scenario']:<24} {j['policy']:<13} seed={j['seed']}  "
              f"{time.time() - t:5.1f}s  (elapsed {time.time() - t0:.0f}s)", flush=True)


if __name__ == "__main__":
    main()
