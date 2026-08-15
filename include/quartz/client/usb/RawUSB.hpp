#pragma once
#include "QRPCSession.hpp"

namespace quartz::client
{
    class RawUSB final
    {
    public:
        using PacketHandler = QRPCSession::PacketHandler;
        using PacketObserver = QRPCSession::PacketObserver;

        RawUSB();
        ~RawUSB();
        RawUSB(const RawUSB&) = delete;
        RawUSB& operator=(const RawUSB&) = delete;
        RawUSB(RawUSB&&) = delete;
        RawUSB& operator=(RawUSB&&) = delete;

        bool initialize() noexcept { return _transport.initialize(); }
        bool connect() noexcept;
        void disconnect() noexcept;
        void shutdown() noexcept;
        bool send(const PacketBuffer& packet) noexcept { return _session.send(packet); }
        void setPacketHandler(PacketHandler handler) { _session.setPacketHandler(std::move(handler)); }
        void setPacketObserver(PacketObserver observer) { _session.setPacketObserver(std::move(observer)); }

        [[nodiscard]] bool isConnected() const noexcept { return _transport.isConnected(); }
        [[nodiscard]] int lastError() const noexcept { return _transport.lastError(); }
        [[nodiscard]] const std::string& deviceName() const noexcept { return _transport.deviceName(); }
        [[nodiscard]] USBStatsSnapshot stats() const noexcept { return _transport.stats(); }
        [[nodiscard]] USBTransport& transport() noexcept { return _transport; }
        [[nodiscard]] const USBTransport& transport() const noexcept { return _transport; }
        [[nodiscard]] QRPCSession& session() noexcept { return _session; }
        [[nodiscard]] const QRPCSession& session() const noexcept { return _session; }

    private:
        // Session must die first because it installs a receive callback that captures itself.
        USBTransport _transport;
        QRPCSession _session;
    };
}
