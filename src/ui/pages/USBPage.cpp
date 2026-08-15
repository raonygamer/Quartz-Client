#include "quartz/client/ui/pages/USBPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void USBPage::render(PageContext& context, PageManager& manager)
    {
        auto& usb = context.usb;
        auto& runtimeBindings = context.runtimeBindings;

        (void)manager;
        drawUSBProfilerPage(usb, runtimeBindings);
    }
}
