#include "quartz/client/ui/pages/ControlsPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include <set>

namespace quartz::client::ui
{
    namespace
    {
        constexpr std::string_view NoOperationSentinel = "__quartz_no_operation";
        struct GroupPath { std::string Outer; std::string Inner; };

        GroupPath splitGroup(const char* value)
        {
            const std::string group = value ? trim(value) : std::string{};
            const std::size_t slash = group.find('/');
            if (slash == std::string::npos) return {group, {}};
            return {trim(group.substr(0, slash)), trim(group.substr(slash + 1))};
        }

        bool isNoOperation(const RuntimeControlRule& control) noexcept
        {
            return control.Target == RuntimeControlTarget::BindingClearError && control.TargetBindingId == 0 && std::string_view(control.TargetId) == NoOperationSentinel;
        }

        void setNoOperation(RuntimeControlRule& control) noexcept
        {
            control.Target = RuntimeControlTarget::BindingClearError;
            control.TargetBindingId = 0;
            control.TargetBankValueId = 0;
            control.TargetControlId = 0;
            control.TargetUseSourceValue = false;
            std::snprintf(control.TargetId, sizeof(control.TargetId), "%s", NoOperationSentinel.data());
        }

        void clearNoOperationSentinel(RuntimeControlRule& control) noexcept
        {
            if (std::string_view(control.TargetId) == NoOperationSentinel) control.TargetId[0] = '\0';
        }

        void drawStateSquare(const char* id, const ImVec4 color)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, color);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
            const float size = ImGui::GetFrameHeight();
            ImGui::Button(id, ImVec2(size, size));
            ImGui::PopStyleColor(3);
        }

        bool drawControlTargetSelector(RuntimeControlRule& control)
        {
            static constexpr const char* Names[] = {"Active shader", "Binding enabled", "Global brightness", "Send framebuffer", "Base color mode", "Material parameter", "Unbound binding value", "Value bank", "Control enabled", "Refresh binding", "Force binding update", "Invalidate binding", "Reset binding state", "Retry register capture", "Rescan binding pattern", "Rebind process", "Clear binding error", "No operation"};
            const bool noop = isNoOperation(control);
            const int current = noop ? 17 : std::clamp(static_cast<int>(control.Target), 0, 16);
            bool changed = false;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::BeginCombo("Target", Names[current]))
            {
                for (int i = 0; i < static_cast<int>(std::size(Names)); ++i)
                {
                    const bool selected = i == current;
                    if (ImGui::Selectable(Names[i], selected))
                    {
                        if (i == 17) setNoOperation(control);
                        else { control.Target = static_cast<RuntimeControlTarget>(i); clearNoOperationSentinel(control); }
                        changed = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            return changed;
        }

        bool drawControlRule(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer, RuntimeControlRule& control, bool& erase)
        {
            bool changed = false;
            ImGui::PushID("RuntimeControl");
            ImGui::PushID(static_cast<int>(control.Id & 0x7fffffffULL));
            const RuntimeBinding* source = engine.findBinding(control.SourceBindingId);
            const bool ready = source && source->Enabled && source->RuntimeEnabled && source->HasValue;
            const bool runtimeEnabled = control.Enabled && control.RuntimeEnabled;
            const bool on = control.ConditionActive || control.TriggeredThisFrame;
            const ImVec4 stateColor = runtimeStateColor(runtimeEnabled, on, false, runtimeEnabled && !ready);
            drawStateSquare("##controlState", stateColor);
            ImGui::SameLine();
            std::string header = std::string(control.Name[0] ? control.Name : "Control") + "  [p" + std::to_string(control.Priority) + "]  ";
            if (!control.Enabled) header += "DISABLED";
            else if (!control.RuntimeEnabled) header += "RUNTIME OFF";
            else if (!ready) header += "WAITING";
            else if (control.TriggeredThisFrame) header += "TRIGGERED";
            else header += control.ConditionActive ? "TRUE" : "FALSE";
            header += "###RuntimeControl" + std::to_string(control.Id);
            if (!ImGui::CollapsingHeader(header.c_str())) { ImGui::PopID(); ImGui::PopID(); return false; }
            ImGui::Indent(10.0f);
            ImGui::TextColored(stateColor, "%s", !control.Enabled ? "Disabled" : !control.RuntimeEnabled ? "Runtime disabled by control" : !ready ? "Waiting for source" : control.TriggeredThisFrame ? "Triggered this frame" : control.ConditionActive ? "Condition true" : "Condition false");
            if (source) { ImGui::SameLine(); ImGui::TextDisabled("source %.6g%s", source->Value, source->HasString ? " + string" : ""); }

            changed |= ImGui::Checkbox("Enabled", &control.Enabled);
            ImGui::SameLine(); if (ImGui::SmallButton("Remove")) erase = true;
            ImGui::SameLine(); ImGui::SetNextItemWidth(220.0f); changed |= ImGui::InputText("Name", control.Name, sizeof(control.Name));
            ImGui::SameLine(); ImGui::SetNextItemWidth(110.0f); changed |= ImGui::InputInt("Priority", &control.Priority);
            ImGui::SetNextItemWidth(160.0f); changed |= ImGui::InputText("Group", control.Group, sizeof(control.Group)); ImGui::SameLine(); ImGui::SetNextItemWidth(80.0f); changed |= ImGui::InputInt("Order", &control.Order); ImGui::SameLine(); if (ImGui::SmallButton("Up##controlOrder")) { --control.Order; changed = true; } ImGui::SameLine(); if (ImGui::SmallButton("Down##controlOrder")) { ++control.Order; changed = true; }

            ImGui::SeparatorText("Input & condition");
            ImGui::SetNextItemWidth(300.0f);
            if (ImGui::BeginCombo("Input binding", source ? source->Name : "<select binding>"))
            {
                for (const auto& candidate : engine.bindings())
                {
                    const bool selected = candidate.Id == control.SourceBindingId;
                    if (ImGui::Selectable(candidate.Name, selected)) { control.SourceBindingId = candidate.Id; changed = true; }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            int condition = static_cast<int>(control.Condition);
            ImGui::SetNextItemWidth(175.0f);
            if (ImGui::Combo("Condition", &condition, "==\0!=\0<\0<=\0>\0>=\0between\0outside\0rising edge\0falling edge\0on change\0changed to\0changed from\0becomes true\0becomes false\0string ==\0string !=\0string contains\0")) { control.Condition = static_cast<RuntimeControlCondition>(condition); changed = true; }
            const bool stringCondition = control.Condition == RuntimeControlCondition::StringEqual || control.Condition == RuntimeControlCondition::StringNotEqual || control.Condition == RuntimeControlCondition::StringContains;
            const bool eventCondition = control.Condition == RuntimeControlCondition::RisingEdge || control.Condition == RuntimeControlCondition::FallingEdge || control.Condition == RuntimeControlCondition::OnChange || control.Condition == RuntimeControlCondition::ChangedTo || control.Condition == RuntimeControlCondition::ChangedFrom || control.Condition == RuntimeControlCondition::BecomesTrue || control.Condition == RuntimeControlCondition::BecomesFalse;
            const bool needsValue = control.Condition <= RuntimeControlCondition::FallingEdge || control.Condition == RuntimeControlCondition::ChangedTo || control.Condition == RuntimeControlCondition::ChangedFrom;
            if (stringCondition) changed |= ImGui::InputText("Text target", control.StringCompare, sizeof(control.StringCompare));
            else if (needsValue)
            {
                const bool twoValues = control.Condition == RuntimeControlCondition::Between || control.Condition == RuntimeControlCondition::Outside;
                ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat(twoValues ? "Minimum" : "Target / threshold", &control.ValueA, 0.01f);
                if (twoValues) { ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Maximum", &control.ValueB, 0.01f); }
                if (control.Condition == RuntimeControlCondition::Equal || control.Condition == RuntimeControlCondition::NotEqual || control.Condition == RuntimeControlCondition::ChangedTo || control.Condition == RuntimeControlCondition::ChangedFrom) { ImGui::SameLine(); ImGui::SetNextItemWidth(135.0f); changed |= ImGui::DragFloat("Tolerance", &control.Tolerance, 0.0001f, 0.000001f, 1000.0f, "%.6f"); }
            }
            if (eventCondition) { changed |= ImGui::Checkbox("Fire on first matching sample", &control.FireOnFirstSample); ImGui::SameLine(); ImGui::TextDisabled("events are one-shot; iterative passes are capped"); }
            else if (!stringCondition) { ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Hysteresis", &control.Hysteresis, 0.001f, 0.0f, 1000.0f, "%.4f"); }

            ImGui::SeparatorText("Target & actions");
            if (ImGui::BeginTabBar("ControlOptions", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyResizeDown))
            {
                if (ImGui::BeginTabItem("Target"))
                {
                    ImGui::TextDisabled("Compatibility action for existing configs. Extra actions below can run alongside it.");
                    changed |= drawControlTargetSelector(control);
                    const bool noop = isNoOperation(control);
                    if (noop)
                    {
                        ImGui::TextDisabled("No primary action. The condition still evaluates normally and extra actions can still fire.");
                    }
                    else if (control.Target == RuntimeControlTarget::ActiveShader)
                    {
                        const ShaderPreset* shaderById = control.ShaderId[0] ? findShaderPresetById(control.ShaderId) : nullptr;
                        const char* preview = control.ShaderPresetIndex == -1 ? "<previous shader>" : shaderById ? shaderById->Name.c_str() : control.ShaderPresetIndex > 0 && control.ShaderPresetIndex <= static_cast<int>(ShaderPresets.size()) ? ShaderPresets[static_cast<std::size_t>(control.ShaderPresetIndex - 1)].Name.c_str() : "<select shader>";
                        ImGui::SetNextItemWidth(300.0f);
                        if (ImGui::BeginCombo("Shader", preview))
                        {
                            if (ImGui::Selectable("<previous shader>", control.ShaderPresetIndex == -1)) { control.ShaderPresetIndex = -1; control.ShaderId[0] = '\0'; changed = true; }
                            for (std::size_t i = 0; i < ShaderPresets.size(); ++i)
                            {
                                const bool selected = control.ShaderPresetIndex == static_cast<int>(i + 1);
                                if (ImGui::Selectable(ShaderPresets[i].Name.c_str(), selected)) { control.ShaderPresetIndex = static_cast<int>(i + 1); std::snprintf(control.ShaderId, sizeof(control.ShaderId), "%s", ShaderPresets[i].Id.c_str()); changed = true; }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::SetNextItemWidth(160.0f); changed |= ImGui::DragFloat("Crossfade", &control.TransitionSeconds, 0.02f, 0.0f, 10.0f, "%.2f s");
                    }
                    else if (control.Target == RuntimeControlTarget::BindingEnabled) { changed |= drawRuntimeBindingReferenceCombo(engine, "Target binding", control.TargetBindingId, control.SourceBindingId); changed |= ImGui::Checkbox("Runtime enabled", &control.TargetBool); }
                    else if (control.Target == RuntimeControlTarget::GlobalBrightness) changed |= ImGui::SliderFloat("Brightness", &control.TargetValue, 0.0f, 1.0f, "%.3f");
                    else if (control.Target == RuntimeControlTarget::SendFramebuffer) changed |= ImGui::Checkbox("Send framebuffer", &control.TargetBool);
                    else if (control.Target == RuntimeControlTarget::BaseColorMode) { int mode = std::clamp(static_cast<int>(std::lround(control.TargetValue)), 0, 2); if (ImGui::Combo("Base color mode", &mode, "RGB wave\0Solid\0Shader\0")) { control.TargetValue = static_cast<float>(mode); changed = true; } }
                    else if (control.Target == RuntimeControlTarget::MaterialParameter) { changed |= drawRuntimeControlMaterialTarget(control, shaderFramebuffer); ImGui::SetNextItemWidth(180.0f); changed |= ImGui::DragFloat("Target value", &control.TargetValue, 0.01f); }
                    else if (control.Target == RuntimeControlTarget::BindingValue) { changed |= drawRuntimeBindingReferenceCombo(engine, "Unbound target", control.TargetBindingId, control.SourceBindingId, true); changed |= ImGui::Checkbox("Use source value", &control.TargetUseSourceValue); if (!control.TargetUseSourceValue) { ImGui::SetNextItemWidth(180.0f); changed |= ImGui::DragFloat("Written value", &control.TargetValue, 0.01f); } }
                    else if (control.Target == RuntimeControlTarget::ValueBank) { changed |= drawRuntimeBankReferenceCombo(engine, "Bank target", control.TargetBankValueId); changed |= ImGui::Checkbox("Use source value", &control.TargetUseSourceValue); if (!control.TargetUseSourceValue) { ImGui::SetNextItemWidth(180.0f); changed |= ImGui::DragFloat("Written value", &control.TargetValue, 0.01f); } }
                    else if (control.Target == RuntimeControlTarget::ControlEnabled) { changed |= drawRuntimeControlReferenceCombo(engine, "Target control", control.TargetControlId); changed |= ImGui::Checkbox("Runtime enabled", &control.TargetBool); }
                    else if (runtimeControlTargetIsBindingOperation(control.Target)) { changed |= drawRuntimeBindingReferenceCombo(engine, "Target binding", control.TargetBindingId, control.SourceBindingId); ImGui::TextDisabled("Primary binding operations fire when the condition enters true (or an event fires). Use an additional action with While active for repeated operations."); }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Extra actions"))
                {
                    ImGui::TextDisabled("Actions execute in priority order. On-trigger actions are ideal for saving/restoring shader state through the value bank.");
                    changed |= drawRuntimeActionList(engine, shaderFramebuffer, control.Actions, control.SourceBindingId, true);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }

            ImGui::SeparatorText("Runtime state");
            ImGui::TextDisabled("Triggers: %llu   %s", static_cast<unsigned long long>(control.TriggerCount), control.LastTriggerTime > 0.0 ? "has fired" : "never fired");
            if (changed) engine.markChanged();
            ImGui::Unindent(10.0f);
            ImGui::Dummy(ImVec2(0.0f, 5.0f));
            ImGui::Separator();
            ImGui::PopID(); ImGui::PopID();
            return changed;
        }

        void drawControlCards(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer)
        {
            std::vector<RuntimeControlRule*> order; order.reserve(engine.controls().size()); for (auto& control : engine.controls()) order.push_back(&control);
            std::ranges::stable_sort(order, [](const RuntimeControlRule* a, const RuntimeControlRule* b) { if (a->Order != b->Order) return a->Order < b->Order; if (a->Priority != b->Priority) return a->Priority < b->Priority; return a->Id < b->Id; });
            std::optional<std::size_t> erase;
            auto drawOne = [&](RuntimeControlRule& control)
            {
                bool shouldErase = false;
                ImGui::PushID(static_cast<int>(control.Id & 0x7fffffffULL));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
                if (ImGui::BeginChild("##ControlCard", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) drawControlRule(engine, shaderFramebuffer, control, shouldErase);
                ImGui::EndChild(); ImGui::PopStyleVar(2); ImGui::PopID(); ImGui::Dummy(ImVec2(0.0f, 8.0f));
                if (shouldErase) erase = static_cast<std::size_t>(&control - engine.controls().data());
            };

            for (auto* control : order) if (splitGroup(control->Group).Outer.empty()) drawOne(*control);
            std::set<std::string> outerGroups;
            for (const auto* control : order) { const auto path = splitGroup(control->Group); if (!path.Outer.empty()) outerGroups.insert(path.Outer); }
            for (const auto& outer : outerGroups)
            {
                if (!ImGui::CollapsingHeader((outer + "###ControlGroup/" + outer).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
                ImGui::Indent(8.0f);
                for (auto* control : order) { const auto path = splitGroup(control->Group); if (path.Outer == outer && path.Inner.empty()) drawOne(*control); }
                std::set<std::string> innerGroups;
                for (const auto* control : order) { const auto path = splitGroup(control->Group); if (path.Outer == outer && !path.Inner.empty()) innerGroups.insert(path.Inner); }
                for (const auto& inner : innerGroups)
                {
                    if (!ImGui::CollapsingHeader((inner + "###ControlSubgroup/" + outer + "/" + inner).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
                    ImGui::Indent(8.0f);
                    for (auto* control : order) { const auto path = splitGroup(control->Group); if (path.Outer == outer && path.Inner == inner) drawOne(*control); }
                    ImGui::Unindent(8.0f);
                }
                ImGui::Unindent(8.0f);
            }
            if (erase) engine.eraseControl(*erase);
        }
    }

    void ControlsPage::render(PageContext& context, PageManager& manager)
    {
        auto& engine = context.runtimeBindings;
        (void)manager;
        ImGui::TextColored(ImVec4(0.95f, 0.67f, 0.28f, 1.0f), "Deprecated runtime graph feature");
        ImGui::TextWrapped("Controls remain for existing graphs. New state machines should use q.state/q.storage, q.events and q.runtime directly from JavaScript.");
        int passes = engine.controlPassLimit(); ImGui::SetNextItemWidth(110.0f); if (ImGui::InputInt("Control passes", &passes)) engine.setControlPassLimit(passes); ImGui::SameLine(); ImGui::TextDisabled("bounded iterative passes/frame");
        if (ImGui::Button("+ Control")) engine.addControl();
        ImGui::SameLine();
        if (ImGui::SmallButton("Threshold preset")) { auto& c = engine.addControl(); std::snprintf(c.Name, sizeof(c.Name), "%s", "Threshold"); c.Condition = RuntimeControlCondition::Greater; c.ValueA = 0.5f; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Rising edge preset")) { auto& c = engine.addControl(); std::snprintf(c.Name, sizeof(c.Name), "%s", "Rising edge"); c.Condition = RuntimeControlCondition::RisingEdge; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Changed preset")) { auto& c = engine.addControl(); std::snprintf(c.Name, sizeof(c.Name), "%s", "On change"); c.Condition = RuntimeControlCondition::OnChange; }
        ImGui::TextDisabled("Grouping supports Group/Subgroup. A plain Group still works exactly as before.");
        drawControlCards(engine, context.shaderFramebuffer);
        if (engine.controls().empty()) ImGui::TextDisabled("No controls yet. Create one, choose an input binding, then choose its condition and target.");
    }
}
