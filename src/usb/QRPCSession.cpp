#include "quartz/client/usb/QRPCSession.hpp"

namespace quartz::client
{
    QRPCSession::QRPCSession(USBTransport& transport) : _transport(transport)
    {
        _transport.setReceiveHandler([this](const std::span<const std::byte> bytes) { consume(bytes); });
    }

    QRPCSession::~QRPCSession()
    {
        _transport.setReceiveHandler({});
    }

    bool QRPCSession::send(const PacketBuffer& packet) noexcept
    {
        if (packet.Size == 0 || packet.Size > packet.Data.size()) return false;
        const std::span<const std::byte> bytes(packet.Data.data(), packet.Size);
        if (!_transport.send(bytes)) return false;
        if (const auto* header = PacketHeader::asPtr(bytes)) dispatch(true, *header, bytes);
        return true;
    }

    void QRPCSession::reset() noexcept
    {
        _rxAssembly.clear();
    }

    void QRPCSession::setPacketHandler(PacketHandler handler)
    {
        std::lock_guard lock(_callbackMutex);
        _packetHandler = std::move(handler);
    }

    void QRPCSession::setPacketObserver(PacketObserver observer)
    {
        std::lock_guard lock(_callbackMutex);
        _packetObserver = std::move(observer);
    }

    void QRPCSession::consume(const std::span<const std::byte> bytes)
    {
        _rxAssembly.insert(_rxAssembly.end(), bytes.begin(), bytes.end());
        while (_rxAssembly.size() >= sizeof(PacketHeader))
        {
            std::uint32_t magic = 0;
            std::memcpy(&magic, _rxAssembly.data(), sizeof(magic));
            if (magic != PacketHeader::MAGIC_NUMBER)
            {
                _rxAssembly.erase(_rxAssembly.begin());
                continue;
            }

            PacketBuffer packet{};
            std::memcpy(packet.Data.data(), _rxAssembly.data(), sizeof(PacketHeader));
            const auto* unchecked = reinterpret_cast<const PacketHeader*>(packet.Data.data());
            const std::size_t packetSize = sizeof(PacketHeader) + unchecked->PayloadLength;
            if (packetSize > packet.Data.size())
            {
                _rxAssembly.clear();
                return;
            }
            if (_rxAssembly.size() < packetSize) return;

            packet.Size = packetSize;
            std::memcpy(packet.Data.data(), _rxAssembly.data(), packetSize);
            const std::span<const std::byte> packetBytes(packet.Data.data(), packet.Size);
            if (const auto* header = PacketHeader::asPtr(packetBytes)) dispatch(false, *header, packetBytes);
            _rxAssembly.erase(_rxAssembly.begin(), _rxAssembly.begin() + static_cast<std::ptrdiff_t>(packetSize));
        }
    }

    void QRPCSession::dispatch(const bool tx, const PacketHeader& header, const std::span<const std::byte> bytes)
    {
        PacketHandler handler;
        PacketObserver observer;
        {
            std::lock_guard lock(_callbackMutex);
            handler = _packetHandler;
            observer = _packetObserver;
        }
        if (observer) observer(tx, header, bytes);
        if (!tx && handler) handler(header);
    }
}
