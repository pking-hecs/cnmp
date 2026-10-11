
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <cstdint>
#include <iostream>

using namespace ns3;

static double firstRxTime = -1.0;
static double lastRxTime = -1.0;
static uint64_t totalRxBytes = 0;

static void
OnRx(Ptr<const Packet> packet, const Address &from)
{
    const double now = Simulator::Now().GetSeconds();

    if (firstRxTime < 0.0) {
        firstRxTime = now;
    }

    lastRxTime = now;
    totalRxBytes += packet->GetSize();
}

int
main(int argc, char *argv[])
{
    uint32_t transferBytes = 10 * 1024 * 1024;
    std::string linkRate = "100Mbps";
    std::string linkDelay = "10ms";

    CommandLine cmd(__FILE__);
    cmd.AddValue("transferBytes", "Bytes to transfer", transferBytes);
    cmd.AddValue("linkRate", "Point-to-point link rate", linkRate);
    cmd.AddValue("linkDelay", "One-way link delay", linkDelay);
    cmd.Parse(argc, argv);

    NodeContainer nodes;
    nodes.Create(2);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(linkRate));
    p2p.SetChannelAttribute("Delay", StringValue(linkDelay));

    NetDeviceContainer devices = p2p.Install(nodes);

    InternetStackHelper internet;
    internet.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    const uint16_t port = 5000;

    PacketSinkHelper sinkHelper(
        "ns3::TcpSocketFactory",
        InetSocketAddress(Ipv4Address::GetAny(), port));

    ApplicationContainer sinkApps = sinkHelper.Install(nodes.Get(1));
    sinkApps.Get(0)->TraceConnectWithoutContext(
        "Rx", MakeCallback(&OnRx));

    sinkApps.Start(Seconds(0.0));
    sinkApps.Stop(Seconds(30.0));

    BulkSendHelper sender(
        "ns3::TcpSocketFactory",
        InetSocketAddress(interfaces.GetAddress(1), port));

    sender.SetAttribute("MaxBytes", UintegerValue(transferBytes));
    sender.SetAttribute("SendSize", UintegerValue(65536));

    ApplicationContainer senderApps = sender.Install(nodes.Get(0));
    senderApps.Start(Seconds(1.0));
    senderApps.Stop(Seconds(29.0));

    Simulator::Stop(Seconds(30.0));
    Simulator::Run();

    const bool complete = (totalRxBytes == transferBytes);

    std::cout << "\n========== NAKR TCP Smoke Test ==========\n";
    std::cout << "Link rate: " << linkRate << "\n";
    std::cout << "One-way link delay: " << linkDelay << "\n";
    std::cout << "Configured bytes: " << transferBytes << "\n";
    std::cout << "Received bytes: " << totalRxBytes << "\n";
    std::cout << "Status: "
              << (complete ? "SUCCESS" : "INCOMPLETE") << "\n";

    if (firstRxTime >= 0.0 && lastRxTime >= firstRxTime) {
        const double duration = lastRxTime - firstRxTime;

        std::cout << "First received byte at: "
                  << firstRxTime << " s\n";
        std::cout << "Last received byte at: "
                  << lastRxTime << " s\n";

        if (duration > 0.0) {
            const double throughputMbps =
                totalRxBytes * 8.0 / duration / 1e6;

            std::cout << "Receive-span throughput: "
                      << throughputMbps << " Mbps\n";
        } else {
            std::cout << "Receive-span throughput: N/A "
                      << "(zero-duration receive span)\n";
        }

        const double senderElapsed = lastRxTime - 1.0;
        if (senderElapsed > 0.0) {
            const double senderThroughputMbps =
                totalRxBytes * 8.0 / senderElapsed / 1e6;

            std::cout << "Throughput since sender start: "
                      << senderThroughputMbps << " Mbps\n";
        }
    } else {
        std::cout << "ERROR: No received data to measure.\n";
    }

    std::cout << "=========================================\n";

    Simulator::Destroy();
    return complete ? 0 : 1;
}
