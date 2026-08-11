#pragma once
#include "ui/scene/Scene.hpp"
#include "utils/Time.hpp"
#include <string>
#include <unordered_map>
#include <memory>

namespace quartz::client::state
{
    struct AppState;
}

namespace quartz::client::ui
{
    class UI
    {
        static std::unordered_map<std::string, std::unique_ptr<Scene>> _scenes;
        static Scene* _current;
        static Scene* _last;
        static state::AppState* _currentAppState;
    public:
        static void initialize(state::AppState* state);
        static void render(state::AppState& state, const utils::TimeStep& time);
        static bool switchScene(const std::string& id);

        template<typename T = Scene, typename... Args>
        requires (std::is_base_of_v<Scene, T> && std::constructible_from<T, const std::string&, Args...>)
        static T& addScene(const std::string& id, Args&&... args)
        {
            _scenes.emplace(id, std::make_unique<T>(id, std::forward<Args>(args)...));
            return *static_cast<T*>(_scenes[id].get());
        }
    };
}