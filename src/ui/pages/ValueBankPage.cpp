#include "quartz/client/ui/pages/ValueBankPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void ValueBankPage::render(PageContext& context, PageManager& manager)
    {
        (void)manager;
        ImGui::TextColored(ImVec4(0.95f, 0.67f, 0.28f, 1.0f), "Deprecated runtime graph feature");
        ImGui::TextWrapped("Value Bank is retained for existing binding/control graphs. New automation should keep transient state in q.state and persistent JSON-serializable state in q.storage.");
        drawRuntimeValueBank(context.runtimeBindings);
    }
}
