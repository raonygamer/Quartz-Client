#include "quartz/client/ui/pages/ShadersPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/I18n.hpp"
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
        void syncEditorText(TextEditor& editor, const std::string_view text) { if (editor.GetText() != text) editor.SetText(text); }
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

        ImGui::TextWrapped("%s", i18n::tr("shaders.description"));
        ImGui::SeparatorText(i18n::tr("shaders.current"));
        const bool presetValid = settings.ShaderPresetIndex > 0 && settings.ShaderPresetIndex <= static_cast<int>(ShaderPresets.size());
        const char* presetPreview = presetValid ? ShaderPresets[static_cast<std::size_t>(settings.ShaderPresetIndex - 1)].Name.c_str() : i18n::tr("shaders.customExternal");
        if (ImGui::BeginCombo(i18n::tr("shaders.catalog"), presetPreview))
        {
            if (ImGui::Selectable(i18n::tr("shaders.customExternal"), settings.ShaderPresetIndex == 0)) { settings.ShaderPresetIndex = 0; settings.ShaderId.clear(); }
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
        ImGui::SameLine(); if (ImGui::Button(i18n::tr("shaders.refreshCatalog"))) refreshShaderLibrary();
        ImGui::SameLine(); if (ImGui::Button(i18n::tr("common.edit")))
        {
            initializeShaderEditors(editor, vertexSource.data(), fragmentSource.data());
            syncEditorText(editor.Vertex, vertexSource.data()); syncEditorText(editor.Fragment, fragmentSource.data()); updateShaderDiagnostics(editor, framebuffer.status()); manager.open("shader-editor");
        }
        if (ImGui::Button(i18n::tr("shaders.compileCurrent"))) compileShaders(framebuffer, editor, vertexSource, fragmentSource);
        ImGui::SameLine(); if (ImGui::Button(i18n::tr("shaders.saveDefaults"))) saveShaderSources(vertexSource, fragmentSource);
        ImGui::SameLine(); if (ImGui::Button(i18n::tr("shaders.restoreDefault")))
        {
            clearExternalShaderFile(editor, false); clearExternalShaderFile(editor, true);
            settings.ShaderPresetIndex = 1; settings.ShaderId = ShaderPresets.front().Id;
            setShaderSource(vertexSource, DefaultVertexShaderSource); setShaderSource(fragmentSource, ShaderPresets.front().FragmentSource);
            syncEditorText(editor.Vertex, vertexSource.data()); syncEditorText(editor.Fragment, fragmentSource.data()); compileShaders(framebuffer, editor, vertexSource, fragmentSource); saveShaderSources(vertexSource, fragmentSource);
        }
        ImGui::SameLine(); ImGui::TextDisabled("%s", framebuffer.status().c_str());

        ImGui::SeparatorText(i18n::tr("shaders.externalFiles"));
        const float externalWidth = ImGui::GetContentRegionAvail().x;
        const float vertexInputWidth = std::clamp(externalWidth - 390.0f, 260.0f, 760.0f);
        const float fragmentInputWidth = std::clamp(externalWidth - 540.0f, 260.0f, 650.0f);
        const std::string selectVertex = std::string(i18n::tr("common.select")) + "##vertex";
        const std::string openVertex = std::string(i18n::tr("common.open")) + "##vertex";
        const std::string selectFragment = std::string(i18n::tr("common.select")) + "##fragment";
        const std::string openFragment = std::string(i18n::tr("common.open")) + "##fragment";
        ImGui::SetNextItemWidth(vertexInputWidth); ImGui::InputText(i18n::tr("shaders.vertexFile"), vertexPath.data(), vertexPath.size()); ImGui::SameLine();
        if (ImGui::Button(selectVertex.c_str())) if (const auto path = pickShaderFile()) setPath(vertexPath, *path); ImGui::SameLine();
        if (ImGui::Button(openVertex.c_str())) loadExternalShaderFile(editor, framebuffer, vertexSource, fragmentSource, settings, vertexPath.data(), false);
        if (!editor.ExternalVertexPath.empty()) { ImGui::TextDisabled("bound vertex: %s", editor.ExternalVertexPath.string().c_str()); ImGui::SameLine(); if (ImGui::SmallButton("Unbind##vertex")) clearExternalShaderFile(editor, false); }

        ImGui::SetNextItemWidth(fragmentInputWidth); ImGui::InputText(i18n::tr("shaders.fragmentFile"), fragmentPath.data(), fragmentPath.size()); ImGui::SameLine();
        if (ImGui::Button(selectFragment.c_str())) if (const auto path = pickShaderFile()) setPath(fragmentPath, *path); ImGui::SameLine();
        if (ImGui::Button(openFragment.c_str())) loadExternalShaderFile(editor, framebuffer, vertexSource, fragmentSource, settings, fragmentPath.data(), true); ImGui::SameLine();
        if (ImGui::Button(i18n::tr("shaders.importCatalog")))
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
        ImGui::Checkbox(i18n::tr("shaders.hotReload"), &editor.HotReloadExternal);
        if (!hasExternal) ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::TextDisabled("%s", i18n::tr("shaders.hotReloadHint"));
        if (!editor.ExternalStatus.empty()) ImGui::TextWrapped("%s", editor.ExternalStatus.c_str());

        ImGui::SeparatorText(i18n::tr("shaders.renderSurface"));
        int requestedSize[2]{settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight};
        if (ImGui::InputInt2(i18n::tr("shaders.framebufferSize"), requestedSize))
        {
            settings.ShaderFramebufferWidth = std::clamp(requestedSize[0], static_cast<int>(Columns), MaxShaderDimension);
            settings.ShaderFramebufferHeight = std::clamp(requestedSize[1], static_cast<int>(Rows), MaxShaderDimension);
        }
        ImGui::SameLine(); if (ImGui::Button(i18n::tr("shaders.regenerate"))) framebuffer.regenerate(settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight);
        const char* downsampleModes[] = {"Average logical cell", "Average center 4x4", "Center pixel / exact"};
        ImGui::Combo(i18n::tr("shaders.downsample"), &settings.ShaderDownsampleMode, downsampleModes, 3);
        ImGui::SliderFloat(i18n::tr("shaders.crossfade"), &settings.ShaderTransitionSeconds, 0.0f, 5.0f, "%.2f s");
        ImGui::Checkbox(i18n::tr("shaders.recompileChange"), &settings.ShaderRecompileOnChange);
        ImGui::Checkbox(i18n::tr("shaders.keyUniforms"), &settings.ShaderKeyStateUniforms);
        ImGui::Checkbox(i18n::tr("shaders.capsColor"), &settings.ShaderCapsLockColorEnabled); ImGui::SameLine(); ImGui::ColorEdit3("##capsShaderColor", settings.ShaderCapsLockColor.data(), ImGuiColorEditFlags_NoInputs);
        ImGui::Checkbox(i18n::tr("shaders.scrollColor"), &settings.ShaderScrollLockColorEnabled); ImGui::SameLine(); ImGui::ColorEdit3("##scrollShaderColor", settings.ShaderScrollLockColor.data(), ImGuiColorEditFlags_NoInputs);
        ImGui::TextDisabled("Active surface %dx%d -> 16x7 QRPC framebuffer", framebuffer.width(), framebuffer.height());

        ImGui::SeparatorText(i18n::tr("shaders.materialParameters"));
        drawShaderMaterialEditor(framebuffer, 190.0f);
        ImGui::TextDisabled("Arbitrary active uniforms are reflected automatically; // @ui annotations can override labels/ranges/defaults.");
    }
}
