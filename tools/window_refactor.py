from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
inc = root / "include/quartz/client"
src = root / "src"
platform_inc = inc / "platform"
ui_inc = inc / "ui"
platform_src = src / "platform"
ui_src = src / "ui"
platform_inc.mkdir(parents=True, exist_ok=True)
platform_src.mkdir(parents=True, exist_ok=True)
ui_inc.mkdir(parents=True, exist_ok=True)
ui_src.mkdir(parents=True, exist_ok=True)

(platform_inc / "Window.hpp").write_text(r'''#pragma once
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
''')

(platform_src / "Window.cpp").write_text(r'''#include "quartz/client/platform/Window.hpp"

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
''')

(ui_inc / "ImGuiRuntime.hpp").write_text(r'''#pragma once
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
''')

(ui_src / "ImGuiRuntime.cpp").write_text(r'''#include "quartz/client/ui/ImGuiRuntime.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/platform/Window.hpp"

namespace quartz::client::ui
{
    ImGuiRuntime::~ImGuiRuntime() { shutdown(); }

    bool ImGuiRuntime::initialize(GLFWwindow* window) noexcept
    {
        if (_contextCreated) return true;
        if (!window) return false;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        _contextCreated = true;
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        applyDarkTheme();
        initializeShaderEditorFonts();

        constexpr const char* GLSLVersion = "#version 330";
        if (!ImGui_ImplGlfw_InitForOpenGL(window, true))
        {
            std::fprintf(stderr, "Failed to initialize ImGui GLFW backend\n");
            shutdown();
            return false;
        }
        _glfwBackend = true;
        if (!ImGui_ImplOpenGL3_Init(GLSLVersion))
        {
            std::fprintf(stderr, "Failed to initialize ImGui OpenGL backend\n");
            shutdown();
            return false;
        }
        _openGLBackend = true;
        return true;
    }

    void ImGuiRuntime::beginFrame() const noexcept
    {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void ImGuiRuntime::render(const platform::Window& window) const noexcept
    {
        GLFWwindow* handle = window.handle();
        if (!handle || !_contextCreated) return;
        ImGui::Render();
        int width = 0, height = 0;
        glfwGetFramebufferSize(handle, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(handle);
    }

    void ImGuiRuntime::shutdown() noexcept
    {
        if (_openGLBackend)
        {
            ImGui_ImplOpenGL3_Shutdown();
            _openGLBackend = false;
        }
        if (_glfwBackend)
        {
            ImGui_ImplGlfw_Shutdown();
            _glfwBackend = false;
        }
        if (_contextCreated)
        {
            ImGui::DestroyContext();
            _contextCreated = false;
        }
    }
}
''')

application = src / "Application.cpp"
text = application.read_text()
if '#include "quartz/client/platform/Window.hpp"' not in text:
    text = text.replace('#include "quartz/client/ui/PageManager.hpp"\n', '#include "quartz/client/ui/PageManager.hpp"\n#include "quartz/client/platform/Window.hpp"\n#include "quartz/client/ui/ImGuiRuntime.hpp"\n')

start = text.find("    glfwSetErrorCallback(")
end = text.find("    RawUSB usb;", start)
if start < 0 or end < 0:
    raise SystemExit("could not locate legacy GLFW/ImGui initialization")
replacement = '''    platform::Window window;\n    if (!window.initialize(1280, 800, "Quartz")) return EXIT_FAILURE;\n    ui::ImGuiRuntime imgui;\n    if (!imgui.initialize(window.handle())) return EXIT_FAILURE;\n\n'''
text = text[:start] + replacement + text[end:]
text = text.replace("        glfwHideWindow(window);", "        window.hide();")
text = text.replace("    while (!glfwWindowShouldClose(window))", "    while (!window.shouldClose())")
text = text.replace("if (keyboardInput.consumeRestoreRequest()) restoreWindow(window);", "if (keyboardInput.consumeRestoreRequest()) window.restore();")
text = text.replace("        glfwPollEvents();", "        window.pollEvents();")
text = text.replace("        runtimeBindings.pollProfileHotkeys(window);", "        runtimeBindings.pollProfileHotkeys(window.handle());")
text = text.replace("        ImGui_ImplOpenGL3_NewFrame();\n        ImGui_ImplGlfw_NewFrame();\n        ImGui::NewFrame();", "        imgui.beginFrame();")
legacy_present = '''        ImGui::Render();\n\n        int width;\n        int height;\n        glfwGetFramebufferSize(window, &width, &height);\n        glViewport(0, 0, width, height);\n        glClearColor(0.05f, 0.05f, 0.05f, 1.0f);\n        glClear(GL_COLOR_BUFFER_BIT);\n        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());\n        glfwSwapBuffers(window);'''
if legacy_present not in text:
    raise SystemExit("could not locate legacy present block")
text = text.replace(legacy_present, "        imgui.render(window);")
legacy_shutdown = '''    ImGui_ImplOpenGL3_Shutdown();\n    ImGui_ImplGlfw_Shutdown();\n    ImGui::DestroyContext();\n    glfwDestroyWindow(window);\n    glfwTerminate();'''
if legacy_shutdown not in text:
    raise SystemExit("could not locate legacy window/UI shutdown")
text = text.replace(legacy_shutdown, "    imgui.shutdown();\n    window.shutdown();")
application.write_text(text)

# Drop the obsolete raw GLFW helper functions now that Application owns an actual Window object.
functions = inc / "Functions.hpp"
text = functions.read_text()
text = re.sub(r"^\s*void hideWindow\(GLFWwindow\* window\);\n", "", text, flags=re.M)
text = re.sub(r"^\s*void restoreWindow\(GLFWwindow\* window\);\n", "", text, flags=re.M)
functions.write_text(text)

ui_cpp = src / "ui/UI.cpp"
text = ui_cpp.read_text()
text = re.sub(r"\n    void hideWindow\(GLFWwindow\* window\)\n    \{.*?\n    \}\n", "\n", text, count=1, flags=re.S)
text = re.sub(r"\n    void restoreWindow\(GLFWwindow\* window\)\n    \{.*?\n    \}\n", "\n", text, count=1, flags=re.S)
ui_cpp.write_text(text)

print("isolated GLFW/OpenGL window lifecycle and ImGui backend runtime")
