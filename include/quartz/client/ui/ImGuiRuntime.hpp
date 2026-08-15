#pragma once
#include "quartz/client/Common.hpp"

namespace quartz::client::platform { class Window; }

namespace quartz::client::ui
{
    class ImGuiRuntime final
    {
    public:
        ImGuiRuntime() = default;
        ~ImGuiRuntime();
        ImGuiRuntime(const ImGuiRuntime&) = delete;
        ImGuiRuntime& operator=(const ImGuiRuntime&) = delete;
        ImGuiRuntime(ImGuiRuntime&&) = delete;
        ImGuiRuntime& operator=(ImGuiRuntime&&) = delete;

        bool initialize(GLFWwindow* window) noexcept;
        void beginFrame() const noexcept;
        void render(const platform::Window& window) const noexcept;
        void shutdown() noexcept;

    private:
        bool _contextCreated = false;
        bool _glfwBackend = false;
        bool _openGLBackend = false;
    };
}
