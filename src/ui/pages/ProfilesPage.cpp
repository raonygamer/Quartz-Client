#include "quartz/client/ui/pages/ProfilesPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/I18n.hpp"

namespace quartz::client::ui
{
    void ProfilesPage::render(PageContext& context, PageManager& manager)
    {
        (void)manager;
        ImGui::TextWrapped("%s", i18n::language()==i18n::Language::PortugueseBrazil ? "Perfis ainda podem agrupar bindings/controles legados e agora selecionam explicitamente quais scripts JavaScript de primeira classe estão ativos. A associação de scripts JavaScript é tratada como o conjunto ativo exato do perfil." : "Profiles can still group legacy bindings/controls, and now explicitly select which first-class JavaScript scripts are active. JavaScript script membership is treated as the profile's exact active script set.");
        drawRuntimeProfiles(context.runtimeBindings, context.javascript);
    }
}
