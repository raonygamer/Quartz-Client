#include "quartz/client/ui/pages/ControlsPage.hpp"
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
                if (ImGui::BeginChild("##ControlCard", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) drawRuntimeControlRule(engine, shaderFramebuffer, control, shouldErase);
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
        ImGui::TextWrapped("Controls turn binding values into conditions and actions. Configure the input/condition first, then the target and any extra actions.");
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
