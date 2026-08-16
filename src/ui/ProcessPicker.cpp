#include "quartz/client/ui/ProcessPicker.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/ui/I18n.hpp"
#include <algorithm>
#include <imgui.h>

namespace quartz::client::ui
{
    bool drawProcessPicker(const char* id, std::vector<RuntimeProcessInfo>& processes, pid_t& pid, char* search, const std::size_t searchSize, const float comboWidth)
    {
        if (processes.empty()) processes = enumerateRuntimeProcesses();
        bool changed = false;
        const auto selectedProcess = [&]() -> const RuntimeProcessInfo* { const auto it = std::ranges::find(processes, pid, &RuntimeProcessInfo::Pid); return it == processes.end() ? nullptr : &*it; };
        const RuntimeProcessInfo* selected = selectedProcess();
        ImGui::PushID(id);
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
        if (ImGui::Button(i18n::tr("common.refreshProcesses"), ImVec2(0.0f, ImGui::GetFrameHeight()))) { processes = enumerateRuntimeProcesses(); selected = selectedProcess(); }
        ImGui::PopStyleVar();
        ImGui::SameLine(); ImGui::SetNextItemWidth(comboWidth);
        if (ImGui::BeginCombo(i18n::tr("common.process"), selected ? runtimeProcessDisplayTitle(*selected).c_str() : i18n::tr("common.selectProcess"), ImGuiComboFlags_HeightLargest))
        {
            const bool focusSearch = ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false);
            if (focusSearch) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-1.0f); ImGui::InputTextWithHint("##ProcessSearch", i18n::tr("common.processSearchHint"), search, searchSize);
            ImGui::Separator();
            const std::string lowered = runtimeLower(search);
            std::size_t matches = 0;
            for (const auto& process : processes)
            {
                if (!runtimeProcessMatchesSearch(process, lowered)) continue;
                ++matches; const bool active = process.Pid == pid; const std::string label = runtimeProcessDisplayTitle(process) + "  [" + std::to_string(process.Pid) + "]";
                if (ImGui::Selectable(label.c_str(), active)) { pid = process.Pid; changed = true; }
                if (active) ImGui::SetItemDefaultFocus();
            }
            if (matches == 0) ImGui::TextDisabled("%s", i18n::tr("common.noProcessMatches"));
            ImGui::EndCombo();
        }
        ImGui::PopID(); return changed;
    }
}
