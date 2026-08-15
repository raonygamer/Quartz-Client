#include "quartz/client/ui/pages/FirmwarePage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void FirmwarePage::render(PageContext& context, PageManager& manager)
    {
        auto& deviceState = context.deviceState;
        PerformanceSnapshot performance{};
        MatrixTimingProbeResult<ActiveProbeRows> timingProbe{};
        bool hasPerformance = false;
        bool hasTimingProbe = false;
        std::uint64_t receivedPackets = 0;
        {
            std::lock_guard lock(deviceState.Mutex);
            performance = deviceState.Performance;
            timingProbe = deviceState.TimingProbe;
            hasPerformance = deviceState.HasPerformance;
            hasTimingProbe = deviceState.HasTimingProbe;
            receivedPackets = deviceState.ReceivedPackets;
        }

        (void)manager;
        drawFirmwarePage(performance, hasPerformance, timingProbe, hasTimingProbe);
    }
}
