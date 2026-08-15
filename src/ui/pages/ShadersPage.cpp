#include "quartz/client/ui/pages/ShadersPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/shader/ShaderWorkspace.hpp"

namespace quartz::client::ui
{
    namespace
    {
        std::optional<std::filesystem::path> pickShaderFile()
        {
            std::string result;
            if (commandExists("kdialog")) result = trim(bytesToString(readCommand("kdialog --getopenfilename . '*.frag *.vert *.glsl|GLSL shaders'")));
            else if (commandExists("zenity")) result = trim(bytesToString(readCommand("zenity --file-selection --file-filter='GLSL shaders | *.frag *.vert *.glsl'")));
            if (result.empty()) return std::nullopt;
            return std::filesystem::path(result);
        }

        void setPath(std::array<char, ShaderPathCapacity>& buffer, const std::filesystem::path& path) { std::snprintf(buffer.data(), buffer.size(), "%s", path.string().c_str()); }
    }

    void ShadersPage::render(PageContext& context, PageManager& manager)
    {
        auto& framebuffer = context.shaderFramebuffer;
        auto& transition = context.shaderTransition;
        auto& editor = context.shaderEditor;
        auto& vertexSource = context.vertexShaderSource;
        auto& fragmentSource = context.fragmentShaderSource;
        auto& vertexPath = context.vertexLoadPath;
        auto& fragmentPath = context.fragmentLoadPath;
        auto& settings = context.settings;
        static const VisualizerSettings defaults{};

        ImGui::TextWrapped("Shader catalog, external source files, hot reload, compilation and reflected material parameters live here. Opening a file keeps Quartz bound to it; importing copies a fragment shader into the Quartz catalog.");
        ImGui::SeparatorText("Current shader");
        const bool presetValid = settings.ShaderPresetIndex > 0 && settings.ShaderPresetIndex <= static_cast<int>(ShaderPresets.size());
        const char* presetPreview = presetValid ? ShaderPresets[static_cast<std::size_t>(settings.ShaderPresetIndex - 1)].Name.c_str() : "Custom / external";
        if (ImGui::BeginCombo("Catalog", presetPreview))
        {
            if (ImGui::Selectable("Custom / external", settings.ShaderPresetIndex == 0)) { settings.ShaderPresetIndex = 0; settings.ShaderId.clear(); }
            for (std::size_t i = 0; i < ShaderPresets.size(); ++i)
            {
                const bool selected = settings.ShaderPresetIndex == static_cast<int>(i + 1);
                std::string label = ShaderPresets[i].Name;
                if (!ShaderPresets[i].BuiltIn) label += "  [catalog]";
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    clearExternalShaderFile(editor, true);
                    switchShaderPreset(framebuffer, transition, editor, vertexSource, fragmentSource, settings, static_cast<int>(i + 1), glfwGetTime(), settings.ShaderTransitionSeconds);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); if (ImGui::Button("Refresh catalog")) refreshShaderLibrary();
        ImGui::SameLine(); if (ImGui::Button("Edit..."))
        {
            initializeShaderEditors(editor, vertexSource.data(), fragmentSource.data());
            editor.Vertex.SetText(vertexSource.data()); editor.Fragment.SetText(fragmentSource.data()); updateShaderDiagnostics(editor, framebuffer.status()); manager.open("shader-editor");
        }
        if (ImGui::Button("Compile current")) compileShaders(framebuffer, editor, vertexSource, fragmentSource);
        ImGui::SameLine(); if (ImGui::Button("Save as Quartz defaults")) saveShaderSources(vertexSource, fragmentSource);
        ImGui::SameLine(); if (ImGui::Button("Restore default"))
        {
            clearExternalShaderFile(editor, false); clearExternalShaderFile(editor, true);
            settings.ShaderPresetIndex = 1; settings.ShaderId = ShaderPresets.front().Id;
            setShaderSource(vertexSource, DefaultVertexShaderSource); setShaderSource(fragmentSource, ShaderPresets.front().FragmentSource);
            editor.Vertex.SetText(vertexSource.data()); editor.Fragment.SetText(fragmentSource.data()); compileShaders(framebuffer, editor, vertexSource, fragmentSource); saveShaderSources(vertexSource, fragmentSource);
        }
        ImGui::SameLine(); ImGui::TextDisabled("%s", framebuffer.status().c_str());

        ImGui::SeparatorText("External files");
        ImGui::SetNextItemWidth(-180.0f); ImGui::InputText("Vertex file", vertexPath.data(), vertexPath.size()); ImGui::SameLine();
        if (ImGui::Button("Select##vertex")) if (const auto path = pickShaderFile()) setPath(vertexPath, *path); ImGui::SameLine();
        if (ImGui::Button("Open##vertex")) loadExternalShaderFile(editor, framebuffer, vertexSource, fragmentSource, settings, vertexPath.data(), false);
        if (!editor.ExternalVertexPath.empty()) { ImGui::TextDisabled("bound vertex: %s", editor.ExternalVertexPath.string().c_str()); ImGui::SameLine(); if (ImGui::SmallButton("Unbind##vertex")) clearExternalShaderFile(editor, false); }

        ImGui::SetNextItemWidth(-280.0f); ImGui::InputText("Fragment file", fragmentPath.data(), fragmentPath.size()); ImGui::SameLine();
        if (ImGui::Button("Select##fragment")) if (const auto path = pickShaderFile()) setPath(fragmentPath, *path); ImGui::SameLine();
        if (ImGui::Button("Open##fragment")) loadExternalShaderFile(editor, framebuffer, vertexSource, fragmentSource, settings, fragmentPath.data(), true); ImGui::SameLine();
        if (ImGui::Button("Import to catalog"))
        {
            std::string importedId, error;
            if (importShaderToLibrary(fragmentPath.data(), importedId, error))
            {
                refreshShaderLibrary();
                if (switchShaderId(framebuffer, transition, editor, vertexSource, fragmentSource, settings, importedId, glfwGetTime(), settings.ShaderTransitionSeconds)) editor.ExternalStatus = "Imported to catalog as " + importedId;
                else editor.ExternalStatus = "Imported, but could not activate " + importedId;
            }
            else editor.ExternalStatus = std::move(error);
        }
        if (!editor.ExternalFragmentPath.empty()) { ImGui::TextDisabled("bound fragment: %s", editor.ExternalFragmentPath.string().c_str()); ImGui::SameLine(); if (ImGui::SmallButton("Unbind##fragment")) clearExternalShaderFile(editor, true); }
        const bool hasExternal = !editor.ExternalVertexPath.empty() || !editor.ExternalFragmentPath.empty();
        if (!hasExternal) ImGui::BeginDisabled();
        ImGui::Checkbox("Hot reload external changes", &editor.HotReloadExternal);
        if (!hasExternal) ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::TextDisabled("200 ms debounce/poll; external edits compile automatically");
        if (!editor.ExternalStatus.empty()) ImGui::TextWrapped("%s", editor.ExternalStatus.c_str());

        ImGui::SeparatorText("Render surface");
        int requestedSize[2]{settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight};
        if (ImGui::InputInt2("Framebuffer size", requestedSize))
        {
            settings.ShaderFramebufferWidth = std::clamp(requestedSize[0], static_cast<int>(Columns), MaxShaderDimension);
            settings.ShaderFramebufferHeight = std::clamp(requestedSize[1], static_cast<int>(Rows), MaxShaderDimension);
        }
        ImGui::SameLine(); if (ImGui::Button("Regenerate")) framebuffer.regenerate(settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight);
        const char* downsampleModes[] = {"Average logical cell", "Average center 4x4", "Center pixel / exact"};
        ImGui::Combo("Downsample", &settings.ShaderDownsampleMode, downsampleModes, 3);
        ImGui::SliderFloat("Crossfade", &settings.ShaderTransitionSeconds, 0.0f, 5.0f, "%.2f s");
        ImGui::Checkbox("Recompile on editor text change", &settings.ShaderRecompileOnChange);
        ImGui::Checkbox("Key-state uniforms", &settings.ShaderKeyStateUniforms);
        ImGui::Checkbox("Caps Lock fixed color", &settings.ShaderCapsLockColorEnabled); ImGui::SameLine(); ImGui::ColorEdit3("##capsShaderColor", settings.ShaderCapsLockColor.data(), ImGuiColorEditFlags_NoInputs);
        ImGui::Checkbox("Scroll Lock fixed color", &settings.ShaderScrollLockColorEnabled); ImGui::SameLine(); ImGui::ColorEdit3("##scrollShaderColor", settings.ShaderScrollLockColor.data(), ImGuiColorEditFlags_NoInputs);
        ImGui::TextDisabled("Active surface %dx%d -> 16x7 QRPC framebuffer", framebuffer.width(), framebuffer.height());

        ImGui::SeparatorText("Material parameters");
        drawShaderMaterialEditor(framebuffer, 190.0f);
        ImGui::TextDisabled("Arbitrary active uniforms are reflected automatically; // @ui annotations can override labels/ranges/defaults.");
    }
}
