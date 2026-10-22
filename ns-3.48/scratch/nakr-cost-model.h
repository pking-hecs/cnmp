// NAKR Phase 4: calibrated cost model shared by nakr-routing-v2.cc (decision calculator)
// and nakr-cluster.cc (Phase 5 multi-node simulation).
//
// Every constant here is either
//   (a) derived from a model architecture (documented below), or
//   (b) fitted from the NS-3 TCP calibration sweep (results/phase4/calibration-fit.json), or
//   (c) an explicit, overridable assumption about GPU speed.
// Nothing is a hidden magic number: all of them are command-line overridable in the programs.
#ifndef NAKR_COST_MODEL_H
#define NAKR_COST_MODEL_H

#include <algorithm>
#include <cstdint>

namespace nakr
{

struct ModelParams
{
    // ---- (a) KV size, derived from architecture -------------------------------------------
    // KV bytes per token = 2 (K and V) * layers * kv_heads * head_dim * bytes_per_value.
    // Llama-3-8B-like (GQA): 2 * 32 * 8 * 128 * 2 (fp16) = 131,072 bytes = 128 KiB per token.
    // (A model without GQA, e.g. Llama-2-7B, would be 4x larger: 512 KiB per token.)
    double kvBytesPerToken = 131072.0;

    // ---- (c) GPU assumptions (override with --prefillTps etc.) ------------------------------
    double prefillTokensPerSec = 12000.0; // prefill speed of one instance
    double perRequestOverheadMs = 5.0;    // scheduling / kernel launch overhead per request
    double tpotMs = 15.0;                 // time per output token (decode), not contended here

    // ---- (b) TCP transfer model, fitted by tools/fit_transfer_model.py ----------------------
    // transfer_ms = tcpStartupRtts * RTT + invEfficiency * bytes*8 / available_rate
    double tcpStartupRtts = 6.87;   // fitted, held-out MAPE 5.7%
    double tcpInvEfficiency = 1.008;
};

inline double
PrefillMs(const ModelParams& m, double tokens)
{
    return m.perRequestOverheadMs + tokens / m.prefillTokensPerSec * 1000.0;
}

inline double
KvBytes(const ModelParams& m, double tokens)
{
    return tokens * m.kvBytesPerToken;
}

// availMbps is the bandwidth one new flow can expect on the path (Mbps); rttMs the path RTT.
inline double
TransferMs(const ModelParams& m, double bytes, double availMbps, double rttMs)
{
    const double rate = std::max(0.001, availMbps);
    // Mbps -> bits per ms is rate*1000.
    return m.tcpStartupRtts * rttMs + m.tcpInvEfficiency * bytes * 8.0 / (rate * 1000.0);
}

} // namespace nakr

#endif
