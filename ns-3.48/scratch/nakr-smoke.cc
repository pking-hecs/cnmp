
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace ns3;

static std::vector<uint64_t> receivedBytes;
static std::vector<double> firstRxTime;
static std::vector<double> lastRxTime;

static void
OnRx(uint32_t id, Ptr<const Packet> packet, const Address& from)
{
    (void)from;

    double now = Simulator::Now().GetSeconds();

    if (firstRxTime[id] < 0.0)
    {
        firstRxTime[id] = now;
    }

    lastRxTime[id] = now;
    receivedBytes[id] += packet->GetSize();
}

// Regular function: NS-3 MakeBoundCallback can bind the sender ID.
static void
OnRxBound(uint32_t id, Ptr<const Packet> packet, const Address& from)
{
    OnRx(id, packet, from);
}

int
main(int argc, char* argv[])
{
    uint32_t numSenders = 1;
    uint32_t transferBytes = 10 * 1024 * 1024;
    std::string linkRate = "10Mbps";
    std::string linkDelay = "10ms";

    CommandLine cmd(__FILE__);
    cmd.AddValue("numSenders", "Number of simultaneous TCP senders", numSenders);
    cmd.AddValue("transferBytes", "Bytes sent by each sender", transferBytes);
    cmd.AddValue("linkRate", "Shared bottleneck link rate", linkRate);
    cmd.AddValue("linkDelay", "One-way bottleneck link delay", linkDelay);
    cmd.Parse(argc, argv);

    if (numSenders == 0 || transferBytes == 0)
    {
        std::cerr << "numSenders and transferBytes must be greater than zero.\n";
        return 1;
    }

    receivedBytes.assign(numSenders, 0);
    firstRxTime.assign(numSenders, -1.0);
    lastRxTime.assign(numSenders, -1.0);

    // Topology: N senders -> router -> one receiver.
    NodeContainer senders;
    senders.Create(numSenders);

    Ptr<Node> router = CreateObject<Node>();
    Ptr<Node> receiver = CreateObject<Node>();

    InternetStackHelper internet;
    internet.Install(senders);
    internet.Install(router);
    internet.Install(receiver);

    // Each sender has a fast, low-delay access link to the router.
    PointToPointHelper access;
    access.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    access.SetChannelAttribute("Delay", StringValue("1ms"));

    Ipv4AddressHelper address;

    for (uint32_t i = 0; i < numSenders; ++i)
    {
        NetDeviceContainer devices =
            access.Install(senders.Get(i), router);

        std::string subnet =
            "10." + std::to_string(i + 1) + ".1.0";

        address.SetBase(subnet.c_str(), "255.255.255.0");
        address.Assign(devices);
    }

    // All senders share this router-to-receiver bottleneck.
    PointToPointHelper bottleneck;
    bottleneck.SetDeviceAttribute("DataRate", StringValue(linkRate));
    bottleneck.SetChannelAttribute("Delay", StringValue(linkDelay));

    NetDeviceContainer bottleneckDevices =
        bottleneck.Install(router, receiver);

    address.SetBase("192.168.1.0", "255.255.255.0");
    Ipv4InterfaceContainer bottleneckIfaces =
        address.Assign(bottleneckDevices);

    // Populate routes after all interfaces have been assigned.
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    const Ipv4Address receiverAddress = bottleneckIfaces.GetAddress(1);
    const uint16_t basePort = 5000;

    for (uint32_t i = 0; i < numSenders; ++i)
    {
        const uint16_t port = basePort + i;

        // A separate TCP listening port for each sender.
        PacketSinkHelper sinkHelper(
            "ns3::TcpSocketFactory",
            InetSocketAddress(Ipv4Address::GetAny(), port));

        ApplicationContainer sinkApps =
            sinkHelper.Install(receiver);

        sinkApps.Get(0)->TraceConnectWithoutContext(
            "Rx",
            MakeBoundCallback(&OnRxBound, i));

        sinkApps.Start(Seconds(0.0));
        sinkApps.Stop(Seconds(100.0));

        BulkSendHelper sender(
            "ns3::TcpSocketFactory",
            InetSocketAddress(receiverAddress, port));

        sender.SetAttribute("MaxBytes", UintegerValue(transferBytes));
        sender.SetAttribute("SendSize", UintegerValue(65536));

        ApplicationContainer senderApps =
            sender.Install(senders.Get(i));

        senderApps.Start(Seconds(1.0));
        senderApps.Stop(Seconds(99.0));
    }

    Simulator::Stop(Seconds(100.0));
    Simulator::Run();

    uint64_t aggregateBytes = 0;
    double latestFinish = -1.0;
    bool allComplete = true;

    std::cout << "\n========== NAKR Concurrent TCP Test ==========\n";
    std::cout << "Number of senders: " << numSenders << "\n";
    std::cout << "Shared bottleneck: " << linkRate << "\n";
    std::cout << "One-way bottleneck delay: " << linkDelay << "\n";
    std::cout << "Target bytes per sender: " << transferBytes << "\n";

    for (uint32_t i = 0; i < numSenders; ++i)
    {
        aggregateBytes += receivedBytes[i];

        const bool complete = (receivedBytes[i] == transferBytes);
        allComplete = allComplete && complete;

        std::cout << "\nSender " << i
                  << ": received " << receivedBytes[i] << " bytes"
                  << (complete ? " [COMPLETE]" : " [INCOMPLETE]")
                  << "\n";

        if (firstRxTime[i] >= 0.0)
        {
            std::cout << "  First byte: " << firstRxTime[i] << " s\n";
            std::cout << "  Last byte:  " << lastRxTime[i] << " s\n";

            const double elapsedSinceStart = lastRxTime[i] - 1.0;

            if (elapsedSinceStart > 0.0)
            {
                const double throughputMbps =
                    receivedBytes[i] * 8.0 /
                    elapsedSinceStart / 1e6;

                std::cout << "  Throughput since sender start: "
                          << throughputMbps << " Mbps\n";
            }

            if (latestFinish < lastRxTime[i])
            {
                latestFinish = lastRxTime[i];
            }
        }
    }

    std::cout << "\nAggregate received: " << aggregateBytes << " bytes\n";

    if (latestFinish > 1.0)
    {
        const double experimentDuration = latestFinish - 1.0;
        const double aggregateThroughput =
            aggregateBytes * 8.0 / experimentDuration / 1e6;

        std::cout << "Time until last received byte: "
                  << experimentDuration << " s\n";
        std::cout << "Aggregate throughput: "
                  << aggregateThroughput << " Mbps\n";
    }
    else
    {
        std::cout << "No received data to measure.\n";
    }

    std::cout << "Overall status: "
              << (allComplete ? "SUCCESS" : "INCOMPLETE")
              << "\n";
    std::cout << "==============================================\n";

    Simulator::Destroy();
    return allComplete ? 0 : 1;
}
