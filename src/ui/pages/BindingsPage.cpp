#include "quartz/client/ui/pages/BindingsPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include <set>

namespace quartz::client::ui
{
    namespace
    {
        struct GroupPath { std::string Outer; std::string Inner; };
        GroupPath splitGroup(const char* value)
        {
            const std::string group = value ? trim(value) : std::string{};
            const std::size_t slash = group.find('/');
            if (slash == std::string::npos) return {group, {}};
            return {trim(group.substr(0, slash)), trim(group.substr(slash + 1))};
        }

        ImVec4 bindingStateColor(const RuntimeBinding& binding)
        {
            const bool runtimeEnabled = binding.Enabled && binding.RuntimeEnabled;
            const bool addressSearching = runtimeEnabled && (binding.SignatureScanRunning || (binding.SignatureRegisterCapture && binding.SignatureInstructionAddress != 0 && binding.SignatureResolvedAddress == 0));
            if (addressSearching)
            {
                const float pulse = 0.5f + std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f) * 0.5f;
                constexpr ImVec4 Gray{0.38f, 0.38f, 0.38f, 1.0f};
                constexpr ImVec4 Cyan{0.10f, 0.78f, 0.86f, 1.0f};
                return {Gray.x + (Cyan.x - Gray.x) * pulse, Gray.y + (Cyan.y - Gray.y) * pulse, Gray.z + (Cyan.z - Gray.z) * pulse, 1.0f};
            }
            const bool booleanState = runtimeBindingLooksBoolean(binding);
            const bool waiting = runtimeEnabled && !binding.HasValue && binding.Error.empty();
            const bool good = binding.HasValue && (!booleanState || binding.Value >= 0.5f);
            return runtimeStateColor(runtimeEnabled, good, !binding.Error.empty(), waiting);
        }

        void drawBindingStateOverlay(const ImVec2 legacyOrigin, const ImVec4 color)
        {
            constexpr float LegacySize = 11.0f;
            const float size = ImGui::GetFrameHeight();
            const float growth = std::max(size - LegacySize, 0.0f);
            const ImVec2 min{legacyOrigin.x - growth * 0.5f, legacyOrigin.y - growth * 0.5f};
            const ImVec2 max{min.x + size, min.y + size};
            ImGui::GetWindowDrawList()->AddRectFilled(min, max, ImGui::GetColorU32(color), ImGui::GetStyle().FrameRounding);
        }

        void drawBindingCards(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer)
        {
            std::vector<RuntimeBinding*> order; order.reserve(engine.bindings().size()); for (auto& binding : engine.bindings()) order.push_back(&binding);
            std::ranges::stable_sort(order, [](const RuntimeBinding* a, const RuntimeBinding* b) { if (a->Order != b->Order) return a->Order < b->Order; if (a->Priority != b->Priority) return a->Priority < b->Priority; return a->Id < b->Id; });
            std::optional<std::size_t> erase;
            auto drawOne = [&](RuntimeBinding& binding)
            {
                bool shouldErase = false;
                ImGui::PushID(static_cast<int>(binding.Id & 0x7fffffffULL));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
                if (ImGui::BeginChild("##BindingCard", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY))
                {
                    const ImVec2 stateOrigin = ImGui::GetCursorScreenPos();
                    drawRuntimeBinding(engine, shaderFramebuffer, binding, shouldErase);
                    drawBindingStateOverlay(stateOrigin, bindingStateColor(binding));
                }
                ImGui::EndChild(); ImGui::PopStyleVar(2); ImGui::PopID(); ImGui::Dummy(ImVec2(0.0f, 8.0f));
                if (shouldErase) erase = static_cast<std::size_t>(&binding - engine.bindings().data());
            };

            for (auto* binding : order) if (splitGroup(binding->Group).Outer.empty()) drawOne(*binding);
            std::set<std::string> outerGroups;
            for (const auto* binding : order) { const auto path = splitGroup(binding->Group); if (!path.Outer.empty()) outerGroups.insert(path.Outer); }
            for (const auto& outer : outerGroups)
            {
                if (!ImGui::CollapsingHeader((outer + "###BindingGroup/" + outer).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
                ImGui::Indent(8.0f);
                for (auto* binding : order) { const auto path = splitGroup(binding->Group); if (path.Outer == outer && path.Inner.empty()) drawOne(*binding); }
                std::set<std::string> innerGroups;
                for (const auto* binding : order) { const auto path = splitGroup(binding->Group); if (path.Outer == outer && !path.Inner.empty()) innerGroups.insert(path.Inner); }
                for (const auto& inner : innerGroups)
                {
                    if (!ImGui::CollapsingHeader((inner + "###BindingSubgroup/" + outer + "/" + inner).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
                    ImGui::Indent(8.0f);
                    for (auto* binding : order) { const auto path = splitGroup(binding->Group); if (path.Outer == outer && path.Inner == inner) drawOne(*binding); }
                    ImGui::Unindent(8.0f);
                }
                ImGui::Unindent(8.0f);
            }
            if (erase) engine.erase(*erase);
        }
    }

    void BindingsPage::render(PageContext& context, PageManager& manager)
    {
        auto& engine = context.runtimeBindings;
        auto& shaderFramebuffer = context.shaderFramebuffer;
        (void)manager;

        ImGui::TextWrapped("Bindings are the data nodes of the runtime graph. Pick a source, optionally compare/aggregate/transform it, then route the result to materials, actions, controls or the value bank.");
        if (ImGui::Button("+ Binding")) engine.add();
        ImGui::SameLine(); if (ImGui::Button("Save graph")) engine.save();
        ImGui::SameLine(); ImGui::TextDisabled("%s", engine.path().string().c_str());
        ImGui::TextDisabled("Grouping supports Group/Subgroup. A plain Group still works exactly as before.");
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
        drawBindingCards(engine, shaderFramebuffer);
        if (engine.bindings().empty()) ImGui::TextDisabled("No bindings yet.");
    }
}
