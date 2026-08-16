#include "quartz/client/ui/pages/MatrixTimingPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/I18n.hpp"

namespace quartz::client::ui
{
    void MatrixTimingPage::render(PageContext& context, PageManager& manager)
    {
        auto& deviceState=context.deviceState; MatrixTimingProbeResult<ActiveProbeRows> timingProbe{}; bool hasTimingProbe=false; { std::lock_guard lock(deviceState.Mutex); timingProbe=deviceState.TimingProbe; hasTimingProbe=deviceState.HasTimingProbe; } (void)manager; if (hasTimingProbe) drawTimingProbe(timingProbe); else ImGui::TextDisabled("%s",i18n::tr("matrix.waiting"));
    }
}
