#include "quartz/client/ui/pages/ProfilesPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void ProfilesPage::render(PageContext& context, PageManager& manager)
    {
        (void)manager;
        ImGui::TextWrapped("Profiles are runtime graph presets: enable/disable coherent sets of bindings and controls without hunting through individual nodes.");
        drawRuntimeProfiles(context.runtimeBindings);
    }
}
