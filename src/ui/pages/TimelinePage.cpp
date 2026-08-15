#include "quartz/client/ui/pages/TimelinePage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void TimelinePage::render(PageContext& context, PageManager& manager)
    {
        auto& runtimeTelemetry = context.runtimeTelemetry;

        (void)manager;
        drawTimelinePage(runtimeTelemetry);
    }
}
