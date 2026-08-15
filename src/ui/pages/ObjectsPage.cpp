#include "quartz/client/ui/pages/ObjectsPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void ObjectsPage::render(PageContext& context, PageManager& manager)
    {
        (void)manager;
        ImGui::TextWrapped("Object descriptors define native layouts; pointer instances bind those layouts to exact runtime addresses. They stay together because they are two halves of the same object workflow.");
        if (ImGui::BeginTabBar("ObjectWorkspace"))
        {
            if (ImGui::BeginTabItem("Models")) { drawRuntimeObjectDescriptors(context.runtimeBindings); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Pointer instances")) { drawRuntimePointers(context.runtimeBindings); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
}
