#include "quartz/client/ui/pages/ValueBankPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void ValueBankPage::render(PageContext& context, PageManager& manager)
    {
        (void)manager;
        ImGui::TextWrapped("Persistent runtime values used by bindings, controls and actions. Keep state here instead of inventing helper bindings just to remember a number/string/address.");
        drawRuntimeValueBank(context.runtimeBindings);
    }
}
