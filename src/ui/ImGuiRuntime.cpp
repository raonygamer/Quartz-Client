#include "quartz/client/ui/ImGuiRuntime.hpp"
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
