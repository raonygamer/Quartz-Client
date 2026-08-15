#include "quartz/client/ui/pages/BindingsPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void BindingsPage::render(PageContext& context, PageManager& manager)
    {
        auto& engine = context.runtimeBindings;
        auto& shaderFramebuffer = context.shaderFramebuffer;
        (void)manager;

        ImGui::TextWrapped("Bindings are the data nodes of the runtime graph. Pick a source, optionally compare/aggregate/transform it, then route the result to materials, actions, controls or the value bank.");
        if (ImGui::Button("+ Binding")) engine.add();
        ImGui::SameLine(); if (ImGui::Button("Save graph")) engine.save();
        ImGui::SameLine(); ImGui::TextDisabled("%s", engine.path().string().c_str());
        if (ImGui::CollapsingHeader("Quick create", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto addPreset = [&](const char* name, const RuntimeSourceKind source, const int signal, const char* target)
            {
                auto& binding = engine.add(); std::snprintf(binding.Name, sizeof(binding.Name), "%s", name); binding.Source = source; binding.Signal = signal; std::snprintf(binding.TargetId, sizeof(binding.TargetId), "%s", target); binding.Clamp = true; binding.OutputMin = 0.0f; binding.OutputMax = 1.0f; binding.SmoothingHz = 8.0f; binding.UpdateHz = source == RuntimeSourceKind::NativeProcess ? 20.0f : 60.0f; return &binding;
            };
            if (ImGui::SmallButton("Native value")) addPreset("Native process value", RuntimeSourceKind::NativeProcess, 0, "runtime.native"); ImGui::SameLine();
            if (ImGui::SmallButton("Native address")) { auto* b = addPreset("Native object address", RuntimeSourceKind::NativeAddress, 1, "runtime.address"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; } ImGui::SameLine();
            if (ImGui::SmallButton("Comparator")) { auto* b = addPreset("Comparator", RuntimeSourceKind::MassCompare, 0, "runtime.compare"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; } ImGui::SameLine();
            if (ImGui::SmallButton("Aggregate")) { auto* b = addPreset("Aggregate", RuntimeSourceKind::Aggregate, 0, "runtime.aggregate"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; } ImGui::SameLine();
            if (ImGui::SmallButton("Passthrough")) { auto* b = addPreset("Binding passthrough", RuntimeSourceKind::BindingValue, 0, "runtime.value"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; } ImGui::SameLine();
            if (ImGui::SmallButton("Writable")) { auto* b = addPreset("Writable state", RuntimeSourceKind::Unbound, 0, "runtime.state"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; }
            if (ImGui::SmallButton("Audio RMS")) { auto* b = addPreset("Audio RMS", RuntimeSourceKind::Audio, 0, "runtime.audio"); b->WriteMaterial = false; } ImGui::SameLine();
            if (ImGui::SmallButton("Current shader")) { auto* b = addPreset("Current shader", RuntimeSourceKind::ShaderState, 0, "runtime.shader"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; } ImGui::SameLine();
            if (ImGui::SmallButton("Active profile")) { auto* b = addPreset("Active profile", RuntimeSourceKind::ProfileState, 0, "runtime.profile"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; }
        }
        if (shaderFramebuffer.materialParameters().empty()) ImGui::TextDisabled("Current shader has no reflected material parameters; value-only bindings still work normally.");
        drawGroupedBindings(engine, shaderFramebuffer);
        if (engine.bindings().empty()) ImGui::TextDisabled("No bindings yet.");
    }
}
