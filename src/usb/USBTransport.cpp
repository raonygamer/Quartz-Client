#include "quartz/client/usb/USBTransport.hpp"

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
