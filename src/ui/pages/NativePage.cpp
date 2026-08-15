#include "quartz/client/ui/pages/NativePage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/native/OpcodePatternEditor.hpp"

namespace quartz::client::ui
{
    void NativePage::render(PageContext& context, PageManager& manager)
    {
        auto& inspector = runtimeMemoryInspectorState(); auto& engine = context.runtimeBindings;
        ImGui::TextWrapped("Native-process workspace: active native bindings, signature telemetry and the shared memory/disassembly inspector. Generic value scans and hardware data breakpoints have dedicated pages now.");
        if (ImGui::Button("Memory Scanner...")) manager.open("memory-scanner"); ImGui::SameLine(); if (ImGui::Button("Memory Watch...")) manager.open("memory-watch");
        ImGui::SeparatorText("Native bindings");
        bool any = false;
        for (auto& binding : engine.bindings())
        {
            if (binding.Source != RuntimeSourceKind::NativeProcess && binding.Source != RuntimeSourceKind::NativeAddress) continue; any = true; ImGui::PushID(static_cast<int>(binding.Id & 0x7fffffffULL));
            ImGui::Text("%s", binding.Name); ImGui::SameLine(); ImGui::TextDisabled("PID %d", binding.ProcessId);
            if (binding.AddressMode == ProcessAddressMode::Signature)
            {
                ImGui::SameLine(); if (binding.SignatureScanRunning) ImGui::TextDisabled("scanning"); else if (binding.SignatureResolvedAddress) ImGui::TextDisabled("resolved 0x%llX", static_cast<unsigned long long>(binding.SignatureResolvedAddress)); else if (!binding.SignatureStatus.empty()) ImGui::TextDisabled("%s", binding.SignatureStatus.c_str());
                if (binding.SignatureScanAverageMiBs > 0.0) { if (binding.SignatureScanAverageMiBs >= 1024.0) ImGui::TextDisabled("Average scan speed: %.2f GiB/s", binding.SignatureScanAverageMiBs / 1024.0); else ImGui::TextDisabled("Average scan speed: %.1f MiB/s", binding.SignatureScanAverageMiBs); if (binding.SignatureScanLastSeconds > 0.0) { ImGui::SameLine(); ImGui::TextDisabled("last %.1f MiB / %.3f s", binding.SignatureScanLastBytes / (1024.0 * 1024.0), binding.SignatureScanLastSeconds); } }
                if (ImGui::SmallButton("Rescan")) { resetRuntimeSignatureScan(binding); binding.SignatureConfigHash = 0; binding.NextUpdate = 0.0; }
                if (binding.SignaturePatternKind == RuntimeSignaturePatternKind::OpcodePattern) { ImGui::SameLine(); if (ImGui::SmallButton("Opcode editor...")) { openOpcodePatternEditor(binding); manager.open("opcode-editor"); } }
            }
            ImGui::Separator(); ImGui::PopID();
        }
        if (!any) ImGui::TextDisabled("No NativeProcess/NativeAddress bindings configured.");
        drawRuntimeMemoryInspector(inspector);
    }
}
