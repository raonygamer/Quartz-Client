#include "quartz/client/ui/pages/PerformancePage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/I18n.hpp"

namespace quartz::client::ui
{
    void PerformancePage::render(PageContext& context, PageManager& manager)
    {
        auto& settings=context.settings; const auto sentFrames=context.sentFrames,droppedFrames=context.droppedFrames,appCpuUsage=context.appCpuUsage; auto& deviceState=context.deviceState; PerformanceSnapshot performance{}; bool hasPerformance=false; std::uint64_t receivedPackets=0;
        { std::lock_guard lock(deviceState.Mutex); performance=deviceState.Performance; hasPerformance=deviceState.HasPerformance; receivedPackets=deviceState.ReceivedPackets; }
        (void)manager; const std::size_t framebufferPacketBytes=sizeof(PacketHeader)+sizeof(FramebufferSetPayload<MatrixSize>); const double configuredTxKiB=settings.Enabled&&settings.SendFramebuffer?framebufferPacketBytes*static_cast<double>(settings.FrameRate)/1024.0:0.0;
        ImGui::Text(i18n::tr("performance.host"),appCpuUsage,settings.FrameRate,configuredTxKiB); ImGui::Text(i18n::tr("performance.packets"),static_cast<unsigned long long>(receivedPackets),static_cast<unsigned long long>(sentFrames),static_cast<unsigned long long>(droppedFrames)); ImGui::Separator(); if (hasPerformance) drawPerformance(performance); else ImGui::TextDisabled("%s",i18n::tr("performance.waiting"));
    }
}
