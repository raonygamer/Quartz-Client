#pragma once
#include "ui/scene/Scene.hpp"

namespace quartz::client::ui::scenes
{
    class StartScene : public Scene
    {
    public:
        explicit StartScene(std::string id);
        ~StartScene() override = default;
        void onEnter(state::AppState& state) override;
        void onRender(state::AppState& state, const utils::TimeStep& time) override;
        void onLeave(state::AppState& state) override;
    };
}