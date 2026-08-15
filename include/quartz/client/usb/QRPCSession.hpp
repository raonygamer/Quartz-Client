#pragma once
#include "USBTransport.hpp"

namespace quartz::client
{
    class QRPCSession final
    {
    public:
        using PacketHandler = std::function<void(const PacketHeader&)>;
        using PacketObserver = std::function<void(bool, const PacketHeader&, std::span<const std::byte>)>;

        explicit QRPCSession(USBTransport& transport);
        ~QRPCSession();
        QRPCSession(const QRPCSession&) = delete;
        QRPCSession& operator=(const QRPCSession&) = delete;
        QRPCSession(QRPCSession&&) = delete;
        QRPCSession& operator=(QRPCSession&&) = delete;

        bool send(const PacketBuffer& packet) noexcept;
        void reset() noexcept;
        void setPacketHandler(PacketHandler handler);
        void setPacketObserver(PacketObserver observer);

    private:
        void consume(std::span<const std::byte> bytes);
        void dispatch(bool tx, const PacketHeader& header, std::span<const std::byte> bytes);

        USBTransport& _transport;
        std::mutex _callbackMutex;
        PacketHandler _packetHandler;
        PacketObserver _packetObserver;
        std::vector<std::byte> _rxAssembly;
    };
}
