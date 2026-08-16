#include "quartz/client/ui/pages/NativePage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/MemoryInspector.hpp"
#include "quartz/client/ui/ReverseEngineeringTools.hpp"
#include "quartz/client/ui/ReverseEngineeringNavigation.hpp"
#include "quartz/client/ui/SignatureMaker.hpp"
#include "quartz/client/native/OpcodePatternEditor.hpp"

namespace quartz::client::ui
{
    void NativePage::render(PageContext& context, PageManager& manager)
    {
        auto& inspector = runtimeMemoryInspectorState();
        const bool focusInspector = consumeMemoryInspectorFocus();
        const bool focusSignature = signatureMakerWantsFocus();
        ImGui::TextWrapped("Native reverse-engineering workspace. Memory, disassembly, watches and signatures share addresses directly so you can move through a process without copy/pasting hexadecimal values between unrelated tools.");
        if (ImGui::BeginTabBar("ReverseEngineeringWorkspace"))
        {
            if (ImGui::BeginTabItem("Memory / disassembly", nullptr, focusInspector ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                if (ImGui::Button("Memory Scanner...")) manager.open("memory-scanner");
                ImGui::SameLine(); if (ImGui::Button("Memory Watch...")) manager.open("memory-watch");
                if (inspector.Pid > 0 && inspector.Address != 0)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Signature from address")) { requestSignatureMaker(inspector.Pid, inspector.Address); }
                }
                ImGui::SeparatorText("Manual watch");
                drawManualMemoryWatch(context, manager);
                drawEnhancedRuntimeMemoryInspector(inspector);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Signatures", nullptr, focusSignature ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                if (ImGui::BeginTabBar("SignatureWorkspace"))
                {
                    if (ImGui::BeginTabItem("Signature maker", nullptr, focusSignature ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) { drawSignatureMaker(context, manager); ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Search")) { drawQuickSignatureSearch(context, manager); ImGui::EndTabItem(); }
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
}
