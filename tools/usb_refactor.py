from pathlib import Path

root = Path(__file__).resolve().parents[1]
inc = root / "include/quartz/client"
src = root / "src"
usb_inc = inc / "usb"
usb_src = src / "usb"
usb_inc.mkdir(parents=True, exist_ok=True)
usb_src.mkdir(parents=True, exist_ok=True)

(usb_inc / "USBTypes.hpp").write_text(r'''#pragma once
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
''')

(usb_inc / "USBTransport.hpp").write_text(r'''#pragma once
#include "USBTypes.hpp"

namespace quartz::client
{
    class USBTransport final
    {
    public:
        using ReceiveHandler = std::function<void(std::span<const std::byte>)>;

        explicit USBTransport(USBDeviceConfig config = {});
        ~USBTransport();
        USBTransport(const USBTransport&) = delete;
        USBTransport& operator=(const USBTransport&) = delete;
        USBTransport(USBTransport&&) = delete;
        USBTransport& operator=(USBTransport&&) = delete;

        bool initialize() noexcept;
        bool connect() noexcept;
        void disconnect() noexcept;
        void shutdown() noexcept;
        bool send(std::span<const std::byte> bytes) noexcept;
        void setReceiveHandler(ReceiveHandler handler);

        [[nodiscard]] bool isConnected() const noexcept { return _connected.load(std::memory_order_acquire); }
        [[nodiscard]] int lastError() const noexcept { return _lastError.load(std::memory_order_acquire); }
        [[nodiscard]] const std::string& deviceName() const noexcept { return _deviceName; }
        [[nodiscard]] USBStatsSnapshot stats() const noexcept;
        [[nodiscard]] const USBDeviceConfig& config() const noexcept { return _config; }

    private:
        void receiveLoop() noexcept;
        void dispatchReceived(std::span<const std::byte> bytes);

        USBDeviceConfig _config;
        libusb_context* _context = nullptr;
        libusb_device_handle* _handle = nullptr;
        std::thread _receiveThread;
        std::atomic_bool _running = false;
        std::atomic_bool _connected = false;
        std::atomic_int _lastError = LIBUSB_SUCCESS;
        std::atomic_uint64_t _txBytes{0}, _rxBytes{0}, _txTransfers{0}, _rxTransfers{0}, _txErrors{0}, _rxErrors{0}, _connects{0}, _disconnects{0};
        std::atomic<double> _lastTxMicroseconds{0.0}, _lastRxMicroseconds{0.0};
        std::mutex _handlerMutex;
        ReceiveHandler _receiveHandler;
        std::string _deviceName;
    };
}
''')

(usb_inc / "QRPCSession.hpp").write_text(r'''#pragma once
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
''')

(usb_inc / "RawUSB.hpp").write_text(r'''#pragma once
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
''')

(usb_inc / "USB.hpp").write_text(r'''#pragma once
#include "USBTypes.hpp"
#include "USBTransport.hpp"
#include "QRPCSession.hpp"
#include "RawUSB.hpp"
''')

(usb_src / "USBTransport.cpp").write_text(r'''#include "quartz/client/usb/USBTransport.hpp"

namespace quartz::client
{
    USBTransport::USBTransport(const USBDeviceConfig config) : _config(config) {}
    USBTransport::~USBTransport() { shutdown(); }

    bool USBTransport::initialize() noexcept
    {
        if (_context) return true;
        const int result = libusb_init(&_context);
        _lastError.store(result, std::memory_order_release);
        return result == LIBUSB_SUCCESS;
    }

    bool USBTransport::connect() noexcept
    {
        if (_connected.load(std::memory_order_acquire)) return true;
        if (!_context && !initialize()) return false;
        if (_handle) disconnect();

        _handle = libusb_open_device_with_vid_pid(_context, _config.Vendor, _config.Product);
        if (!_handle)
        {
            _lastError.store(LIBUSB_ERROR_NO_DEVICE, std::memory_order_release);
            return false;
        }

        _deviceName.clear();
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(libusb_get_device(_handle), &descriptor) == LIBUSB_SUCCESS && descriptor.iProduct != 0)
        {
            std::array<unsigned char, 256> product{};
            const int length = libusb_get_string_descriptor_ascii(_handle, descriptor.iProduct, product.data(), static_cast<int>(product.size() - 1));
            if (length > 0) _deviceName.assign(reinterpret_cast<const char*>(product.data()), static_cast<std::size_t>(length));
        }
        if (_deviceName.empty()) _deviceName = "Quartz K552X";

        const int detachResult = libusb_set_auto_detach_kernel_driver(_handle, 1);
        if (detachResult != LIBUSB_SUCCESS && detachResult != LIBUSB_ERROR_NOT_SUPPORTED)
        {
            _lastError.store(detachResult, std::memory_order_release);
            libusb_close(_handle);
            _handle = nullptr;
            return false;
        }

        const int claimResult = libusb_claim_interface(_handle, _config.Interface);
        if (claimResult != LIBUSB_SUCCESS)
        {
            _lastError.store(claimResult, std::memory_order_release);
            libusb_close(_handle);
            _handle = nullptr;
            return false;
        }

        _running.store(true, std::memory_order_release);
        _connected.store(true, std::memory_order_release);
        _lastError.store(LIBUSB_SUCCESS, std::memory_order_release);
        _connects.fetch_add(1, std::memory_order_relaxed);
        try { _receiveThread = std::thread(&USBTransport::receiveLoop, this); }
        catch (...)
        {
            _running.store(false, std::memory_order_release);
            _connected.store(false, std::memory_order_release);
            libusb_release_interface(_handle, _config.Interface);
            libusb_close(_handle);
            _handle = nullptr;
            _lastError.store(LIBUSB_ERROR_OTHER, std::memory_order_release);
            return false;
        }
        return true;
    }

    void USBTransport::disconnect() noexcept
    {
        const bool hadDevice = _handle != nullptr || _connected.load(std::memory_order_acquire);
        _running.store(false, std::memory_order_release);
        _connected.store(false, std::memory_order_release);
        if (_receiveThread.joinable()) _receiveThread.join();
        if (_handle)
        {
            libusb_release_interface(_handle, _config.Interface);
            libusb_close(_handle);
            _handle = nullptr;
        }
        _deviceName.clear();
        if (hadDevice) _disconnects.fetch_add(1, std::memory_order_relaxed);
    }

    void USBTransport::shutdown() noexcept
    {
        disconnect();
        if (_context)
        {
            libusb_exit(_context);
            _context = nullptr;
        }
    }

    bool USBTransport::send(const std::span<const std::byte> bytes) noexcept
    {
        if (!_handle || !_connected.load(std::memory_order_acquire) || bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
        int transferred = 0;
        const auto started = std::chrono::steady_clock::now();
        const int result = libusb_bulk_transfer(_handle, _config.OutEndpoint, reinterpret_cast<unsigned char*>(const_cast<std::byte*>(bytes.data())), static_cast<int>(bytes.size()), &transferred, _config.SendTimeoutMs);
        const auto finished = std::chrono::steady_clock::now();
        _lastTxMicroseconds.store(std::chrono::duration<double, std::micro>(finished - started).count(), std::memory_order_relaxed);
        _txTransfers.fetch_add(1, std::memory_order_relaxed);
        if (transferred > 0) _txBytes.fetch_add(static_cast<std::uint64_t>(transferred), std::memory_order_relaxed);
        if (result != LIBUSB_SUCCESS || transferred != static_cast<int>(bytes.size())) _txErrors.fetch_add(1, std::memory_order_relaxed);
        _lastError.store(result, std::memory_order_release);
        if (result == LIBUSB_ERROR_NO_DEVICE)
        {
            _running.store(false, std::memory_order_release);
            _connected.store(false, std::memory_order_release);
        }
        return result == LIBUSB_SUCCESS && transferred == static_cast<int>(bytes.size());
    }

    void USBTransport::setReceiveHandler(ReceiveHandler handler)
    {
        std::lock_guard lock(_handlerMutex);
        _receiveHandler = std::move(handler);
    }

    USBStatsSnapshot USBTransport::stats() const noexcept
    {
        return {
            _txBytes.load(std::memory_order_relaxed), _rxBytes.load(std::memory_order_relaxed),
            _txTransfers.load(std::memory_order_relaxed), _rxTransfers.load(std::memory_order_relaxed),
            _txErrors.load(std::memory_order_relaxed), _rxErrors.load(std::memory_order_relaxed),
            _connects.load(std::memory_order_relaxed), _disconnects.load(std::memory_order_relaxed),
            _lastTxMicroseconds.load(std::memory_order_relaxed) / 1000.0, _lastRxMicroseconds.load(std::memory_order_relaxed) / 1000.0
        };
    }

    void USBTransport::receiveLoop() noexcept
    {
        std::array<unsigned char, 512> buffer{};
        while (_running.load(std::memory_order_acquire))
        {
            int transferred = 0;
            const auto started = std::chrono::steady_clock::now();
            const int result = libusb_bulk_transfer(_handle, _config.InEndpoint, buffer.data(), static_cast<int>(buffer.size()), &transferred, _config.ReceiveTimeoutMs);
            const auto finished = std::chrono::steady_clock::now();
            if (result == LIBUSB_ERROR_TIMEOUT) continue;
            _lastRxMicroseconds.store(std::chrono::duration<double, std::micro>(finished - started).count(), std::memory_order_relaxed);
            _rxTransfers.fetch_add(1, std::memory_order_relaxed);
            if (transferred > 0) _rxBytes.fetch_add(static_cast<std::uint64_t>(transferred), std::memory_order_relaxed);
            _lastError.store(result, std::memory_order_release);
            if (result != LIBUSB_SUCCESS)
            {
                _rxErrors.fetch_add(1, std::memory_order_relaxed);
                if (result == LIBUSB_ERROR_NO_DEVICE) _connected.store(false, std::memory_order_release);
                break;
            }
            if (transferred > 0) dispatchReceived(std::as_bytes(std::span(buffer.data(), static_cast<std::size_t>(transferred))));
        }
        _running.store(false, std::memory_order_release);
    }

    void USBTransport::dispatchReceived(const std::span<const std::byte> bytes)
    {
        ReceiveHandler handler;
        {
            std::lock_guard lock(_handlerMutex);
            handler = _receiveHandler;
        }
        if (handler) handler(bytes);
    }
}
''')

(usb_src / "QRPCSession.cpp").write_text(r'''#include "quartz/client/usb/QRPCSession.hpp"

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
''')

(usb_src / "RawUSB.cpp").write_text(r'''#include "quartz/client/usb/RawUSB.hpp"

namespace quartz::client
{
    RawUSB::RawUSB() : _transport(), _session(_transport) {}
    RawUSB::~RawUSB() { shutdown(); }

    bool RawUSB::connect() noexcept
    {
        _session.reset();
        return _transport.connect();
    }

    void RawUSB::disconnect() noexcept
    {
        _transport.disconnect();
        _session.reset();
    }

    void RawUSB::shutdown() noexcept
    {
        _transport.shutdown();
        _session.reset();
    }
}
''')

# Preserve the non-template packet builder that the old monolith exposed.
(usb_src / "USB.cpp").write_text(r'''#include "quartz/client/Model.hpp"

namespace quartz::client
{
    PacketBuffer makePacket(const PacketType type)
    {
        PacketBuffer packet{};
        packet.Size = sizeof(PacketHeader);
        std::construct_at(reinterpret_cast<PacketHeader*>(packet.Data.data()), 1, type, PacketDirection::HostToDevice, 0, 0);
        return packet;
    }
}
''')

# Remove the old monolithic USB class/type blocks and include the real USB subsystem before the remaining model types.
model = inc / "Model.hpp"
text = model.read_text()
start = text.find("    struct PacketBuffer\n")
end = text.find("    struct AudioLevelSnapshot\n", start)
if start < 0 or end < 0:
    raise SystemExit("could not locate generated USB model block")
text = text[:start] + text[end:]
include = '#include "quartz/client/usb/USB.hpp"\n'
if include not in text:
    text = text.replace('#include "Functions.hpp"\n', '#include "Functions.hpp"\n' + include, 1)
model.write_text(text)

print("split RawUSB into USBTransport + QRPCSession + compatibility facade")
