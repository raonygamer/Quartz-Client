#pragma once
#include "utils/Time.hpp"
#include <string>
#include <utility>

namespace quartz::client::state
{
    struct AppState;
}

namespace quartz::client::ui
{
    class Scene
    {
        std::string _id;
    public:
        explicit Scene(std::string id) :
            _id(std::move(id)) {}
        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;
        Scene(Scene&&) = delete;
        Scene& operator=(Scene&&) = delete;
        virtual ~Scene() = default;
        virtual void onEnter(state::AppState& state) {}
        virtual void onRender(state::AppState& state, const utils::TimeStep& time) = 0;
        virtual void onLeave(state::AppState& state) {}
        const std::string& id() const
        {
            return _id;
        }
    };
}