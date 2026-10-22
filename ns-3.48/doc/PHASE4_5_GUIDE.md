# NAKR: Phases 1-5 guide

## Where everything goes (copy these into your repo root, `cnmp/`)

```
cnmp/
├── ns-3.48/scratch/                    NEW C++ programs (ns-3 builds each .cc into its own executable)
│   ├── nakr-cost-model.h               calibrated cost model shared by the two programs below
│   ├── nakr-tcp-calibration.cc         Phase 4: TCP experiment (buffer/qdisc/size/rate/delay/flows)
│   ├── nakr-routing-v2.cc              Phase 4: routing decision calculator on the calibrated model
│   └── nakr-cluster.cc                 Phase 5: 4-instance cluster, real TCP KV pulls, 6 routing policies
├── tools/                              NEW Python drivers (no ns-3 code here)
│   ├── analyze_phases.py               Phases 1-3 -> readable CSVs/plots (fixes the Phase 1 timing bug)
│   ├── phase4_calibrate.py             Phase 4: anomaly test, calibration grid, model fit + validation
│   ├── phase4_model_sweep.py           Phase 4: break-even bandwidth and static-vs-telemetry decision map
│   ├── phase5_run_experiments.py       Phase 5: 270-run campaign (resumable)
│   └── phase5_analyze.py               Phase 5: tables, verdicts, validation, figures
├── scripts/run-phase4-5.sh             NEW: one command that rebuilds and reruns everything
├── docs/PHASE4_5_GUIDE.md              this file
└── results/
    ├── phase1-3/                       regenerated Phase 1-3 outputs (clear CSVs + PNGs)
    ├── phase4/                         calibration + model sweeps
    └── phase5/                         campaign results (cluster-summary.csv is the raw data)
```
Your existing files (`nakr-smoke*.cc`, `nakr-routing.cc`, `results/nakr-concurrency.csv`, `results/phase2-test-matrix.csv`,
`results/phase3-test-cases.csv`, ...) are untouched and still needed.

## Run it
```bash
cd ns-3.48 && ./ns3 configure -d optimized && cd ..   # optimized build: the Phase 5 campaign is ~4x slower otherwise
bash scripts/run-phase4-5.sh          # full: ~30 min on one core (Phase 5 campaign dominates)
bash scripts/run-phase4-5.sh quick    # re-analyze existing results only
```
Single runs:
```bash
cd ns-3.48
./ns3 run "nakr-cluster --policy=nakr --rate=100 --requests=250"            # one policy
./ns3 run "nakr-routing-v2 --header=1 --prefixTokens=4096 --linkMbps=25000 --bgUtil=0.9 --holderQueueMs=800"
```
Needs: Python 3 with numpy, pandas, matplotlib. Repo hygiene: add `ns-3.48/cmake-cache/` and `ns-3.48/build/` to `.gitignore`
(cmake-cache is currently committed). `scripts/run-phase3-tests.sh` hardcodes `$HOME/cnmp`; the new script does not.

## What was done in each phase
**Phase 4 - make the cost model real.**
- *Anomaly solved:* one flow reached only 36% of 100 Mbps because ns-3's default 128 KiB TCP buffer caps a flow at
  ~48 Mbps on a 22 ms RTT path. 512 KiB+ buffers give ~86 Mbps. Qdisc/queue length made no difference at the small buffer.
- *Transfer model fitted on 54 ns-3 runs:* `T = 6.87 x RTT + 1.008 x bytes/rate`; 5.7% mean error on held-out runs.
- *Token-based cost model* (`nakr-cost-model.h`): KV bytes/token derived from architecture (128 KiB for a Llama-3-8B-like model);
  GPU speed (12k prefill tokens/s, 15 ms/token decode) are explicit, overridable assumptions.
- *Closed form:* pulling beats recomputing only when bandwidth > KV bytes/token x prefill tokens/s x 8 = ~12.7 Gbps here,
  independent of prefix length (`results/phase4/phase4-break-even.csv`).
- *Phase 3 gap closed:* live telemetry flips 9 of 30 pull decisions (`phase4-decision-map.png`).

**Phase 5 - multi-node simulation.** Four instances behind one switch (10/10/2/2 Gbps by default), Zipf-skewed shared
prefixes, per-instance LRU KV caches and FIFO prefill queues, real ns-3 TCP flows for every KV pull, optional background UDP,
and a telemetry loop built from per-link byte counters (20 ms windows, EWMA). Policies form a ladder:
`load_based`, `network_only`, `cache_aware` -> `cost_nopull` (+queue/compute awareness) -> `cost_static` (+pulls, nominal link
rates) -> `nakr` (+live telemetry + in-flight pull count). Same seeded workload for every policy; 5 seeds, 250 requests each.

## Results (5 seeds; p95 TTFT; details in results/phase5/*.csv)
| Scenario | Cache-aware | Queue-aware, no pulls | Static cost model | NAKR |
|---|---|---|---|---|
| 100 req/s, 10/10/2/2 Gbps | 3,027 ms | 1,089 | 3,605 | 1,187 |
| 100 req/s, all links 10 Gbps | 3,027 | 1,089 | 10,232 | 1,496 |
| 100 req/s, all links 1 Gbps | 3,027 | 1,089 | 1,056 | 1,060 |

1. **NAKR beats KV-cache-aware routing** on p95 TTFT in 8 of 8 scenarios (-51% to -79%).
2. **Live telemetry beats the static network model** in 6 of 8 (ties in 2). The static model launches many pulls into busy
   links (p95 up to 10 s). Mean pull-time error: static 61%, telemetry 38%.
3. **But most of that gain is not from the network.** A policy that is only queue/compute-aware (never pulls) is as good as or
   better than NAKR: NAKR is better in 0, tied in 3, worse in 5 of 8 scenarios (worse by 31-37% at 3-10 Gbps).
   In this setup the break-even bandwidth (~12.7 Gbps) is above most links, so recomputing is usually cheaper than pulling.
4. NAKR still under-predicts pull time by 28% on average (later pulls compete with earlier ones), which makes it pull
   slightly too often.
5. EWMA smoothing (alpha 0.2 vs 1.0: 1,271 vs 1,442 ms) is within seed noise (std ~700 ms): no demonstrated effect.

## Assumptions and limits (read before quoting numbers)
- GPU speed, decode time and workload (Zipf, 16 prefix groups of 1-4k tokens) are synthetic assumptions.
- Decode is not contended, so TPOT is a constant and is not evaluated; request routing messages cost no network time.
- Telemetry comes from ns-3 device byte counters, standing in for eBPF/switch counters.
- 4 instances, one switch, 250 requests per run; seed-to-seed variance is large for cache-aware routing.
- Suggested next steps: a calibrated pull-time penalty for in-cluster contention, faster-link (25-100 Gbps) scenarios
  where the break-even is exceeded, and TPOT with batched decode.

## Corrections to earlier outputs
- Phase 1: `last_byte_time_s` in `nakr-concurrency.csv` is an absolute time including the 1 s start offset. Earlier analysis
  overstated completion times by 1 s; the 8-sender slowdown at 10 Mbps is 7.95x (not 7.3x).
- `ns-3.48/results/phase2-decisions.log` is stale (older holder-cost formula); regenerated in `results/phase1-3/`.
