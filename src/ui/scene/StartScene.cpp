#include "ui/scene/StartScene.hpp"
#include "imgui.h"

namespace quartz::client::ui::scenes
{
    StartScene::StartScene(std::string id) :
        Scene(std::move(id))
    {
    }

    void StartScene::onEnter(state::AppState&)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    }

    void StartScene::onRender(state::AppState& state, const utils::TimeStep& time)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::Begin(
            "Quartz",
            nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoSavedSettings
        );
        constexpr auto text = "Waiting for Quartz device...";
        ImGui::SetWindowFontScale(2.0f);
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        const ImVec2 available = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos({
            (available.x - textSize.x) * 0.5f,
            (available.y - textSize.y) * 0.5f
        });
        ImGui::TextUnformatted(text);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::End();
    }

    void StartScene::onLeave(state::AppState&)
    {
        ImGui::PopStyleVar(2);
    }
}