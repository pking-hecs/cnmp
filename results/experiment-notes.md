# NAKR — Initial NS-3 Concurrency Experiment

## 1. Objective

Evaluate how concurrent TCP transfers behave when multiple senders share a constrained network bottleneck. This is the first networking experiment for NAKR (Network-Aware KV-Cache Routing).

## 2. Experimental Setup

- **Simulator:** NS-3.48
- **Transport protocol:** TCP
- **Topology:** Multiple sender nodes connect through a router to one receiver.
- **Access links:** 1 Gbps, 1 ms delay.
- **Shared bottleneck:** 10 Mbps or 100 Mbps.
- **Bottleneck one-way delay:** 10 ms.
- **Transfer size:** 10 MiB per sender (10,485,760 bytes).
- **Concurrency levels:** 1, 2, 4, and 8 senders.
- **Measurements:** Received bytes, per-sender throughput, aggregate throughput, and time until the final byte arrives.

## 3. Results

Results are stored in `nakr-concurrency.csv`.

At 10 Mbps, aggregate throughput remained around 9.02–9.07 Mbps as concurrency increased from 1 to 8 senders. All transfers completed successfully.

At 100 Mbps, aggregate throughput increased from 35.91 Mbps with one sender to 89.63 Mbps with eight senders. All transfers completed successfully.

## 4. Initial Interpretation

The 10 Mbps results are consistent with aggregate throughput being limited by the shared bottleneck. As more senders share the link, each sender receives a smaller share of the available capacity and the total batch takes longer to complete.

At 100 Mbps, multiple concurrent TCP flows achieved higher aggregate throughput than a single flow. This indicates that concurrency and TCP behavior affect utilization in this simulated setup.

These observations motivate further investigation into how transfer size, network conditions, and concurrent KV-cache pulls influence retrieval latency.

## 5. Figures

- `nakr-throughput.png` — Aggregate throughput versus number of senders.
- `nakr-completion-time.png` — Completion duration versus number of senders.

## 6. Limitations

- This is a synthetic network experiment; the payloads represent generic TCP data, not actual LLM KV-cache tensors.
- The experiment does not yet compare pulling KV cache with recomputing a prefix.
- It does not implement NAKR's routing decision or compare routing policies.
- The 100 Mbps single-flow throughput is below the configured link rate. TCP startup, congestion-control behavior, and other simulation settings need investigation before assigning a specific cause.

## 7. Next Steps

1. Validate and repeat selected runs for reproducibility.
2. Investigate TCP behavior in the 100 Mbps single-flow case.
3. Test different transfer sizes, bandwidths, delays, and concurrent background traffic.
4. Develop and validate a transfer-cost model.
5. Implement the NAKR decision among executing at a cache holder, pulling KV cache, and recomputing the prefix.
