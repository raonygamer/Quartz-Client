#include "quartz/client/ui/pages/RGBPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void RGBPage::render(PageContext& context, PageManager& manager)
    {
        auto& settings = context.settings;
        const auto& framebuffer = context.framebuffer;
        const auto& rgbAnalytics = context.rgbAnalytics;

        (void)manager;
        drawRGBProfilerPage(framebuffer, settings, rgbAnalytics);
    }
}
