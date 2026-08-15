#include "quartz/client/usb/RawUSB.hpp"

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
