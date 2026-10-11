
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt

results_dir = Path(__file__).parent
csv_path = results_dir / "nakr-concurrency.csv"

data = defaultdict(list)

with csv_path.open(newline="") as f:
    for row in csv.DictReader(f):
        if row["all_complete"].strip().lower() != "true":
            continue

        rate = int(row["link_rate_mbps"])
        senders = int(row["senders"])

        data[rate].append({
            "senders": senders,
            "throughput": float(row["aggregate_throughput_mbps"]),
            # CSV stores absolute simulation timestamp.
            # Sender applications start at simulation time 1 second.
            "completion": float(row["last_byte_time_s"]) - 1.0,
        })

for rate in data:
    data[rate].sort(key=lambda x: x["senders"])

# Plot 1: aggregate throughput
plt.figure()
for rate, rows in sorted(data.items()):
    plt.plot(
        [r["senders"] for r in rows],
        [r["throughput"] for r in rows],
        marker="o",
        label=f"{rate} Mbps bottleneck",
    )

plt.xlabel("Number of concurrent senders")
plt.ylabel("Aggregate throughput (Mbps)")
plt.title("NAKR: Aggregate Throughput vs Concurrency")
plt.xticks([1, 2, 4, 8])
plt.grid(True, alpha=0.3)
plt.legend()
plt.tight_layout()
plt.savefig(results_dir / "nakr-throughput.png", dpi=200)
plt.close()

# Plot 2: duration from sender start until final byte arrives
plt.figure()
for rate, rows in sorted(data.items()):
    plt.plot(
        [r["senders"] for r in rows],
        [r["completion"] for r in rows],
        marker="o",
        label=f"{rate} Mbps bottleneck",
    )

plt.xlabel("Number of concurrent senders")
plt.ylabel("Completion duration (seconds)")
plt.title("NAKR: Completion Time vs Concurrency")
plt.xticks([1, 2, 4, 8])
plt.grid(True, alpha=0.3)
plt.legend()
plt.tight_layout()
plt.savefig(results_dir / "nakr-completion-time.png", dpi=200)
plt.close()

print(f"Saved: {results_dir / 'nakr-throughput.png'}")
print(f"Saved: {results_dir / 'nakr-completion-time.png'}")
