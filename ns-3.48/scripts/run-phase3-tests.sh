#!/usr/bin/env bash
set -euo pipefail

PROJECT="$HOME/cnmp/ns-3.48"
RESULTS="$PROJECT/results"
mkdir -p "$RESULTS"
cd "$PROJECT"

CSV="$RESULTS/phase3-policy-results.csv"
LOG="$RESULTS/phase3-policy-tests.log"
printf 'policy,kvBytes,thresholdBytes,linkRateMbps,linkDelayMs,recomputeMs,holderLoad,inFlightPulls,destinationLoad,observedThroughputMbps,telemetryAgeMs,action,estimatedCostMs\n' > "$CSV"
: > "$LOG"

run_case() {
  local name="$1"; shift
  echo "===== $name =====" | tee -a "$LOG"
  ./ns3 run "scratch/nakr-routing $* --policy=all --csvPath=$CSV" 2>&1 | tee -a "$LOG"
  echo | tee -a "$LOG"
}

# Case A: fixed-threshold baseline should select PULL_KV (small payload).
run_case "fixed-threshold-pull" \
  --kvBytes=1024 --thresholdBytes=4194304 --linkRateMbps=100 \
  --linkDelayMs=10 --recomputeMs=80 --holderLoad=0.2 \
  --inFlightPulls=0 --destinationLoad=0.1 --observedThroughputMbps=90 --telemetryAgeMs=0

# Case B: fixed-threshold baseline should select RECOMPUTE (large payload).
run_case "fixed-threshold-recompute" \
  --kvBytes=16777216 --thresholdBytes=4194304 --linkRateMbps=100 \
  --linkDelayMs=10 --recomputeMs=80 --holderLoad=0.2 \
  --inFlightPulls=0 --destinationLoad=0.1 --observedThroughputMbps=90 --telemetryAgeMs=0

# Case C: cost-aware policy should be able to select EXECUTE_AT_HOLDER.
run_case "cost-aware-holder" \
  --kvBytes=16777216 --thresholdBytes=4194304 --linkRateMbps=100 \
  --linkDelayMs=10 --recomputeMs=80 --holderLoad=0.1 \
  --inFlightPulls=0 --destinationLoad=0.1 --observedThroughputMbps=90 --telemetryAgeMs=0

# Case D: low recomputation cost makes RECOMPUTE competitive.
run_case "cost-aware-recompute" \
  --kvBytes=16777216 --thresholdBytes=4194304 --linkRateMbps=10 \
  --linkDelayMs=10 --recomputeMs=1 --holderLoad=0.95 \
  --inFlightPulls=0 --destinationLoad=0.0 --observedThroughputMbps=8 --telemetryAgeMs=0

# Case E: holder is busy, KV is tiny, and destination is lightly loaded; pull may win.
run_case "network-aware-pull" \
  --kvBytes=1024 --thresholdBytes=4194304 --linkRateMbps=100 \
  --linkDelayMs=10 --recomputeMs=80 --holderLoad=1.0 \
  --inFlightPulls=0 --destinationLoad=0.0 --observedThroughputMbps=90 --telemetryAgeMs=0

echo "Results CSV: $CSV"
echo "Test log:    $LOG"
