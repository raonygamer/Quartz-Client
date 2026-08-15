#pragma once
#include "quartz/client/Common.hpp"

namespace quartz::client
{
    struct PacketBuffer
    {
        alignas(4) std::array<std::byte, 512> Data{};
        std::size_t Size = 0;
    };

    struct USBStatsSnapshot
    {
        std::uint64_t TxBytes = 0;
        std::uint64_t RxBytes = 0;
        std::uint64_t TxTransfers = 0;
        std::uint64_t RxTransfers = 0;
        std::uint64_t TxErrors = 0;
        std::uint64_t RxErrors = 0;
        std::uint64_t Connects = 0;
        std::uint64_t Disconnects = 0;
        double LastTxMilliseconds = 0.0;
        double LastRxMilliseconds = 0.0;
    };

    struct USBDeviceConfig
    {
        std::uint16_t Vendor = VendorId;
        std::uint16_t Product = ProductId;
        int Interface = RPCInterfaceNumber;
        std::uint8_t OutEndpoint = RPCOutEndpoint;
        std::uint8_t InEndpoint = RPCInEndpoint;
        unsigned int SendTimeoutMs = 20;
        unsigned int ReceiveTimeoutMs = 100;
    };
}
