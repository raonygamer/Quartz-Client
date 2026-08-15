#include "quartz/client/ui/pages/OpcodeEditorPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/native/OpcodePatternEditor.hpp"

namespace quartz::client::ui
{
    void OpcodeEditorPage::render(PageContext& context, PageManager& manager)
    {
        auto& state = opcodePatternEditorState();
        if (!state.Initialized) { ImGui::TextDisabled("Open an opcode-pattern native binding from Native / Memory first."); if (ImGui::Button("Back")) manager.closeStandalone(); return; }
        if (ImGui::Button("Back")) manager.closeStandalone(); ImGui::SameLine(); if (ImGui::Button("Lint")) lintOpcodePattern(state); ImGui::SameLine(); if (ImGui::Button("Apply to binding")) applyOpcodePattern(state, context.runtimeBindings); ImGui::SameLine(); ImGui::TextDisabled("%s", state.Status.c_str());
        ImGui::TextWrapped("Intel-syntax opcode pattern editor. Mnemonics/registers are highlighted; * matches arbitrary text and ? matches one character in Quartz's decoded-instruction matcher. Linting checks mnemonics and operand bracket structure without pretending wildcard patterns are assemblable source code.");
        state.Editor.Render("OpcodePatternEditor", ImVec2(0.0f, -90.0f), ImGuiChildFlags_Borders);
        if (!state.Diagnostics.empty()) { ImGui::SeparatorText("Diagnostics"); for (const auto& diagnostic : state.Diagnostics) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", diagnostic.c_str()); }
    }
}
