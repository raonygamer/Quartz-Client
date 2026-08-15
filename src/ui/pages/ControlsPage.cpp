#include "quartz/client/ui/pages/ControlsPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
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
        drawGroupedControls(engine, context.shaderFramebuffer);
        if (engine.controls().empty()) ImGui::TextDisabled("No controls yet. Create one, choose an input binding, then choose its condition and target.");
    }
}
