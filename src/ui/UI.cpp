#include "ui/UI.hpp"

#include "state/AppState.hpp"
#include "ui/scene/StartScene.hpp"

namespace quartz::client::state
{
    struct AppState;
}
namespace quartz::client::ui
{
    std::unordered_map<std::string, std::unique_ptr<Scene>> UI::_scenes = {};
    Scene* UI::_current = nullptr;
    Scene* UI::_last = nullptr;
    state::AppState* UI::_currentAppState = nullptr;

    void UI::initialize(state::AppState* state)
    {
        _currentAppState = state;
        addScene<scenes::StartScene>("scenes.start");
        switchScene("scenes.start");
    }

    void UI::render(state::AppState& state, const utils::TimeStep& time)
    {
        if (!_current)
            return;
        _current->onRender(state, time);
    }

    bool UI::switchScene(const std::string& id)
    {
        const auto it = _scenes.find(id);
        if (it == _scenes.end())
            return false;
        if (_current)
            _current->onLeave(*_currentAppState);
        _last = _current;
        _current = it->second.get();
        _current->onEnter(*_currentAppState);
        return true;
    }
}