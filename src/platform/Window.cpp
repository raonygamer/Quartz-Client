#include "quartz/client/platform/Window.hpp"

namespace quartz::client::platform
{
    Window::~Window() { shutdown(); }

    bool Window::initialize(const int width, const int height, const char* title) noexcept
    {
        if (_handle) return true;
        glfwSetErrorCallback([](const int error, const char* description) { std::fprintf(stderr, "GLFW error %d: %s\n", error, description); });
        if (!glfwInit()) return false;
        _glfwInitialized = true;

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        _handle = glfwCreateWindow(width, height, title, nullptr, nullptr);
        if (!_handle)
        {
            shutdown();
            return false;
        }
        glfwSetWindowCloseCallback(_handle, [](GLFWwindow* target)
        {
            glfwSetWindowShouldClose(target, GLFW_FALSE);
            glfwHideWindow(target);
        });
        glfwMakeContextCurrent(_handle);
#ifdef GLFW_LOCK_KEY_MODS
        glfwSetInputMode(_handle, GLFW_LOCK_KEY_MODS, GLFW_TRUE);
#endif
        glfwSwapInterval(0);
        if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress)))
        {
            std::fprintf(stderr, "Failed to initialize OpenGL loader\n");
            shutdown();
            return false;
        }
        return true;
    }

    void Window::shutdown() noexcept
    {
        if (_handle)
        {
            glfwDestroyWindow(_handle);
            _handle = nullptr;
        }
        if (_glfwInitialized)
        {
            glfwTerminate();
            _glfwInitialized = false;
        }
    }

    void Window::restore() const noexcept
    {
        if (!_handle) return;
        glfwShowWindow(_handle);
        glfwRestoreWindow(_handle);
        glfwFocusWindow(_handle);
    }
}
