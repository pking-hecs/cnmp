// NAKR Phase 4: TCP transfer calibration.
//
// Same shape as the Phase 1 experiment (N senders -> router -> one receiver, shared bottleneck),
// but every knob that could explain the Phase 1 anomaly is exposed so it can be tested:
//   - TCP send/receive buffer size   (--bufBytes)
//   - bottleneck queueing            (--qdisc=default|none, --queuePackets)
//   - segment size                   (--mss)
// One run prints one CSV row (appended to --csvPath if given).
//
// CSV columns:
//   senders,bytes,link_mbps,delay_ms,rtt_ms,buf_bytes,mss,qdisc,queue_pkts,
//   completion_s,mean_flow_s,agg_goodput_mbps,all_complete

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace ns3;

static std::vector<uint64_t> g_rx;
static std::vector<double> g_last;

static void
OnRx(uint32_t id, Ptr<const Packet> p, const Address&)
{
    g_rx[id] += p->GetSize();
    g_last[id] = Simulator::Now().GetSeconds();
}

int
main(int argc, char* argv[])
{
    uint32_t senders = 1;
    uint64_t bytes = 10 * 1024 * 1024;
    double linkMbps = 100.0;
    double delayMs = 10.0;
    double accessMbps = 1000.0;
    double accessDelayMs = 1.0;
    uint32_t bufBytes = 131072; // ns-3 default TcpSocket Snd/RcvBufSize
    uint32_t mss = 1448;
    uint32_t mtu = 1500;
    uint32_t queuePackets = 100; // ns-3 default device queue
    std::string qdisc = "default";
    std::string csvPath;
    double startS = 1.0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("senders", "Concurrent TCP senders", senders);
    cmd.AddValue("bytes", "Bytes per sender", bytes);
    cmd.AddValue("linkMbps", "Bottleneck rate (Mbps)", linkMbps);
    cmd.AddValue("delayMs", "Bottleneck one-way delay (ms)", delayMs);
    cmd.AddValue("accessMbps", "Sender access link rate (Mbps)", accessMbps);
    cmd.AddValue("accessDelayMs", "Sender access one-way delay (ms)", accessDelayMs);
    cmd.AddValue("bufBytes", "TCP send+receive buffer (bytes)", bufBytes);
    cmd.AddValue("mss", "TCP segment size (bytes)", mss);
    cmd.AddValue("mtu", "Device MTU (bytes); use 9000 with mss=8948", mtu);
    cmd.AddValue("queuePackets", "Device queue size (packets)", queuePackets);
    cmd.AddValue("qdisc", "default (ns-3 default queue disc) or none", qdisc);
    cmd.AddValue("csvPath", "Append one CSV row here", csvPath);
    cmd.Parse(argc, argv);

    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(bufBytes));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(bufBytes));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(mss));

    g_rx.assign(senders, 0);
    g_last.assign(senders, -1.0);

    NodeContainer snd;
    snd.Create(senders);
    Ptr<Node> router = CreateObject<Node>();
    Ptr<Node> recv = CreateObject<Node>();
    InternetStackHelper internet;
    internet.Install(snd);
    internet.Install(router);
    internet.Install(recv);

    std::string qs = std::to_string(queuePackets) + "p";
    TrafficControlHelper tch;
    Ipv4AddressHelper addr;

    PointToPointHelper access;
    access.SetDeviceAttribute("DataRate", StringValue(std::to_string(accessMbps) + "Mbps"));
    access.SetChannelAttribute("Delay", StringValue(std::to_string(accessDelayMs) + "ms"));
    access.SetDeviceAttribute("Mtu", UintegerValue(mtu));
    access.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue(qs));
    for (uint32_t i = 0; i < senders; ++i)
    {
        NetDeviceContainer d = access.Install(snd.Get(i), router);
        addr.SetBase(("10." + std::to_string(i + 1) + ".1.0").c_str(), "255.255.255.0");
        addr.Assign(d);
        if (qdisc == "none")
        {
            tch.Uninstall(d);
        }
    }

    PointToPointHelper bn;
    bn.SetDeviceAttribute("DataRate", StringValue(std::to_string(linkMbps) + "Mbps"));
    bn.SetChannelAttribute("Delay", StringValue(std::to_string(delayMs) + "ms"));
    bn.SetDeviceAttribute("Mtu", UintegerValue(mtu));
    bn.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue(qs));
    NetDeviceContainer bd = bn.Install(router, recv);
    addr.SetBase("192.168.1.0", "255.255.255.0");
    Ipv4InterfaceContainer bi = addr.Assign(bd);
    if (qdisc == "none")
    {
        tch.Uninstall(bd);
    }
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    for (uint32_t i = 0; i < senders; ++i)
    {
        uint16_t port = 5000 + i;
        PacketSinkHelper sink("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer sa = sink.Install(recv);
        sa.Get(0)->TraceConnectWithoutContext("Rx", MakeBoundCallback(&OnRx, i));
        sa.Start(Seconds(0.0));
        BulkSendHelper bulk("ns3::TcpSocketFactory", InetSocketAddress(bi.GetAddress(1), port));
        bulk.SetAttribute("MaxBytes", UintegerValue(bytes));
        bulk.SetAttribute("SendSize", UintegerValue(65536));
        ApplicationContainer ba = bulk.Install(snd.Get(i));
        ba.Start(Seconds(startS));
    }

    Simulator::Stop(Seconds(3600));
    Simulator::Run();

    bool ok = true;
    double maxT = 0, sumT = 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < senders; ++i)
    {
        ok = ok && g_rx[i] == bytes;
        total += g_rx[i];
        double t = g_last[i] - startS;
        maxT = std::max(maxT, t);
        sumT += t;
    }
    double rttMs = 2.0 * (delayMs + accessDelayMs);
    double goodput = maxT > 0 ? total * 8.0 / maxT / 1e6 : 0;

    std::ostringstream row;
    row << std::fixed << std::setprecision(4) << senders << "," << bytes << "," << linkMbps << ","
        << delayMs << "," << rttMs << "," << bufBytes << "," << mss << "," << qdisc << ","
        << queuePackets << "," << maxT << "," << sumT / senders << "," << goodput << ","
        << (ok ? "true" : "false");
    std::cout << row.str() << "\n";
    if (!csvPath.empty())
    {
        std::ofstream out(csvPath, std::ios::app);
        out << row.str() << "\n";
    }
    Simulator::Destroy();
    return ok ? 0 : 1;
}
