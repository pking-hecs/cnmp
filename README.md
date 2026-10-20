# NAKR Phase 3 — Policy validation

## Purpose
This phase makes the three routing policies explicit and adds deterministic test cases.
It is still a synthetic analytical cost model. It does not yet perform KV TCP transfers
or model actual LLM/GPU inference.

## Policies
- `fixed_threshold`: uses only KV payload size. At or below `thresholdBytes`, choose
  `PULL_KV`; above the threshold, choose `RECOMPUTE`. It does not use load or telemetry.
- `cost_aware`: choose the lowest nominal estimated cost among holder execution, KV pull,
  and recomputation.
- `network_aware`: choose the lowest estimated cost after incorporating observed throughput,
  in-flight-pull penalty, and telemetry-age penalty into the pull estimate.

The cost coefficients are synthetic placeholders. They are useful for checking program
logic only and must not be presented as real LLM performance results.

## Install
The commands assume your project is `~/cnmp/ns-3.48` and the package was extracted to
`~/Downloads/nakr_phase3`. Change the source path if you extracted elsewhere.

```bash
cp ~/Downloads/nakr_phase3/scratch/nakr-routing.cc ~/cnmp/ns-3.48/scratch/nakr-routing.cc
mkdir -p ~/cnmp/ns-3.48/scripts ~/cnmp/ns-3.48/results
cp ~/Downloads/nakr_phase3/scripts/run-phase3-tests.sh ~/cnmp/ns-3.48/scripts/
chmod +x ~/cnmp/ns-3.48/scripts/run-phase3-tests.sh
```

## Run one test
```bash
cd ~/cnmp/ns-3.48
./ns3 run scratch/nakr-routing
```

## Run the test suite
```bash
bash ~/cnmp/ns-3.48/scripts/run-phase3-tests.sh
```

Output files:
- `results/phase3-policy-results.csv`
- `results/phase3-policy-tests.log`

Inspect them:
```bash
column -s, -t ~/cnmp/ns-3.48/results/phase3-policy-results.csv | less -S
cat ~/cnmp/ns-3.48/results/phase3-policy-tests.log
```

## Expected checks
- Case A: `fixed_threshold` selects `PULL_KV`.
- Case B: `fixed_threshold` selects `RECOMPUTE`.
- Case D should make recomputation very competitive and is expected to select `RECOMPUTE`
  for cost-aware and network-aware policies with the supplied parameters.
- Case E should make pulling competitive due to tiny payload and a heavily loaded holder.
- Inspect actual output; do not assume every policy must choose the same action.

## Manual parameter tests
```bash
./ns3 run "scratch/nakr-routing --kvBytes=1024 --thresholdBytes=4194304 --policy=all"
./ns3 run "scratch/nakr-routing --kvBytes=16777216 --recomputeMs=1 --holderLoad=0.95 --linkRateMbps=10 --policy=all"
```

## Completion criteria for Phase 3
- Builds successfully.
- All test cases run.
- Fixed-threshold behavior matches its defined rule.
- At least two different actions are selected across the suite.
- CSV and log are saved.
- Results are described as synthetic estimates, not network measurements.
