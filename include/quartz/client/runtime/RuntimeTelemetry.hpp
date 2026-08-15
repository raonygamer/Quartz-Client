#pragma once
#include "quartz/client/runtime/RuntimeTypes.hpp"

namespace quartz::client
{
    class RuntimeTelemetry
    {
    public:
        void event(const double time, std::string category, std::string text)
        {
            std::lock_guard lock(_mutex);
            _events.push_back({time, std::move(category), std::move(text)});
            while (_events.size() > 512) _events.pop_front();
        }

        void packet(const double time, const bool tx, const PacketHeader& header, const std::span<const std::byte> bytes)
        {
            std::lock_guard lock(_mutex);
            RuntimePacketRecord record;
            record.Time = time;
            record.Tx = tx;
            record.Type = static_cast<std::uint16_t>(header.Type);
            record.Version = header.Version;
            record.PacketId = header.PacketId;
            record.ResponseFor = header.ResponseFor;
            record.PayloadLength = header.PayloadLength;
            record.Bytes = bytes.size();
            record.CapturedBytes = std::min(bytes.size(), record.Data.size());
            std::copy_n(bytes.begin(), record.CapturedBytes, record.Data.begin());
            _packets.push_back(record);
            while (_packets.size() > 512) _packets.pop_front();
        }

        std::deque<RuntimeTimelineEvent> events() const { std::lock_guard lock(_mutex); return _events; }
        std::deque<RuntimePacketRecord> packets() const { std::lock_guard lock(_mutex); return _packets; }
        void clearEvents() { std::lock_guard lock(_mutex); _events.clear(); }
        void clearPackets() { std::lock_guard lock(_mutex); _packets.clear(); }

    private:
        mutable std::mutex _mutex;
        std::deque<RuntimeTimelineEvent> _events;
        std::deque<RuntimePacketRecord> _packets;
    };

}
