// NAKR Phase 5: multi-node LLM-serving cluster on ns-3.
//
//   instances I0..I(N-1) ---- access links (heterogeneous rate/delay) ---- switch S
//   background senders B_i ------------------------------------------------ S   (optional congestion)
//
// A request has a shared prefix (a prompt group such as a system prompt or document) plus a short
// unique suffix. KV for each prefix lives in per-instance LRU caches. For every request the router
// chooses (target instance, how to obtain the prefix KV):
//     HIT        target already holds the prefix KV           -> compute suffix only
//     PULL       target fetches the prefix KV over TCP        -> real ns-3 BulkSend/PacketSink flow
//     RECOMPUTE  target recomputes the whole prompt           -> compute prefix + suffix
// Each instance is a FIFO prefill server (service time from the calibrated Phase 4 model).
// KV transfers are REAL TCP flows on the simulated network, so they contend with each other and
// with background traffic. Everything the router "knows" about the network comes from telemetry it
// measures itself (per-link utilization windows + EWMA, plus its own in-flight pull count).
//
// Policies (--policy=):
//   load_based    least-queued instance; HIT if it happens to hold the prefix, else RECOMPUTE
//   cache_aware   instance holding the prefix (least-queued holder); never pulls
//   network_only  instance with most available downlink bandwidth; HIT/RECOMPUTE only
//   cost_nopull   min estimated TTFT over HIT / RECOMPUTE only (queue-aware, never uses the network)
//   cost_static   min estimated TTFT over all (target, action) using NOMINAL link rates
//   nakr          min estimated TTFT over all (target, action) using LIVE telemetry (EWMA)
//
// All policies see the identical, seeded workload (independent of ns-3's RNG).

#include "nakr-cost-model.h"

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

namespace
{
enum class Kind
{
    HIT,
    PULL,
    RECOMPUTE
};

const char*
KindName(Kind k)
{
    return k == Kind::HIT ? "HIT" : (k == Kind::PULL ? "PULL" : "RECOMPUTE");
}

struct Req
{
    uint32_t id = 0;
    double arrivalMs = 0;
    uint32_t group = 0;
    uint32_t suffix = 0;
    uint32_t out = 0;
    int target = -1;
    int source = -1;
    Kind kind = Kind::RECOMPUTE;
    double serviceMs = 0;
    double estTtftMs = 0;
    double predPullMs = 0;
    double pullStartMs = 0;
    double pullActualMs = 0;
    double ttftMs = 0;
    double e2eMs = 0;
};

struct Entry
{
    uint32_t group;
    uint32_t tokens;
    double lastUse;
};

struct Inst
{
    double rateMbps = 1000;
    double delayMs = 0.05;
    bool busy = false;
    double busyUntilMs = 0;
    double busyTotalMs = 0;
    double committedMs = 0; // service time of committed-but-not-started requests
    std::deque<uint32_t> q;
    std::vector<Entry> cache;
    uint64_t bytesUp = 0, bytesDown = 0, lastUp = 0, lastDown = 0;
    double uUp = 0, uDown = 0; // EWMA utilization (0..1)
    int inUp = 0, inDown = 0; // in-flight pulls leaving / entering this instance
};

struct PullCtx
{
    uint32_t reqId;
    uint64_t bytes;
    uint64_t got = 0;
    bool done = false;
};

struct Opt
{
    int target = -1;
    int source = -1;
    Kind kind = Kind::RECOMPUTE;
    double est = 1e30;
    double transferMs = 0;
    double serviceMs = 0;
};

// ----- global state --------------------------------------------------------------------------
std::string g_policy = "nakr";
nakr::ModelParams g_mp;
uint32_t g_n = 4;
uint64_t g_cacheTokens = 16384;
double g_alpha = 0.5;
double g_telemetryMs = 20;
std::vector<Inst> g_inst;
std::vector<Req> g_reqs;
std::vector<uint32_t> g_groupTokens;
NodeContainer g_nodes;
std::vector<Ipv4Address> g_addr;
std::vector<std::unique_ptr<PullCtx>> g_pulls;
uint16_t g_nextPort = 6000;
uint32_t g_completed = 0;
bool g_finished = false;
double g_firstArrivalMs = 1e30, g_lastFinishMs = 0;
uint32_t g_sndSize = 65536;

double
NowMs()
{
    return Simulator::Now().GetSeconds() * 1000.0;
}

std::vector<double>
ParseList(const std::string& s, size_t n, double fallback)
{
    std::vector<double> v;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ','))
    {
        if (!tok.empty())
        {
            v.push_back(std::stod(tok));
        }
    }
    if (v.empty())
    {
        v.push_back(fallback);
    }
    while (v.size() < n)
    {
        v.push_back(v.back());
    }
    return v;
}

// ----- cache -----------------------------------------------------------------------------------
Entry*
FindEntry(int j, uint32_t g)
{
    for (auto& e : g_inst[j].cache)
    {
        if (e.group == g)
        {
            return &e;
        }
    }
    return nullptr;
}

void
InsertEntry(int j, uint32_t g)
{
    if (Entry* e = FindEntry(j, g))
    {
        e->lastUse = NowMs();
        return;
    }
    uint32_t t = g_groupTokens[g];
    auto& c = g_inst[j].cache;
    auto used = [&]() {
        uint64_t s = 0;
        for (auto& e : c)
        {
            s += e.tokens;
        }
        return s;
    };
    while (!c.empty() && used() + t > g_cacheTokens)
    {
        auto it = std::min_element(c.begin(), c.end(), [](const Entry& a, const Entry& b) {
            return a.lastUse < b.lastUse;
        });
        c.erase(it);
    }
    c.push_back({g, t, NowMs()});
}

// ----- telemetry ------------------------------------------------------------------------------
void
OnTx(uint64_t* counter, Ptr<const Packet> p)
{
    *counter += p->GetSize();
}

void
TelemetryTick()
{
    if (g_finished)
    {
        return;
    }
    const double w = g_telemetryMs / 1000.0;
    for (auto& I : g_inst)
    {
        double rawUp = (I.bytesUp - I.lastUp) * 8.0 / (I.rateMbps * 1e6 * w);
        double rawDn = (I.bytesDown - I.lastDown) * 8.0 / (I.rateMbps * 1e6 * w);
        I.lastUp = I.bytesUp;
        I.lastDown = I.bytesDown;
        rawUp = std::min(1.0, std::max(0.0, rawUp));
        rawDn = std::min(1.0, std::max(0.0, rawDn));
        I.uUp = g_alpha * rawUp + (1 - g_alpha) * I.uUp;
        I.uDown = g_alpha * rawDn + (1 - g_alpha) * I.uDown;
    }
    Simulator::Schedule(MilliSeconds(g_telemetryMs), &TelemetryTick);
}

// Bandwidth (Mbps) one new flow can expect on path h -> t.
double
AvailMbps(int h, int t, bool telemetry)
{
    const double up = g_inst[h].rateMbps, dn = g_inst[t].rateMbps;
    if (!telemetry)
    {
        return std::min(up, dn);
    }
    // Free capacity left by whatever else is on the link; if our own pulls already fill it,
    // a new flow still gets a fair share rate/(k+1).
    double a = up * (1 - g_inst[h].uUp);
    if (g_inst[h].inUp > 0)
    {
        a = std::max(a, up / (g_inst[h].inUp + 1));
    }
    double b = dn * (1 - g_inst[t].uDown);
    if (g_inst[t].inDown > 0)
    {
        b = std::max(b, dn / (g_inst[t].inDown + 1));
    }
    return std::max(0.02 * std::min(up, dn), std::min(a, b));
}

double
AvailDown(int t)
{
    const double dn = g_inst[t].rateMbps;
    double b = dn * (1 - g_inst[t].uDown);
    if (g_inst[t].inDown > 0)
    {
        b = std::max(b, dn / (g_inst[t].inDown + 1));
    }
    return b;
}

double
RttMs(int h, int t)
{
    return 2.0 * (g_inst[h].delayMs + g_inst[t].delayMs);
}

double
QueueMs(int j)
{
    return std::max(0.0, g_inst[j].busyUntilMs - NowMs()) + g_inst[j].committedMs;
}

// ----- routing --------------------------------------------------------------------------------
Opt
OptHit(const Req& r, int j)
{
    Opt o;
    o.target = j;
    o.kind = Kind::HIT;
    o.serviceMs = nakr::PrefillMs(g_mp, r.suffix);
    o.est = QueueMs(j) + o.serviceMs + g_mp.tpotMs;
    return o;
}

Opt
OptRecompute(const Req& r, int j)
{
    Opt o;
    o.target = j;
    o.kind = Kind::RECOMPUTE;
    o.serviceMs = nakr::PrefillMs(g_mp, g_groupTokens[r.group] + r.suffix);
    o.est = QueueMs(j) + o.serviceMs + g_mp.tpotMs;
    return o;
}

Opt
OptPull(const Req& r, int j, const std::vector<int>& holders, bool telemetry)
{
    Opt best;
    const double bytes = nakr::KvBytes(g_mp, g_groupTokens[r.group]);
    for (int i : holders)
    {
        if (i == j)
        {
            continue;
        }
        double tr = nakr::TransferMs(g_mp, bytes, AvailMbps(i, j, telemetry), RttMs(i, j));
        double svc = nakr::PrefillMs(g_mp, r.suffix);
        // The request joins the GPU queue only when its KV has arrived; the queue keeps
        // draining meanwhile, so it starts at max(queue, transfer).
        double est = std::max(QueueMs(j), tr) + svc + g_mp.tpotMs;
        if (est < best.est)
        {
            best.target = j;
            best.source = i;
            best.kind = Kind::PULL;
            best.est = est;
            best.transferMs = tr;
            best.serviceMs = svc;
        }
    }
    return best;
}

Opt
Decide(const Req& r)
{
    std::vector<int> holders;
    for (uint32_t j = 0; j < g_n; ++j)
    {
        if (FindEntry(j, r.group))
        {
            holders.push_back(j);
        }
    }
    auto argminQueue = [&](const std::vector<int>& set) {
        int b = set[0];
        for (int j : set)
        {
            if (QueueMs(j) < QueueMs(b))
            {
                b = j;
            }
        }
        return b;
    };
    std::vector<int> all;
    for (uint32_t j = 0; j < g_n; ++j)
    {
        all.push_back(j);
    }

    if (g_policy == "load_based")
    {
        int j = argminQueue(all);
        return FindEntry(j, r.group) ? OptHit(r, j) : OptRecompute(r, j);
    }
    if (g_policy == "cache_aware")
    {
        if (!holders.empty())
        {
            return OptHit(r, argminQueue(holders));
        }
        return OptRecompute(r, argminQueue(all));
    }
    if (g_policy == "network_only")
    {
        int b = 0;
        for (uint32_t j = 1; j < g_n; ++j)
        {
            if (AvailDown(j) > AvailDown(b))
            {
                b = j;
            }
        }
        return FindEntry(b, r.group) ? OptHit(r, b) : OptRecompute(r, b);
    }
    if (g_policy == "cost_nopull")
    {
        // Queue- and compute-aware but network-blind: HIT where the KV is, RECOMPUTE elsewhere, never pull.
        Opt bestNp;
        for (uint32_t j = 0; j < g_n; ++j)
        {
            Opt c = FindEntry(j, r.group) ? OptHit(r, j) : OptRecompute(r, j);
            if (c.est < bestNp.est)
            {
                bestNp = c;
            }
        }
        return bestNp;
    }
    // cost_static / nakr: minimise estimated TTFT over every (target, action).
    const bool telemetry = (g_policy == "nakr");
    Opt best;
    for (uint32_t j = 0; j < g_n; ++j)
    {
        std::vector<Opt> cands;
        if (FindEntry(j, r.group))
        {
            cands.push_back(OptHit(r, j));
        }
        else
        {
            cands.push_back(OptRecompute(r, j));
            Opt p = OptPull(r, j, holders, telemetry);
            if (p.target >= 0)
            {
                cands.push_back(p);
            }
        }
        for (auto& c : cands)
        {
            if (c.est < best.est)
            {
                best = c;
            }
        }
    }
    return best;
}

// ----- GPU servers ----------------------------------------------------------------------------
void StartService(int j);

void
FinishService(int j, uint32_t id)
{
    Inst& I = g_inst[j];
    Req& r = g_reqs[id];
    I.busy = false;
    r.ttftMs = NowMs() - r.arrivalMs + g_mp.tpotMs;
    r.e2eMs = r.ttftMs + (r.out > 0 ? (r.out - 1) * g_mp.tpotMs : 0);
    InsertEntry(j, r.group); // KV for this prefix now exists locally
    g_lastFinishMs = std::max(g_lastFinishMs, NowMs());
    if (++g_completed == g_reqs.size())
    {
        g_finished = true;
        Simulator::Stop(MilliSeconds(1));
        return;
    }
    if (!I.q.empty())
    {
        StartService(j);
    }
}

void
StartService(int j)
{
    Inst& I = g_inst[j];
    uint32_t id = I.q.front();
    I.q.pop_front();
    Req& r = g_reqs[id];
    I.busy = true;
    I.committedMs = std::max(0.0, I.committedMs - r.serviceMs);
    I.busyUntilMs = NowMs() + r.serviceMs;
    I.busyTotalMs += r.serviceMs;
    if (Entry* e = FindEntry(j, r.group))
    {
        e->lastUse = NowMs();
    }
    Simulator::Schedule(MilliSeconds(r.serviceMs), &FinishService, j, id);
}

void
EnqueueGpu(uint32_t id)
{
    int j = g_reqs[id].target;
    g_inst[j].q.push_back(id);
    if (!g_inst[j].busy)
    {
        StartService(j);
    }
}

// ----- KV pulls (real TCP) --------------------------------------------------------------------
void
OnPullRx(PullCtx* ctx, Ptr<const Packet> p, const Address&)
{
    ctx->got += p->GetSize();
    if (!ctx->done && ctx->got >= ctx->bytes)
    {
        ctx->done = true;
        Req& r = g_reqs[ctx->reqId];
        r.pullActualMs = NowMs() - r.pullStartMs;
        g_inst[r.source].inUp--;
        g_inst[r.target].inDown--;
        InsertEntry(r.target, r.group); // KV replicated to target
        EnqueueGpu(ctx->reqId);
    }
}

void
StartPull(Req& r)
{
    const uint64_t bytes = static_cast<uint64_t>(nakr::KvBytes(g_mp, g_groupTokens[r.group]));
    g_inst[r.source].inUp++;
    g_inst[r.target].inDown++;
    r.pullStartMs = NowMs();
    g_pulls.push_back(std::make_unique<PullCtx>());
    PullCtx* ctx = g_pulls.back().get();
    ctx->reqId = r.id;
    ctx->bytes = bytes;
    const uint16_t port = g_nextPort++;
    PacketSinkHelper sink("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sa = sink.Install(g_nodes.Get(r.target));
    sa.Get(0)->TraceConnectWithoutContext("Rx", MakeBoundCallback(&OnPullRx, ctx));
    sa.Start(Seconds(0));
    BulkSendHelper bulk("ns3::TcpSocketFactory", InetSocketAddress(g_addr[r.target], port));
    bulk.SetAttribute("MaxBytes", UintegerValue(bytes));
    bulk.SetAttribute("SendSize", UintegerValue(g_sndSize));
    ApplicationContainer ba = bulk.Install(g_nodes.Get(r.source));
    ba.Start(Seconds(0));
}

void
OnArrival(uint32_t id)
{
    Req& r = g_reqs[id];
    Opt o = Decide(r);
    r.target = o.target;
    r.source = o.source;
    r.kind = o.kind;
    r.serviceMs = o.serviceMs;
    r.estTtftMs = o.est;
    r.predPullMs = o.transferMs;
    g_inst[o.target].committedMs += o.serviceMs;
    if (o.kind == Kind::PULL)
    {
        StartPull(r);
    }
    else
    {
        EnqueueGpu(id);
    }
}

double
Pct(std::vector<double> v, double p)
{
    if (v.empty())
    {
        return 0;
    }
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(std::ceil(p / 100.0 * v.size()));
    idx = std::min(v.size(), std::max<size_t>(1, idx)) - 1;
    return v[idx];
}

double
Mean(const std::vector<double>& v)
{
    double s = 0;
    for (double x : v)
    {
        s += x;
    }
    return v.empty() ? 0 : s / v.size();
}
} // namespace

int
main(int argc, char* argv[])
{
    std::string scenario = "default";
    std::string linkMbps = "10000,10000,2000,2000";
    std::string linkDelayMs = "0.05";
    std::string bgMbps = "0";
    std::string summaryCsv, perRequestCsv;
    std::string qdisc = "none";
    uint32_t requests = 300, groups = 16, seed = 1;
    double rate = 20.0, zipfS = 1.0, bgStartS = 0.0, bgOnS = 1.0, bgOffS = 0.0;
    uint32_t suffixMin = 256, suffixMax = 768, outTokens = 128;
    uint32_t bufBytes = 8 * 1024 * 1024, queuePackets = 512;
    double tps = g_mp.prefillTokensPerSec;

    CommandLine cmd(__FILE__);
    cmd.AddValue("policy", "load_based|cache_aware|network_only|cost_nopull|cost_static|nakr", g_policy);
    cmd.AddValue("scenario", "Label written to the CSV", scenario);
    cmd.AddValue("instances", "Number of inference instances", g_n);
    cmd.AddValue("linkMbps", "Per-instance access link rates, comma list (Mbps)", linkMbps);
    cmd.AddValue("linkDelayMs", "Per-instance one-way access delay, comma list (ms)", linkDelayMs);
    cmd.AddValue("bgMbps", "Per-instance background UDP rate into the instance, comma list", bgMbps);
    cmd.AddValue("bgStartS", "Background traffic start (s)", bgStartS);
    cmd.AddValue("bgOnS", "Background burst on-time (s)", bgOnS);
    cmd.AddValue("bgOffS", "Background burst off-time (s); 0 = constant load", bgOffS);
    cmd.AddValue("requests", "Number of requests", requests);
    cmd.AddValue("rate", "Poisson arrival rate (requests/s)", rate);
    cmd.AddValue("groups", "Number of shared-prefix groups", groups);
    cmd.AddValue("zipfS", "Zipf exponent of group popularity", zipfS);
    cmd.AddValue("seed", "Workload seed", seed);
    cmd.AddValue("cacheTokens", "KV cache capacity per instance (tokens)", g_cacheTokens);
    cmd.AddValue("outTokens", "Output tokens per request", outTokens);
    cmd.AddValue("suffixMin", "Min new (non-prefix) tokens per request", suffixMin);
    cmd.AddValue("suffixMax", "Max new (non-prefix) tokens per request", suffixMax);
    cmd.AddValue("prefillTps", "Prefill speed (tokens/s)", tps);
    cmd.AddValue("tpotMs", "Decode time per token (ms)", g_mp.tpotMs);
    cmd.AddValue("kvBytesPerToken", "KV bytes per token", g_mp.kvBytesPerToken);
    cmd.AddValue("tcpStartupRtts", "Fitted TCP startup (RTTs)", g_mp.tcpStartupRtts);
    cmd.AddValue("tcpInvEfficiency", "Fitted 1/efficiency", g_mp.tcpInvEfficiency);
    cmd.AddValue("telemetryMs", "Telemetry window (ms)", g_telemetryMs);
    cmd.AddValue("ewmaAlpha", "EWMA weight of newest window (1 = no smoothing)", g_alpha);
    cmd.AddValue("bufBytes", "TCP send/recv buffer (bytes)", bufBytes);
    cmd.AddValue("qdisc", "none|default", qdisc);
    cmd.AddValue("summaryCsv", "Append one summary row", summaryCsv);
    cmd.AddValue("perRequestCsv", "Write per-request log", perRequestCsv);
    cmd.Parse(argc, argv);
    g_mp.prefillTokensPerSec = tps;

    const std::vector<double> rates = ParseList(linkMbps, g_n, 1000);
    const std::vector<double> delays = ParseList(linkDelayMs, g_n, 0.05);
    const std::vector<double> bgs = ParseList(bgMbps, g_n, 0);

    // ---------------- workload (identical for every policy) ----------------
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    const uint32_t lens[3] = {1024, 2048, 4096};
    g_groupTokens.resize(groups);
    for (uint32_t g = 0; g < groups; ++g)
    {
        g_groupTokens[g] = lens[rng() % 3];
    }
    std::vector<double> cum(groups);
    double tot = 0;
    for (uint32_t g = 0; g < groups; ++g)
    {
        tot += 1.0 / std::pow(g + 1.0, zipfS);
        cum[g] = tot;
    }
    std::vector<uint32_t> perm(groups);
    for (uint32_t g = 0; g < groups; ++g)
    {
        perm[g] = g;
    }
    std::shuffle(perm.begin(), perm.end(), rng); // random placement of hot groups
    g_reqs.resize(requests);
    double t = 100.0; // ms
    for (uint32_t i = 0; i < requests; ++i)
    {
        t += -std::log(1.0 - U(rng)) / rate * 1000.0;
        Req& r = g_reqs[i];
        r.id = i;
        r.arrivalMs = t;
        double u = U(rng) * tot;
        r.group = std::lower_bound(cum.begin(), cum.end(), u) - cum.begin();
        r.group = std::min(r.group, groups - 1);
        r.suffix = suffixMin + static_cast<uint32_t>(U(rng) * (suffixMax - suffixMin + 1));
        r.out = outTokens;
        g_firstArrivalMs = std::min(g_firstArrivalMs, r.arrivalMs);
    }

    // ---------------- network ----------------
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(bufBytes));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(bufBytes));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(8948));

    g_inst.assign(g_n, Inst());
    g_nodes.Create(g_n);
    Ptr<Node> sw = CreateObject<Node>();
    InternetStackHelper internet;
    internet.Install(g_nodes);
    internet.Install(sw);

    TrafficControlHelper tch;
    Ipv4AddressHelper addr;
    const std::string qs = std::to_string(queuePackets) + "p";
    g_addr.resize(g_n);
    for (uint32_t i = 0; i < g_n; ++i)
    {
        g_inst[i].rateMbps = rates[i];
        g_inst[i].delayMs = delays[i];
        PointToPointHelper p2p;
        p2p.SetDeviceAttribute("DataRate", StringValue(std::to_string(rates[i]) + "Mbps"));
        p2p.SetDeviceAttribute("Mtu", UintegerValue(9000));
        p2p.SetChannelAttribute("Delay", TimeValue(MicroSeconds(delays[i] * 1000.0)));
        p2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue(qs));
        NetDeviceContainer d = p2p.Install(g_nodes.Get(i), sw); // d.Get(0)=instance, d.Get(1)=switch
        addr.SetBase(("10.1." + std::to_string(i + 1) + ".0").c_str(), "255.255.255.0");
        Ipv4InterfaceContainer ifc = addr.Assign(d);
        g_addr[i] = ifc.GetAddress(0);
        if (qdisc == "none")
        {
            tch.Uninstall(d);
        }
        d.Get(0)->TraceConnectWithoutContext("PhyTxBegin", MakeBoundCallback(&OnTx, &g_inst[i].bytesUp));
        d.Get(1)->TraceConnectWithoutContext("PhyTxBegin", MakeBoundCallback(&OnTx, &g_inst[i].bytesDown));
    }

    // Optional background congestion: UDP into instance i's downlink.
    for (uint32_t i = 0; i < g_n; ++i)
    {
        if (bgs[i] <= 0)
        {
            continue;
        }
        Ptr<Node> b = CreateObject<Node>();
        internet.Install(b);
        PointToPointHelper p2p;
        p2p.SetDeviceAttribute("DataRate", StringValue("100Gbps"));
        p2p.SetDeviceAttribute("Mtu", UintegerValue(9000));
        p2p.SetChannelAttribute("Delay", TimeValue(MicroSeconds(10)));
        p2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue(qs));
        NetDeviceContainer d = p2p.Install(b, sw);
        addr.SetBase(("10.2." + std::to_string(i + 1) + ".0").c_str(), "255.255.255.0");
        addr.Assign(d);
        if (qdisc == "none")
        {
            tch.Uninstall(d);
        }
        PacketSinkHelper usink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), 9));
        usink.Install(g_nodes.Get(i)).Start(Seconds(0));
        OnOffHelper on("ns3::UdpSocketFactory", InetSocketAddress(g_addr[i], 9));
        on.SetAttribute("DataRate", StringValue(std::to_string(bgs[i]) + "Mbps"));
        on.SetAttribute("PacketSize", UintegerValue(8000));
        on.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=" + std::to_string(bgOnS) + "]"));
        on.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=" + std::to_string(bgOffS) + "]"));
        ApplicationContainer oa = on.Install(b);
        oa.Start(Seconds(bgStartS));
    }
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // ---------------- preload KV: every group has one "home" instance ----------------
    for (uint32_t k = 0; k < groups; ++k)
    {
        InsertEntry(static_cast<int>(k % g_n), perm[k]);
    }

    for (uint32_t i = 0; i < requests; ++i)
    {
        Simulator::Schedule(MilliSeconds(g_reqs[i].arrivalMs), &OnArrival, i);
    }
    Simulator::Schedule(MilliSeconds(g_telemetryMs), &TelemetryTick);
    Simulator::Stop(Seconds(3600));
    Simulator::Run();

    // ---------------- metrics ----------------
    const double endMs = NowMs();
    std::vector<double> ttft, e2e, pullAct, pullPred, pullErr, pullBias;
    uint32_t hit = 0, pull = 0, rec = 0;
    double pullBytes = 0;
    for (auto& r : g_reqs)
    {
        ttft.push_back(r.ttftMs);
        e2e.push_back(r.e2eMs);
        if (r.kind == Kind::HIT)
        {
            ++hit;
        }
        else if (r.kind == Kind::RECOMPUTE)
        {
            ++rec;
        }
        else
        {
            ++pull;
            pullBytes += nakr::KvBytes(g_mp, g_groupTokens[r.group]);
            pullAct.push_back(r.pullActualMs);
            pullPred.push_back(r.predPullMs);
            pullErr.push_back(std::fabs(r.predPullMs - r.pullActualMs) / std::max(1e-6, r.pullActualMs));
            pullBias.push_back((r.predPullMs - r.pullActualMs) / std::max(1e-6, r.pullActualMs));
        }
    }
    const double makespanMs = std::max(1.0, g_lastFinishMs - g_firstArrivalMs);
    double gpuSum = 0, gpuMax = 0, netSum = 0, netMax = 0;
    for (auto& I : g_inst)
    {
        double gu = std::min(1.0, I.busyTotalMs / makespanMs);
        gpuSum += gu;
        gpuMax = std::max(gpuMax, gu);
        double nu = I.bytesDown * 8.0 / (I.rateMbps * 1e6 * (endMs / 1000.0));
        netSum += nu;
        netMax = std::max(netMax, nu);
    }
    const double n = static_cast<double>(g_reqs.size());

    std::ostringstream row;
    row << std::fixed << std::setprecision(3);
    row << scenario << "," << g_policy << "," << seed << "," << requests << "," << rate << "," << g_n << ",\""
        << linkMbps << "\",\"" << bgMbps << "\"," << g_alpha << "," << g_telemetryMs << ","
        << Mean(ttft) << "," << Pct(ttft, 50) << "," << Pct(ttft, 95) << "," << Pct(ttft, 99) << ","
        << Mean(e2e) << "," << Pct(e2e, 95) << "," << Pct(e2e, 99) << ","
        << (hit + pull) / n << "," << hit << "," << pull << "," << rec << ","
        << pullBytes / (1024.0 * 1024.0 * 1024.0) << "," << Mean(pullAct) << "," << Mean(pullPred) << ","
        << Mean(pullErr) * 100.0 << "," << Mean(pullBias) * 100.0 << ","
        << gpuSum / g_n * 100.0 << "," << gpuMax * 100.0 << "," << netSum / g_n * 100.0 << ","
        << netMax * 100.0 << "," << n / (makespanMs / 1000.0);
    std::cout << row.str() << "\n";
    if (!summaryCsv.empty())
    {
        std::ofstream out(summaryCsv, std::ios::app);
        if (out.tellp() == 0)
        {
            out << "scenario,policy,seed,requests,rate_rps,instances,link_mbps,bg_mbps,ewma_alpha,telemetry_ms,"
                   "ttft_mean_ms,ttft_p50_ms,ttft_p95_ms,ttft_p99_ms,e2e_mean_ms,e2e_p95_ms,e2e_p99_ms,"
                   "kv_reuse_rate,local_hits,pulls,recomputes,pulled_gib,pull_actual_ms,pull_pred_ms,"
                   "pull_mape_pct,pull_bias_pct,gpu_util_mean_pct,gpu_util_max_pct,"
                   "net_down_util_mean_pct,net_down_util_max_pct,throughput_rps\n";
        }
        out << row.str() << "\n";
    }
    if (!perRequestCsv.empty())
    {
        std::ofstream out(perRequestCsv);
        out << "id,arrival_s,group,prefix_tokens,kind,target,source,est_ttft_ms,ttft_ms,e2e_ms,"
               "pred_pull_ms,actual_pull_ms\n";
        out << std::fixed << std::setprecision(3);
        for (auto& r : g_reqs)
        {
            out << r.id << "," << r.arrivalMs / 1000.0 << "," << r.group << "," << g_groupTokens[r.group]
                << "," << KindName(r.kind) << "," << r.target << "," << r.source << "," << r.estTtftMs
                << "," << r.ttftMs << "," << r.e2eMs << "," << r.predPullMs << "," << r.pullActualMs
                << "\n";
        }
    }
    Simulator::Destroy();
    return g_completed == g_reqs.size() ? 0 : 1;
}
