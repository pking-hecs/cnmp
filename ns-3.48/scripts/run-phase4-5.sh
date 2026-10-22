#!/usr/bin/env bash
# Reproduce everything for Phases 1-5 from any directory.
#   bash scripts/run-phase4-5.sh            # full run (Phase 5 campaign takes ~30-40 min on one core)
#   bash scripts/run-phase4-5.sh quick      # skip the long Phase 5 campaign, re-analyze existing results
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NS3="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO="$(cd "$NS3/.." && pwd)"
TOOLS="$NS3/tools"
RESULTS="$REPO/results"
MODE="${1:-full}"

echo "== Build ns-3 (new Phase 4/5 programs in scratch/) =="
( cd "$NS3" && ./ns3 build )

echo "== Phases 1-3: clearer CSVs and plots =="
python3 "$TOOLS/analyze_phases.py" --repo "$REPO" --out "$RESULTS/phase1-3"

echo "== Phase 4: calibrate transfer model + decision sweeps =="
python3 "$TOOLS/phase4_calibrate.py"  --ns3dir "$NS3" --out "$RESULTS/phase4"
python3 "$TOOLS/phase4_model_sweep.py" --ns3dir "$NS3" --out "$RESULTS/phase4"

if [ "$MODE" != "quick" ]; then
  echo "== Phase 5: multi-node campaign (resumable) =="
  python3 "$TOOLS/phase5_run_experiments.py" --ns3dir "$NS3" --out "$RESULTS/phase5"
fi
echo "== Phase 5: analysis =="
python3 "$TOOLS/phase5_analyze.py" --dir "$RESULTS/phase5"
echo "Done. See $RESULTS/phase1-3, $RESULTS/phase4, $RESULTS/phase5."
