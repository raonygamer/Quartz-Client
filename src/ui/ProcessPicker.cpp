#include "quartz/client/ui/ProcessPicker.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/ui/I18n.hpp"
#include <algorithm>
#include <imgui.h>
#include <string>
#include <unordered_map>

namespace quartz::client::ui
{
    namespace
    {
        pid_t SharedProcessPid = 0;
        std::unordered_map<std::string,bool> LinkedSelectors;

        bool& selectorLinked(const char* id)
        {
            const auto [it, inserted] = LinkedSelectors.try_emplace(id ? id : "", true);
            return it->second;
        }
    }

    pid_t sharedReverseEngineeringProcess() noexcept { return SharedProcessPid; }
    void setSharedReverseEngineeringProcess(const pid_t pid) noexcept { if (pid > 0) SharedProcessPid = pid; }

    bool drawProcessPicker(const char* id, std::vector<RuntimeProcessInfo>& processes, pid_t& pid, char* search, const std::size_t searchSize, const float comboWidth)
    {
        if (processes.empty()) processes = enumerateRuntimeProcesses();
        bool changed = false;
        bool& linked = selectorLinked(id);
        if (linked)
        {
            if (SharedProcessPid > 0 && pid != SharedProcessPid) { pid = SharedProcessPid; changed = true; }
            else if (SharedProcessPid <= 0 && pid > 0) SharedProcessPid = pid;
        }

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
                if (ImGui::Selectable(label.c_str(), active)) { pid = process.Pid; changed = true; if (linked) SharedProcessPid = pid; }
                if (active) ImGui::SetItemDefaultFocus();
            }
            if (matches == 0) ImGui::TextDisabled("%s", i18n::tr("common.noProcessMatches"));
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        if (linked) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x,accent.y,accent.z,0.28f)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent.x,accent.y,accent.z,0.48f)); }
        if (ImGui::SmallButton(linked ? i18n::tr("common.processLinked") : i18n::tr("common.processLocal")))
        {
            linked = !linked;
            if (linked)
            {
                if (SharedProcessPid > 0 && pid != SharedProcessPid) { pid = SharedProcessPid; changed = true; }
                else if (pid > 0) SharedProcessPid = pid;
            }
        }
        if (linked) ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::tr("common.processLinkTooltip"));
        ImGui::PopID(); return changed;
    }
}
