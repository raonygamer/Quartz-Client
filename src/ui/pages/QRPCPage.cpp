#include "quartz/client/ui/pages/QRPCPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void QRPCPage::render(PageContext& context, PageManager& manager)
    {
        auto& runtimeTelemetry = context.runtimeTelemetry;

        (void)manager;
        drawQRPCInspectorPage(runtimeTelemetry);
    }
}
