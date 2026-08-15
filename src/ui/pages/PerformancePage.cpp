#include "quartz/client/ui/pages/PerformancePage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void PerformancePage::render(PageContext& context, PageManager& manager)
    {
        auto& settings = context.settings;
        const auto& framebuffer = context.framebuffer;
        const auto sentFrames = context.sentFrames;
        const auto droppedFrames = context.droppedFrames;
        const auto appCpuUsage = context.appCpuUsage;
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
        const std::size_t framebufferPacketBytes = sizeof(PacketHeader) + sizeof(FramebufferSetPayload<MatrixSize>);
        const double configuredTxKiB = settings.Enabled && settings.SendFramebuffer ? framebufferPacketBytes * static_cast<double>(settings.FrameRate) / 1024.0 : 0.0;
        ImGui::Text("Host CPU: %.2f%%   target framebuffer rate: %d Hz   estimated TX: %.1f KiB/s", appCpuUsage, settings.FrameRate, configuredTxKiB);
        ImGui::Text("Packets received: %llu   frames sent: %llu   dropped/busy: %llu", static_cast<unsigned long long>(receivedPackets), static_cast<unsigned long long>(sentFrames), static_cast<unsigned long long>(droppedFrames));
        ImGui::Separator();
        if (hasPerformance)
            drawPerformance(performance);
        else
            ImGui::TextDisabled("Waiting for PerformanceResponse...");
    }
}
