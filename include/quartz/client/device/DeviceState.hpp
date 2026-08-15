#pragma once
#include "quartz/client/Functions.hpp"

namespace quartz::client
{
    struct PerformanceSnapshot
    {
        std::uint32_t CoreClock = 0;
        std::uint32_t BeginScanTicks = 0;
        std::uint32_t ScanTicks = 0;
        std::uint32_t EndScanTicks = 0;
        std::uint32_t StateUpdateTicks = 0;
        std::uint32_t HIDTicks = 0;
        std::uint32_t RGBTicks = 0;
        std::uint32_t AverageScanPeriodTicks = 0;
        std::uint32_t RGBSlotMaxTicks = 0;
    };

    struct SharedDeviceState
    {
        std::mutex Mutex;
        PerformanceSnapshot Performance{};
        MatrixTimingProbeResult<ActiveProbeRows> TimingProbe{};
        bool HasPerformance = false;
        bool HasTimingProbe = false;
        std::uint64_t ReceivedPackets = 0;
    };


}
