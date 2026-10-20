#include "ns3/core-module.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

using namespace ns3;

namespace
{
enum class Action { ExecuteAtHolder, PullKv, Recompute };

struct Costs
{
    double holderMs;
    double pullMs;
    double recomputeMs;
};

std::string ActionName(Action action)
{
    switch (action)
    {
    case Action::ExecuteAtHolder: return "EXECUTE_AT_HOLDER";
    case Action::PullKv: return "PULL_KV";
    case Action::Recompute: return "RECOMPUTE";
    }
    return "UNKNOWN";
}

double ClampLoad(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

Costs EstimateCosts(uint64_t kvBytes,
                    double linkRateMbps,
                    double linkDelayMs,
                    double recomputeMs,
                    double holderLoad,
                    uint32_t inFlightPulls,
                    double destinationLoad,
                    double observedThroughputMbps,
                    double telemetryAgeMs,
                    bool networkAware)
{
    const double safeRate = std::max(0.001, linkRateMbps);
    const double safeObservedRate = std::max(0.001, observedThroughputMbps);
    const double holder = ClampLoad(holderLoad);
    const double destination = ClampLoad(destinationLoad);
    const double rttMs = 2.0 * std::max(0.0, linkDelayMs);
    const double remainingComputeMs = 5.0;

    // Explicitly synthetic workload model. Calibrate/replace these coefficients
    // before making claims about real LLM or GPU latency.
    const double holderMs = rttMs + remainingComputeMs + holder * 100.0;

    double effectiveRate = safeRate;
    double queuePenaltyMs = 0.0;
    double stalePenaltyMs = 0.0;
    if (networkAware)
    {
        effectiveRate = std::min(safeRate, safeObservedRate);
        queuePenaltyMs = static_cast<double>(inFlightPulls) * 3.0;
        stalePenaltyMs = std::max(0.0, telemetryAgeMs) * 0.05;
    }

    // Mbps = megabits/second; bytes*8 / Mbps / 1000 gives milliseconds.
    const double transferMs = static_cast<double>(kvBytes) * 8.0 / (effectiveRate * 1000.0);
    const double pullMs = rttMs + transferMs + remainingComputeMs +
                          destination * 50.0 + queuePenaltyMs + stalePenaltyMs;
    const double recomputeTotalMs = std::max(0.0, recomputeMs) +
                                   remainingComputeMs + destination * 50.0;
    return {holderMs, pullMs, recomputeTotalMs};
}

// Baseline policy: decide only between pull and recompute using KV-size threshold.
// It deliberately does not use load or network telemetry.
Action ChooseFixedThreshold(uint64_t kvBytes, uint64_t thresholdBytes)
{
    return kvBytes <= thresholdBytes ? Action::PullKv : Action::Recompute;
}

Action ChooseMinimumCost(const Costs& costs)
{
    // Deterministic tie-break: holder, then pull, then recompute.
    if (costs.holderMs <= costs.pullMs && costs.holderMs <= costs.recomputeMs)
        return Action::ExecuteAtHolder;
    if (costs.pullMs <= costs.recomputeMs)
        return Action::PullKv;
    return Action::Recompute;
}

double ActionCost(Action action, const Costs& costs)
{
    switch (action)
    {
    case Action::ExecuteAtHolder: return costs.holderMs;
    case Action::PullKv: return costs.pullMs;
    case Action::Recompute: return costs.recomputeMs;
    }
    return 0.0;
}
} // namespace

int main(int argc, char* argv[])
{
    uint64_t kvBytes = 4 * 1024 * 1024;
    uint64_t thresholdBytes = 4 * 1024 * 1024;
    double linkRateMbps = 100.0;
    double linkDelayMs = 10.0;
    double recomputeMs = 80.0;
    double holderLoad = 0.2;
    uint32_t inFlightPulls = 0;
    double destinationLoad = 0.2;
    double observedThroughputMbps = 80.0;
    double telemetryAgeMs = 0.0;
    std::string policy = "all";
    std::string csvPath = "";

    CommandLine cmd(__FILE__);
    cmd.AddValue("kvBytes", "KV payload size in bytes", kvBytes);
    cmd.AddValue("thresholdBytes", "Fixed-threshold cutoff in bytes", thresholdBytes);
    cmd.AddValue("linkRateMbps", "Configured bottleneck bandwidth in Mbps", linkRateMbps);
    cmd.AddValue("linkDelayMs", "One-way link delay in milliseconds", linkDelayMs);
    cmd.AddValue("recomputeMs", "Synthetic prefix recomputation time in milliseconds", recomputeMs);
    cmd.AddValue("holderLoad", "Normalized holder load from 0 to 1", holderLoad);
    cmd.AddValue("inFlightPulls", "Number of concurrent pulls targeting the link/holder", inFlightPulls);
    cmd.AddValue("destinationLoad", "Normalized destination load from 0 to 1", destinationLoad);
    cmd.AddValue("observedThroughputMbps", "Observed throughput used by network-aware policy", observedThroughputMbps);
    cmd.AddValue("telemetryAgeMs", "Age of telemetry in milliseconds", telemetryAgeMs);
    cmd.AddValue("policy", "fixed, cost, network, or all", policy);
    cmd.AddValue("csvPath", "Optional CSV output path", csvPath);
    cmd.Parse(argc, argv);

    if (linkRateMbps <= 0.0 || observedThroughputMbps <= 0.0 ||
        linkDelayMs < 0.0 || recomputeMs < 0.0 ||
        holderLoad < 0.0 || holderLoad > 1.0 ||
        destinationLoad < 0.0 || destinationLoad > 1.0)
    {
        std::cerr << "ERROR: rates must be positive; delays/costs non-negative; loads in [0,1].\n";
        return 2;
    }
    if (policy != "fixed" && policy != "cost" && policy != "network" && policy != "all")
    {
        std::cerr << "ERROR: policy must be fixed, cost, network, or all.\n";
        return 2;
    }

    const Costs nominal = EstimateCosts(kvBytes, linkRateMbps, linkDelayMs, recomputeMs,
                                        holderLoad, inFlightPulls, destinationLoad,
                                        observedThroughputMbps, telemetryAgeMs, false);
    const Costs aware = EstimateCosts(kvBytes, linkRateMbps, linkDelayMs, recomputeMs,
                                      holderLoad, inFlightPulls, destinationLoad,
                                      observedThroughputMbps, telemetryAgeMs, true);

    const Action fixedAction = ChooseFixedThreshold(kvBytes, thresholdBytes);
    const Action costAction = ChooseMinimumCost(nominal);
    const Action networkAction = ChooseMinimumCost(aware);

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "NAKR Phase 3: policy validation (synthetic cost model)\n"
              << "kvBytes=" << kvBytes << " thresholdBytes=" << thresholdBytes
              << " linkRateMbps=" << linkRateMbps << " linkDelayMs=" << linkDelayMs
              << " recomputeMs=" << recomputeMs << " holderLoad=" << holderLoad
              << " inFlightPulls=" << inFlightPulls << " destinationLoad=" << destinationLoad
              << " observedThroughputMbps=" << observedThroughputMbps
              << " telemetryAgeMs=" << telemetryAgeMs << "\n\n";

    std::cout << "Nominal costs (ms): holder=" << nominal.holderMs
              << " pull=" << nominal.pullMs << " recompute=" << nominal.recomputeMs << "\n";
    std::cout << "Network-aware costs (ms): holder=" << aware.holderMs
              << " pull=" << aware.pullMs << " recompute=" << aware.recomputeMs << "\n";

    struct Row { const char* name; Action action; double cost; };
    Row rows[] = {
        {"fixed_threshold", fixedAction, ActionCost(fixedAction, nominal)},
        {"cost_aware", costAction, ActionCost(costAction, nominal)},
        {"network_aware", networkAction, ActionCost(networkAction, aware)}
    };

    for (const auto& row : rows)
    {
        if (policy == "all" ||
            (policy == "fixed" && std::string(row.name) == "fixed_threshold") ||
            (policy == "cost" && std::string(row.name) == "cost_aware") ||
            (policy == "network" && std::string(row.name) == "network_aware"))
        {
            std::cout << row.name << "," << ActionName(row.action)
                      << "," << row.cost << " ms\n";
        }
    }

    if (!csvPath.empty())
    {
        std::ofstream out(csvPath, std::ios::app);
        if (!out)
        {
            std::cerr << "ERROR: could not open csvPath: " << csvPath << "\n";
            return 3;
        }
        for (const auto& row : rows)
        {
            if (policy == "all" ||
                (policy == "fixed" && std::string(row.name) == "fixed_threshold") ||
                (policy == "cost" && std::string(row.name) == "cost_aware") ||
                (policy == "network" && std::string(row.name) == "network_aware"))
            {
                out << row.name << "," << kvBytes << "," << thresholdBytes << ","
                    << linkRateMbps << "," << linkDelayMs << "," << recomputeMs << ","
                    << holderLoad << "," << inFlightPulls << "," << destinationLoad << ","
                    << observedThroughputMbps << "," << telemetryAgeMs << ","
                    << ActionName(row.action) << "," << row.cost << "\n";
            }
        }
    }

    Simulator::Destroy();
    return 0;
}
