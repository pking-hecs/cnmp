#!/usr/bin/env bash
# Reproduce everything for Phases 1-5 from the repo root (works from any directory).
#   bash scripts/run-phase4-5.sh            # full run (Phase 5 campaign takes ~30-40 min on one core)
#   bash scripts/run-phase4-5.sh quick      # skip the long Phase 5 campaign, re-analyze existing results
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NS3="$REPO/ns-3.48"
MODE="${1:-full}"

echo "== Build ns-3 (new Phase 4/5 programs in scratch/) =="
( cd "$NS3" && ./ns3 build )

echo "== Phases 1-3: clearer CSVs and plots =="
python3 "$REPO/tools/analyze_phases.py" --repo "$REPO" --out "$REPO/results/phase1-3"

echo "== Phase 4: calibrate transfer model + decision sweeps =="
python3 "$REPO/tools/phase4_calibrate.py"  --ns3dir "$NS3" --out "$REPO/results/phase4"
python3 "$REPO/tools/phase4_model_sweep.py" --ns3dir "$NS3" --out "$REPO/results/phase4"

if [ "$MODE" != "quick" ]; then
  echo "== Phase 5: multi-node campaign (resumable) =="
  python3 "$REPO/tools/phase5_run_experiments.py" --ns3dir "$NS3" --out "$REPO/results/phase5"
fi
echo "== Phase 5: analysis =="
python3 "$REPO/tools/phase5_analyze.py" --dir "$REPO/results/phase5"
echo "Done. See results/phase1-3, results/phase4, results/phase5."
