#include "quartz/client/ui/pages/NativePage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/I18n.hpp"
#include "quartz/client/ui/MemoryInspector.hpp"
#include "quartz/client/ui/ObjectExperiments.hpp"
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
        ImGui::TextWrapped("%s", i18n::tr("re.workspaceDescription"));
        if (ImGui::BeginTabBar("ReverseEngineeringWorkspace"))
        {
            if (ImGui::BeginTabItem(i18n::tr("re.memoryDisassembly"), nullptr, focusInspector ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                if (ImGui::Button(i18n::tr("re.memoryScanner"))) manager.open("memory-scanner");
                ImGui::SameLine(); if (ImGui::Button(i18n::tr("re.memoryWatch"))) manager.open("memory-watch");
                if (inspector.Pid > 0 && inspector.Address != 0)
                {
                    ImGui::SameLine();
                    if (ImGui::Button(i18n::tr("re.signatureFromAddress"))) requestSignatureMaker(inspector.Pid, inspector.Address);
                }
                drawEnhancedRuntimeMemoryInspector(inspector);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(i18n::tr("re.signatures"), nullptr, focusSignature ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                if (ImGui::BeginTabBar("SignatureWorkspace"))
                {
                    if (ImGui::BeginTabItem(i18n::tr("re.signatureMaker"), nullptr, focusSignature ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) { drawSignatureMaker(context, manager); ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem(i18n::tr("re.signatureSearch"))) { drawQuickSignatureSearch(context, manager); ImGui::EndTabItem(); }
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(i18n::tr("re.objectExperiments"))) { drawObjectExperiments(context, manager); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
}
