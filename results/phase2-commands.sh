#!/usr/bin/env bash
set -euo pipefail

cd "$HOME/cnmp/ns-3.48"

mkdir -p results
: > results/phase2-decisions.log

run_case() {
  local label="$1"
  shift
  echo "===== ${label} =====" | tee -a results/phase2-decisions.log
  ./ns3 run "scratch/nakr-routing $*" 2>&1 | tee -a results/phase2-decisions.log
  echo | tee -a results/phase2-decisions.log
}

# Case 1: small KV payload on a lightly loaded link.
run_case "small-kv-low-contention" \
  --kvBytes=1048576 --thresholdBytes=4194304 --linkRateMbps=100 \
  --linkDelayMs=10 --recomputeMs=80 --holderLoad=0.1 \
  --inFlightPulls=0 --destinationLoad=0.1 \
  --observedThroughputMbps=90 --telemetryAgeMs=0 --policy=all

# Case 2: large KV payload with several concurrent pulls and lower observed throughput.
run_case "large-kv-high-contention" \
  --kvBytes=16777216 --thresholdBytes=4194304 --linkRateMbps=100 \
  --linkDelayMs=10 --recomputeMs=80 --holderLoad=0.8 \
  --inFlightPulls=6 --destinationLoad=0.2 \
  --observedThroughputMbps=15 --telemetryAgeMs=100 --policy=all

# Case 3: small KV but a busy cache holder; compare holder execution with alternatives.
run_case "busy-holder" \
  --kvBytes=2097152 --thresholdBytes=4194304 --linkRateMbps=100 \
  --linkDelayMs=10 --recomputeMs=80 --holderLoad=0.95 \
  --inFlightPulls=1 --destinationLoad=0.1 \
  --observedThroughputMbps=70 --telemetryAgeMs=20 --policy=all

# Case 4: low bandwidth link.
run_case "low-bandwidth" \
  --kvBytes=4194304 --thresholdBytes=4194304 --linkRateMbps=10 \
  --linkDelayMs=10 --recomputeMs=80 --holderLoad=0.2 \
  --inFlightPulls=2 --destinationLoad=0.2 \
  --observedThroughputMbps=8 --telemetryAgeMs=50 --policy=all

echo "Saved output to results/phase2-decisions.log"
