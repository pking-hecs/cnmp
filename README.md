# NAKR — Network-Aware KV-Cache Routing in ns-3

NAKR is a research prototype for evaluating routing decisions in a multi-node inference-serving network. It models whether a request should execute at a node that already holds a reusable KV cache, transfer the KV state from another node, or recompute the prefix on another node.

The repository contains experiments for TCP contention, synthetic routing-policy validation, TCP transfer-model calibration, and a multi-node simulation campaign.

> **Scope and interpretation:** This is an ns-3 simulation and analytical-model study, not a production LLM-serving system. It does not run real LLM inference or GPU kernels. Results describe the configured simulation, workloads, transport settings, and cost-model assumptions; they should not be presented as direct measurements of a real inference cluster.

## Contents

- [Research question](#research-question)
- [Routing actions and policies](#routing-actions-and-policies)
- [Repository layout](#repository-layout)
- [Requirements](#requirements)
- [Build](#build)
- [Run the experiments](#run-the-experiments)
- [Experiment phases](#experiment-phases)
- [Outputs](#outputs)
- [Reproducibility and resuming](#reproducibility-and-resuming)
- [Interpreting the results](#interpreting-the-results)
- [Known limitations](#known-limitations)
- [Troubleshooting](#troubleshooting)

## Research question

When a request arrives at a node that does not have the required prefix KV cache, is it better to:

1. execute at a node that already holds the KV state;
2. transfer the KV state over the network and then execute; or
3. recompute the prefix on another node?

The best choice depends on more than KV payload size. Relevant factors include the holder's queue, the target's queue, prefix length, link bandwidth and delay, concurrent transfers, background traffic, and how fresh the router's network telemetry is.

## Routing actions and policies

### Routing actions

- `EXECUTE_AT_HOLDER` — execute the request at the node that already has the reusable KV state.
- `PULL_KV` — transfer the KV state to the selected execution node, then execute.
- `RECOMPUTE` — execute on another node without transferring the existing KV state, recomputing the reusable prefix there.

The precise implementation and available command-line parameters are defined in the C++ programs under `ns-3.48/scratch/`.

### Policy families

Policy names used by the Phase 5 campaign include:

| Policy | Intended behavior |
|---|---|
| `load_based` | Load-oriented baseline. |
| `network_only` | Network-oriented baseline. |
| `cache_aware` | KV-cache-aware baseline that prefers reuse without the full queue/network cost comparison. |
| `cost_nopull` | Cost/queue-aware baseline that does not use KV pulls; it helps isolate queue and compute awareness from the benefit of moving KV over the network. |
| `cost_static` | Cost-aware policy with a static/nominal network model and KV pulls. |
| `nakr` | Cost-aware policy using live network telemetry for KV-pull estimates. |

The earlier Phase 3 policy-validation program also defines:

- `fixed_threshold` — chooses `PULL_KV` when the KV payload is at or below `thresholdBytes`; otherwise chooses `RECOMPUTE`.
- `cost_aware` — chooses the lowest nominal estimated cost among holder execution, KV pull, and recomputation.
- `network_aware` — chooses the lowest estimated cost after network-related adjustments, including throughput, in-flight pulls, and telemetry age.

Policy names are phase/program-specific; do not assume every executable supports every policy name.

## Repository layout

Paths below are relative to the repository root:

```text
.
├── README.md
└── ns-3.48/
    ├── scratch/
    │   ├── nakr-cluster.cc
    │   ├── nakr-cost-model.h
    │   ├── nakr-routing.cc
    │   ├── nakr-routing-v2.cc
    │   ├── nakr-smoke.cc
    │   ├── nakr-smoke-single.cc
    │   └── nakr-tcp-calibration.cc
    ├── scripts/
    │   └── run-phase4-5.sh
    ├── tools/
    │   ├── analyze_phases.py
    │   ├── phase4_calibrate.py
    │   ├── phase4_model_sweep.py
    │   ├── phase5_run_experiments.py
    │   └── phase5_analyze.py
    └── results/
        ├── phase1-3/
        ├── phase4/
        └── phase5/
```

The `ns-3.48/` directory is the ns-3 project root. Run ns-3 commands from that directory.

## Requirements

The experiments require:

- Linux or another environment supported by the included ns-3.48 source tree.
- A C++ compiler and the build dependencies required by ns-3.48.
- Python 3.
- Python packages used by analysis scripts: `numpy`, `pandas`, and `matplotlib`.

Install missing Python packages in your preferred environment, for example:

```bash
python3 -m pip install numpy pandas matplotlib
```

Follow the ns-3.48 installation documentation included in `ns-3.48/` for platform-specific build dependencies.

## Build

```bash
git clone https://github.com/pking-hecs/cnmp.git
cd cnmp/ns-3.48
./ns3 build
```

If the build system has not yet been configured on your machine, consult the bundled ns-3.48 documentation and configure it as needed before building.

To confirm that NAKR scratch binaries were built:

```bash
find build/scratch -maxdepth 1 -type f -name '*nakr*' -print
```

## Run the experiments

### Run phases individually

Running the scripts individually makes it easier to identify failures and avoids rerunning a long campaign unnecessarily. From the repository's `ns-3.48/` directory:

```bash
# Build C++ simulation programs
./ns3 build

# Analyze Phase 1–3 data already present in the repository
python3 tools/analyze_phases.py --repo . --out results/phase1-3

# Phase 4: TCP calibration and model/decision sweeps
python3 tools/phase4_calibrate.py --ns3dir . --out results/phase4
python3 tools/phase4_model_sweep.py --ns3dir . --out results/phase4

# Phase 5: multi-node experiment campaign
python3 tools/phase5_run_experiments.py --ns3dir . --out results/phase5

# Phase 5: summarize and plot results
python3 tools/phase5_analyze.py --dir results/phase5
```

Phase 4 calibration must run before the model sweep because the sweep reads `results/phase4/calibration-fit.json`. Phase 5 analysis expects `results/phase5/cluster-summary.csv`, generated by the Phase 5 campaign.

### Phase 5 options

```bash
python3 tools/phase5_run_experiments.py --help
```

Common options:

- `--ns3dir` — path to the ns-3 project directory.
- `--out` — output directory.
- `--requests` — number of requests per experiment; default `250`.
- `--seeds` — comma-separated random seeds; default `1,2,3,4,5`.
- `--only` — substring filter on scenario labels.

Example: run only scenarios whose labels contain `S1-load-100rps`:

```bash
python3 tools/phase5_run_experiments.py \
  --ns3dir . --out results/phase5 --only S1-load-100rps
```

The filter is a scenario-label substring, not a policy filter. The selected scenario still runs the policy/seed combinations defined by the campaign generator.

### Combined runner

The repository also contains `scripts/run-phase4-5.sh`. It is intended to build the project, analyze Phase 1–3, run Phase 4, optionally run Phase 5, and analyze Phase 5. Its `quick` mode skips the Phase 5 campaign and analyzes existing Phase 5 results:

```bash
bash scripts/run-phase4-5.sh quick
```

Full campaign:

```bash
bash scripts/run-phase4-5.sh
```

**Runner path note:** the script must set `NS3` to the directory containing `./ns3`. Since the script is in `ns-3.48/scripts/`, its project-root calculation already resolves to `ns-3.48`; set `NS3="$REPO"`, not `NS3="$REPO/ns-3.48"`. Until that script is corrected, use the individual commands above.

## Experiment phases

### Phase 1 — TCP contention experiments

Phase 1 investigates how TCP transfer performance changes under contention and different network/transport settings. These experiments provide background for modeling the cost of moving KV state. Use the checked-in Phase 1 result files and `tools/analyze_phases.py` for the precise parameters and measurements.

### Phase 2 — analytical cost model

Phase 2 introduces an analytical model for comparing execution at the KV holder, KV transfer, and recomputation. At this stage, estimates are synthetic and useful for exploring policy behavior, not measurements of real LLM inference latency.

### Phase 3 — deterministic policy validation

Phase 3 makes routing policies explicit and tests their choices under controlled cases. The fixed-threshold policy is a rule-based baseline; cost-aware policies compare estimated action costs. The model coefficients are synthetic placeholders. Phase 3 checks decision behavior; it does not perform real KV TCP transfers or model actual LLM/GPU inference.

### Phase 4 — TCP transfer-model calibration and decision sweeps

Phase 4 calibrates a transfer-time model against ns-3 TCP experiments. The calibration script runs:

1. A factorial investigation of a single-flow TCP throughput anomaly, varying buffer size, queue discipline, and device queue length.
2. A grid across transfer size, link bandwidth, delay, and concurrency using the transport configuration intended for Phase 5.
3. A train/held-out fit of a model of the form:

```text
T_ms = tcpStartupRtts * RTT_ms
      + tcpInvEfficiency * (n * bytes * 8 / (rate_mbps * 1000))
```

The coefficients and held-out error are saved in `calibration-fit.json`. The model sweep explores bandwidth break-even points and cases where live telemetry changes the decision relative to a static network estimate. Calibration accuracy applies to the tested grid and model form; it is not a universal guarantee for arbitrary networks.

### Phase 5 — multi-node routing evaluation

Phase 5 runs a multi-node simulation with four instances, Zipf-skewed shared prefixes, and TCP-based KV pulls. It compares simple baselines, cost-aware policies, and NAKR with live network telemetry.

The campaign generator defines these scenario families:

- **S1 — Load:** arrival rates of 40, 70, and 100 requests/second on a heterogeneous network.
- **S2 — Congestion:** 100 requests/second with background UDP traffic at configured levels.
- **S3 — Bandwidth:** 100 requests/second with homogeneous link rates of 1, 5, and 10 Gbps.
- **S4 — Bursty traffic / telemetry smoothing:** bursty background traffic and an EWMA smoothing-parameter ablation.

The runner appends results to `cluster-summary.csv` and skips scenario/policy/seed rows already present, allowing a campaign to resume after interruption. Report the exact scenarios, seeds, and request count used for each result set.

## Outputs

All paths below are relative to `ns-3.48/`, unless otherwise noted.

### Phase 1–3

The Phase 1–3 analyzer writes consolidated results and plots to `results/phase1-3/`. See the generated files and `tools/analyze_phases.py` for the current artifact list.

### Phase 4

Typical outputs include:

- `results/phase4/anomaly-factorial.csv` — TCP anomaly factorial experiment.
- `results/phase4/tcp-calibration-grid.csv` — transfer calibration measurements.
- `results/phase4/calibration-fit.json` — fitted coefficients and train/held-out error.
- `results/phase4/phase4-anomaly.png` — throughput anomaly plot.
- `results/phase4/phase4-transfer-model-fit.png` — measured-versus-predicted transfer times.
- `results/phase4/phase4-break-even.csv` and `phase4-break-even.png` — bandwidth break-even analysis.
- `results/phase4/phase4-decision-grid.csv` and `phase4-decision-map.png` — static versus telemetry-aware decisions.
- `results/phase4/phase4-findings.csv` — machine-readable findings.

### Phase 5

The Phase 5 analyzer generates:

- `results/phase5/cluster-summary.csv` — one row per scenario, policy, and seed.
- `results/phase5/per-request/` — per-request logs for the configured headline comparison.
- `results/phase5/phase5-results-by-scenario.csv` — metrics aggregated by scenario and policy.
- `results/phase5/phase5-scenario-verdicts.csv` — per-scenario comparisons and verdicts.
- `results/phase5/phase5-cost-model-validation.csv` — predicted versus actual pull-time validation.
- `results/phase5/phase5-findings.csv` — summary findings from experiment results.
- `results/phase5/phase5-ttft-p95.png` — p95 time-to-first-token comparisons.
- `results/phase5/phase5-ttft-cdf.png` — per-request TTFT distribution, when per-request logs are available.
- `results/phase5/phase5-pull-prediction.png` — predicted versus actual KV-pull time, when applicable.
- `results/phase5/phase5-ablation.png` — telemetry smoothing ablation, when corresponding data is present.

Use generated CSVs as the source of truth for numerical claims. Plots are summaries, not substitutes for reporting scenario, seed count, metrics, and baseline.

## Reproducibility and resuming

- Record the Git commit, ns-3 build configuration, command line, scenario filter, request count, and seeds alongside reported results.
- Keep generated CSVs and plots with the exact code revision that produced them.
- Phase 5 resumes by skipping `(scenario, policy, seed)` rows already in `cluster-summary.csv`. If code or parameters change, use a fresh output directory or archive the old summary first; otherwise old and new rows may be mixed.
- Do not compare campaigns with different seeds, request counts, transport settings, or policy definitions as identical experiments.
- Phase 4's `--skip-runs` option reuses the existing calibration CSV to refit and replot the model; it does not rerun the TCP grid.

Example fresh Phase 5 output directory:

```bash
python3 tools/phase5_run_experiments.py \
  --ns3dir . --out results/phase5-fresh --requests 250 --seeds 1,2,3,4,5
python3 tools/phase5_analyze.py --dir results/phase5-fresh
```

## Interpreting the results

Use tail latency (especially p95 and p99 time-to-first-token, TTFT) together with throughput, KV reuse, number of pulls, bytes transferred, and GPU/network utilization. A policy can improve on a cache-only baseline while still losing to a queue-aware policy that avoids network pulls.

The key Phase 5 comparison is not only whether `nakr` beats `cache_aware`, but also whether it beats `cost_nopull`. The latter helps determine whether moving KV over the network adds value beyond queue/compute awareness alone.

The reported analysis has shown a mixed outcome: NAKR improved p95 TTFT over the cache-aware baseline in the reported core scenarios, but the queue-aware no-pull baseline performed better in several scenarios. The current evidence therefore supports a benefit over the cache-aware baseline in the tested setup, but does **not** establish a general p95 TTFT benefit over a strong no-pull baseline. Re-run analysis against the exact result files before quoting counts or percentages.

When reporting results, include policy names, scenario and network configuration, arrival rate, request count and seeds, p50/p95/p99 TTFT, throughput and KV reuse/pull metrics, aggregation method, and seed-to-seed variation where available.

## Known limitations

- No real LLM inference, model weights, GPU kernels, or production serving stack are executed.
- Prefix processing and recomputation are represented by a model rather than measured inference execution.
- The transfer-time model is calibrated only against tested ns-3 configurations and may be inaccurate outside that range.
- Telemetry is simulated and has modeled age/smoothing behavior; it is not sourced from a real cluster monitoring system.
- Results depend on topology, transport settings, workload distribution, cost coefficients, seeds, and policy implementation.
- Lower TTFT in simulation does not by itself establish better production throughput, cost, fairness, energy use, or end-to-end service quality.

## Troubleshooting

### `nakr-cluster binary not found`

Build the ns-3 project first:

```bash
cd cnmp/ns-3.48
./ns3 build
```

### Phase 4 model sweep cannot find `calibration-fit.json`

Run calibration before the sweep:

```bash
python3 tools/phase4_calibrate.py --ns3dir . --out results/phase4
python3 tools/phase4_model_sweep.py --ns3dir . --out results/phase4
```

### Phase 5 analysis cannot find `cluster-summary.csv`

Run the Phase 5 campaign first, or point the analyzer at a directory containing an existing summary:

```bash
python3 tools/phase5_run_experiments.py --ns3dir . --out results/phase5
python3 tools/phase5_analyze.py --dir results/phase5
```

### A campaign appears to skip experiments

The runner skips rows already recorded in `cluster-summary.csv` for the same scenario, policy, and seed. Use a new output directory for a clean run, or carefully archive the old summary before starting over.

### Inspect command-line options

```bash
python3 tools/phase4_calibrate.py --help
python3 tools/phase4_model_sweep.py --help
python3 tools/phase5_run_experiments.py --help
python3 tools/phase5_analyze.py --help
```

## License

The ns-3 source tree retains its upstream licensing and notices. Review the license files shipped with `ns-3.48/` and this repository before redistributing modified source or generated artifacts.
