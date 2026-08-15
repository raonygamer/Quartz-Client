#include "quartz/client/ui/pages/ProfilesPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void ProfilesPage::render(PageContext& context, PageManager& manager)
    {
        (void)manager;
        ImGui::TextWrapped("Profiles can still group legacy bindings/controls, and now explicitly select which first-class JavaScript scripts are active. JavaScript script membership is treated as the profile's exact active script set.");
        drawRuntimeProfiles(context.runtimeBindings, context.javascript);
    }
}
