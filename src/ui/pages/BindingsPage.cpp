#include "quartz/client/ui/pages/BindingsPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void BindingsPage::render(PageContext& context, PageManager& manager)
    {
        auto& shaderFramebuffer = context.shaderFramebuffer;
        auto& runtimeBindings = context.runtimeBindings;

        (void)manager;
        drawRuntimeBindingsPage(runtimeBindings, shaderFramebuffer);
    }
}
