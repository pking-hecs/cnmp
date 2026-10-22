// NAKR Phase 4: routing decision calculator on the calibrated cost model.
//
// Replaces the Phase 3 placeholder coefficients (holder*100, destination*50, inFlight*3, ...)
// with quantities that have physical meaning:
//   - prefix / suffix lengths in TOKENS  -> KV bytes (architecture-derived) and prefill time
//   - queue backlog in ms                -> compute availability
//   - link rate, RTT, background utilization, in-flight pulls -> network state
//   - TCP startup and efficiency fitted from the ns-3 calibration sweep (Phase 4)
//
// Scenario: a request needs a prefix whose KV lives on a HOLDER. Options:
//   HOLDER     run on the holder (wait behind its queue, compute suffix only)
//   PULL       run on an idle TARGET after pulling the KV over the network
//   RECOMPUTE  run on the idle TARGET and recompute the whole prompt
// "static" uses nominal link rate only; "telemetry" uses observed utilization and in-flight pulls.
//
// Output: one CSV row (use --header=1 to print the header first).

#include "nakr-cost-model.h"

#include "ns3/core-module.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>

using namespace ns3;

namespace
{
const char*
Choose(double holder, double pull, double recompute)
{
    if (holder <= pull && holder <= recompute)
    {
        return "HOLDER";
    }
    return pull <= recompute ? "PULL" : "RECOMPUTE";
}
} // namespace

int
main(int argc, char* argv[])
{
    nakr::ModelParams m;
    std::string scenario = "adhoc";
    double prefixTokens = 2048, suffixTokens = 128;
    double linkMbps = 10000, rttMs = 0.4;
    double bgUtil = 0.0;       // fraction of the path already used by other traffic (0..1)
    uint32_t inFlight = 0;     // KV pulls already sharing the path
    double holderQueueMs = 0;  // backlog at the holder
    double targetQueueMs = 0;  // backlog at the pull/recompute target
    uint32_t header = 0;
    double tps = m.prefillTokensPerSec;

    CommandLine cmd(__FILE__);
    cmd.AddValue("scenario", "Label", scenario);
    cmd.AddValue("prefixTokens", "Reusable prefix length (tokens)", prefixTokens);
    cmd.AddValue("suffixTokens", "New tokens after the prefix", suffixTokens);
    cmd.AddValue("linkMbps", "Nominal path bandwidth (Mbps)", linkMbps);
    cmd.AddValue("rttMs", "Path RTT (ms)", rttMs);
    cmd.AddValue("bgUtil", "Background utilization of the path (0..1)", bgUtil);
    cmd.AddValue("inFlight", "In-flight pulls on the path", inFlight);
    cmd.AddValue("holderQueueMs", "Holder queue backlog (ms)", holderQueueMs);
    cmd.AddValue("targetQueueMs", "Target queue backlog (ms)", targetQueueMs);
    cmd.AddValue("kvBytesPerToken", "KV bytes per token", m.kvBytesPerToken);
    cmd.AddValue("prefillTps", "Prefill speed (tokens/s)", tps);
    cmd.AddValue("tcpStartupRtts", "Fitted TCP startup (RTTs)", m.tcpStartupRtts);
    cmd.AddValue("tcpInvEfficiency", "Fitted 1/efficiency", m.tcpInvEfficiency);
    cmd.AddValue("header", "Print CSV header first", header);
    cmd.Parse(argc, argv);
    m.prefillTokensPerSec = tps;

    const double bytes = nakr::KvBytes(m, prefixTokens);
    const double suffixMs = nakr::PrefillMs(m, suffixTokens);
    const double fullMs = nakr::PrefillMs(m, prefixTokens + suffixTokens);

    const double holder = holderQueueMs + suffixMs;
    const double recompute = targetQueueMs + fullMs;

    const double staticTransfer = nakr::TransferMs(m, bytes, linkMbps, rttMs);
    // Free capacity left by other traffic; if our own pulls already fill the link a new flow
    // still gets a fair share link/(k+1).
    double availTel = linkMbps * (1.0 - bgUtil);
    if (inFlight > 0)
    {
        availTel = std::max(availTel, linkMbps / (inFlight + 1.0));
    }
    const double telTransfer = nakr::TransferMs(m, bytes, std::max(0.02 * linkMbps, availTel), rttMs);

    // The request starts when both the KV has arrived and the target queue has drained.
    const double pullStatic = std::max(targetQueueMs, staticTransfer) + suffixMs;
    const double pullTel = std::max(targetQueueMs, telTransfer) + suffixMs;

    const char* cs = Choose(holder, pullStatic, recompute);
    const char* ct = Choose(holder, pullTel, recompute);

    if (header)
    {
        std::cout << "scenario,prefix_tokens,kv_mib,link_mbps,bg_util,in_flight,holder_queue_ms,"
                     "target_queue_ms,holder_ms,recompute_ms,pull_static_ms,pull_telemetry_ms,"
                     "static_choice,telemetry_choice,telemetry_changed_decision\n";
    }
    std::cout << std::fixed << std::setprecision(3) << scenario << "," << prefixTokens << ","
              << bytes / (1024.0 * 1024.0) << "," << linkMbps << "," << bgUtil << "," << inFlight << ","
              << holderQueueMs << "," << targetQueueMs << "," << holder << "," << recompute << ","
              << pullStatic << "," << pullTel << "," << cs << "," << ct << ","
              << (std::string(cs) != ct ? "yes" : "no") << "\n";
    return 0;
}
