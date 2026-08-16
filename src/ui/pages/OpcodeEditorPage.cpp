#include "quartz/client/ui/pages/OpcodeEditorPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/I18n.hpp"
#include "quartz/client/native/OpcodePatternEditor.hpp"

namespace quartz::client::ui
{
    void OpcodeEditorPage::render(PageContext& context, PageManager& manager)
    {
        auto& state = opcodePatternEditorState();
        if (!state.Initialized) { ImGui::TextDisabled("%s",i18n::tr("opcode.openFromMemory")); if (ImGui::Button(i18n::tr("opcode.back"))) manager.closeStandalone(); return; }
        if (ImGui::Button(i18n::tr("opcode.back"))) manager.closeStandalone(); ImGui::SameLine(); if (ImGui::Button(i18n::tr("opcode.lint"))) lintOpcodePattern(state); ImGui::SameLine(); if (ImGui::Button(i18n::tr("opcode.applyBinding"))) applyOpcodePattern(state, context.runtimeBindings); ImGui::SameLine(); ImGui::TextDisabled("%s", state.Status.c_str());
        ImGui::TextWrapped("%s",i18n::tr("opcode.description"));
        state.Editor.Render("OpcodePatternEditor", ImVec2(0.0f, -90.0f), ImGuiChildFlags_Borders);
        if (!state.Diagnostics.empty()) { ImGui::SeparatorText(i18n::tr("opcode.diagnostics")); for (const auto& diagnostic : state.Diagnostics) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", diagnostic.c_str()); }
    }
}
