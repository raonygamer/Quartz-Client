#pragma once
#include "quartz/client/Common.hpp"

namespace quartz::client::platform
{
    class Window final
    {
    public:
        Window() = default;
        ~Window();
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;
        Window(Window&&) = delete;
        Window& operator=(Window&&) = delete;

        bool initialize(int width, int height, const char* title) noexcept;
        void shutdown() noexcept;
        void pollEvents() const noexcept { glfwPollEvents(); }
        void hide() const noexcept { if (_handle) glfwHideWindow(_handle); }
        void restore() const noexcept;

        [[nodiscard]] GLFWwindow* handle() const noexcept { return _handle; }
        [[nodiscard]] bool shouldClose() const noexcept { return !_handle || glfwWindowShouldClose(_handle) != GLFW_FALSE; }

    private:
        GLFWwindow* _handle = nullptr;
        bool _glfwInitialized = false;
    };
}
