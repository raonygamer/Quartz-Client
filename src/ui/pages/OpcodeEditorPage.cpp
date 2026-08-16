#include "quartz/client/ui/pages/OpcodeEditorPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/I18n.hpp"
#include "quartz/client/native/OpcodePatternEditor.hpp"

namespace quartz::client::ui
{
    namespace
    {
        const char* opcodeText(const char* english, const char* portuguese) { return i18n::language()==i18n::Language::PortugueseBrazil?portuguese:english; }
    }

    void OpcodeEditorPage::render(PageContext& context, PageManager& manager)
    {
        auto& state = opcodePatternEditorState();
        if (!state.Initialized) { ImGui::TextDisabled("%s",opcodeText("Open an opcode-pattern native binding from Native / Memory first.","Abra primeiro um binding nativo de padrão de opcode em Nativo / Memória.")); if (ImGui::Button(opcodeText("Back","Voltar"))) manager.closeStandalone(); return; }
        if (ImGui::Button(opcodeText("Back","Voltar"))) manager.closeStandalone(); ImGui::SameLine(); if (ImGui::Button(opcodeText("Lint","Verificar"))) lintOpcodePattern(state); ImGui::SameLine(); if (ImGui::Button(opcodeText("Apply to binding","Aplicar ao binding"))) applyOpcodePattern(state, context.runtimeBindings); ImGui::SameLine(); ImGui::TextDisabled("%s", state.Status.c_str());
        ImGui::TextWrapped("%s",opcodeText("Intel-syntax opcode pattern editor. Mnemonics/registers are highlighted; * matches arbitrary text and ? matches one character in Quartz's decoded-instruction matcher. Linting checks mnemonics and operand bracket structure without pretending wildcard patterns are assemblable source code.","Editor de padrões de opcode em sintaxe Intel. Mnemônicos/registradores são destacados; * corresponde a texto arbitrário e ? a um caractere no matcher de instruções decodificadas do Quartz. A verificação valida mnemônicos e a estrutura dos colchetes dos operandos sem fingir que padrões com curingas são código montável."));
        state.Editor.Render("OpcodePatternEditor", ImVec2(0.0f, -90.0f), ImGuiChildFlags_Borders);
        if (!state.Diagnostics.empty()) { ImGui::SeparatorText(opcodeText("Diagnostics","Diagnósticos")); for (const auto& diagnostic : state.Diagnostics) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", diagnostic.c_str()); }
    }
}
