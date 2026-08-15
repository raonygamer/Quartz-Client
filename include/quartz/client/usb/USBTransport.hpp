#pragma once
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
