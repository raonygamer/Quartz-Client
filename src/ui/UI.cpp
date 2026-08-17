#include "quartz/client/Model.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/Configuration.hpp"
#include "quartz/client/ui/I18n.hpp"
#include "quartz/client/ui/Theme.hpp"

namespace quartz::client
{
    bool drawShaderMaterialEditor(ShaderFramebuffer& shaderFramebuffer, const float maxHeight)
    {
        auto& parameters = shaderFramebuffer.materialParameters();
        if (parameters.empty())
        {
            ImGui::TextDisabled("No reflected user uniforms. Add an active uniform to GLSL, e.g. uniform float uSpeed;");
            ImGui::TextDisabled("Optional metadata: // @ui min=0 max=4 step=0.01 default=1 label=\"Speed\"  |  use @ui color for vec3/vec4.");
            return false;
        }

        bool changed = false;
        const float height = std::min(maxHeight, 34.0f + static_cast<float>(parameters.size()) * ImGui::GetFrameHeightWithSpacing());
        if (ImGui::BeginChild("##ShaderMaterialParameters", ImVec2(0.0f, height), true))
        {
            for (auto& parameter : parameters)
            {
                ImGui::PushID(parameter.Name.c_str());
                ImGui::SetNextItemWidth(std::min(330.0f, ImGui::GetContentRegionAvail().x * 0.58f));
                bool itemChanged = false;
                const float minValue = parameter.HasMin ? parameter.Min : 0.0f;
                const float maxValue = parameter.HasMax ? parameter.Max : 0.0f;
                if (parameter.Boolean)
                {
                    if (parameter.Components == 1)
                    {
                        bool value = parameter.IntValue[0] != 0;
                        if (ImGui::Checkbox(parameter.Label.c_str(), &value)) { parameter.IntValue[0] = value ? 1 : 0; itemChanged = true; }
                    }
                    else
                    {
                        ImGui::TextUnformatted(parameter.Label.c_str());
                        for (int component = 0; component < parameter.Components; ++component)
                        {
                            if (component != 0) ImGui::SameLine();
                            ImGui::PushID(component);
                            bool value = parameter.IntValue[static_cast<std::size_t>(component)] != 0;
                            const char* names[] = {"X", "Y", "Z", "W"};
                            if (ImGui::Checkbox(names[component], &value)) { parameter.IntValue[static_cast<std::size_t>(component)] = value ? 1 : 0; itemChanged = true; }
                            ImGui::PopID();
                        }
                    }
                }
                else if (parameter.Color)
                {
                    itemChanged = parameter.Components == 3
                        ? ImGui::ColorEdit3(parameter.Label.c_str(), parameter.FloatValue.data(), ImGuiColorEditFlags_Float)
                        : ImGui::ColorEdit4(parameter.Label.c_str(), parameter.FloatValue.data(), ImGuiColorEditFlags_Float);
                }
                else if (parameter.Integer)
                {
                    const float speed = std::max(parameter.Step, 1.0f);
                    switch (parameter.Components)
                    {
                    case 1: itemChanged = ImGui::DragInt(parameter.Label.c_str(), parameter.IntValue.data(), speed, parameter.HasMin ? static_cast<int>(std::lround(minValue)) : 0, parameter.HasMax ? static_cast<int>(std::lround(maxValue)) : 0); break;
                    case 2: itemChanged = ImGui::DragInt2(parameter.Label.c_str(), parameter.IntValue.data(), speed, parameter.HasMin ? static_cast<int>(std::lround(minValue)) : 0, parameter.HasMax ? static_cast<int>(std::lround(maxValue)) : 0); break;
                    case 3: itemChanged = ImGui::DragInt3(parameter.Label.c_str(), parameter.IntValue.data(), speed, parameter.HasMin ? static_cast<int>(std::lround(minValue)) : 0, parameter.HasMax ? static_cast<int>(std::lround(maxValue)) : 0); break;
                    case 4: itemChanged = ImGui::DragInt4(parameter.Label.c_str(), parameter.IntValue.data(), speed, parameter.HasMin ? static_cast<int>(std::lround(minValue)) : 0, parameter.HasMax ? static_cast<int>(std::lround(maxValue)) : 0); break;
                    default: break;
                    }
                }
                else
                {
                    switch (parameter.Components)
                    {
                    case 1: itemChanged = ImGui::DragFloat(parameter.Label.c_str(), parameter.FloatValue.data(), parameter.Step, minValue, maxValue, "%.3f"); break;
                    case 2: itemChanged = ImGui::DragFloat2(parameter.Label.c_str(), parameter.FloatValue.data(), parameter.Step, minValue, maxValue, "%.3f"); break;
                    case 3: itemChanged = ImGui::DragFloat3(parameter.Label.c_str(), parameter.FloatValue.data(), parameter.Step, minValue, maxValue, "%.3f"); break;
                    case 4: itemChanged = ImGui::DragFloat4(parameter.Label.c_str(), parameter.FloatValue.data(), parameter.Step, minValue, maxValue, "%.3f"); break;
                    default: break;
                    }
                }

                ImGui::SameLine();
                if (ImGui::SmallButton("Reset"))
                {
                    if (parameter.Integer || parameter.Boolean) parameter.IntValue = parameter.IntDefault;
                    else parameter.FloatValue = parameter.FloatDefault;
                    itemChanged = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", shaderUniformTypeName(parameter.Type));
                if (ImGui::IsItemHovered())
                {
                    if (parameter.PersistenceKey == parameter.Name)
                        ImGui::SetTooltip("%s", parameter.Name.c_str());
                    else
                        ImGui::SetTooltip("%s\\nPersistent id: %s", parameter.Name.c_str(), parameter.PersistenceKey.c_str());
                }
                changed |= itemChanged;
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        if (changed) shaderFramebuffer.markMaterialChanged();
        return changed;
    }

    void drawShaderEditorPage(RawUSB& usb, SharedDeviceState& deviceState, const EvdevKeyboard& keyboardInput, ShaderFramebuffer& shaderFramebuffer, ShaderTransitionState& shaderTransition, ShaderEditorState& shaderEditor, ViewPage& page, std::array<char, ShaderSourceCapacity>& vertexShaderSource, std::array<char, ShaderSourceCapacity>& fragmentShaderSource, std::array<char, ShaderPathCapacity>& vertexLoadPath, std::array<char, ShaderPathCapacity>& fragmentLoadPath, VisualizerSettings& settings, const std::array<Color32, MatrixSize>& framebuffer, JavaScriptRuntime& javascript, const float appCpuUsage, const bool scrollLockActive, const bool capsLockActive)
    {
        initializeShaderEditors(shaderEditor, vertexShaderSource.data(), fragmentShaderSource.data());
        if (ImGui::Button("< Back"))
            page = ViewPage::Main;
        ImGui::SameLine();
        ImGui::TextUnformatted("Shader IDE");
        ImGui::SameLine();
        if (ImGui::Button("Compile"))
        {
            setShaderSource(vertexShaderSource, shaderEditor.Vertex.GetText());
            setShaderSource(fragmentShaderSource, shaderEditor.Fragment.GetText());
            if (compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource))
                saveShaderSources(vertexShaderSource, fragmentShaderSource);
        }
        ImGui::SameLine();
        if (ImGui::Button("Save"))
        {
            setShaderSource(vertexShaderSource, shaderEditor.Vertex.GetText());
            setShaderSource(fragmentShaderSource, shaderEditor.Fragment.GetText());
            saveShaderSources(vertexShaderSource, fragmentShaderSource);
        }
        TextEditor& activeEditor = shaderEditor.ActiveStage == 0 ? shaderEditor.Fragment : shaderEditor.Vertex;
        ImGui::SameLine();
        ImGui::BeginDisabled(!activeEditor.CanUndo());
        if (ImGui::Button("Undo")) activeEditor.Undo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!activeEditor.CanRedo());
        if (ImGui::Button("Redo")) activeEditor.Redo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Find")) activeEditor.OpenFindReplaceWindow();
        ImGui::SameLine();
        ImGui::Checkbox("Recompile on change", &settings.ShaderRecompileOnChange);
        ImGui::SameLine();
        if (ImGui::SmallButton("-##ShaderZoom")) settings.ShaderEditorZoom = std::max(settings.ShaderEditorZoom - 0.10f, 0.60f);
        ImGui::SameLine();
        if (ImGui::SmallButton("100%##ShaderZoom")) settings.ShaderEditorZoom = 1.0f;
        ImGui::SameLine();
        if (ImGui::SmallButton("+##ShaderZoom")) settings.ShaderEditorZoom = std::min(settings.ShaderEditorZoom + 0.10f, 2.50f);
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f%% / %.0f px", settings.ShaderEditorZoom * 100.0f, shaderEditorPixelSize(settings.ShaderEditorZoom));

        const bool compileOk = shaderFramebuffer.status().starts_with("Shaders compiled") || shaderFramebuffer.status().starts_with("Shader framebuffer regenerated");
        const std::string_view fullStatus = shaderFramebuffer.status();
        const std::size_t statusEnd = fullStatus.find('\n');
        const std::string statusSummary(fullStatus.substr(0, statusEnd));
        ImGui::TextColored(compileOk ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f) : ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", statusSummary.c_str());
        if (!compileOk && statusEnd != std::string_view::npos && ImGui::TreeNode("Compiler log"))
        {
            ImGui::TextWrapped("%s", shaderFramebuffer.status().c_str());
            ImGui::TreePop();
        }

        if (ImGui::Button("Refresh shader library")) { refreshShaderLibrary(); settings.ShaderPresetIndex = shaderPresetIndexById(settings.ShaderId); }
        ImGui::SameLine(); ImGui::TextDisabled("%zu shaders | %s", ShaderPresets.size(), shaderLibraryPath().string().c_str());
        const char* presetPreview = settings.ShaderPresetIndex == 0 ? "Custom / current" : ShaderPresets[settings.ShaderPresetIndex - 1].Name.c_str();
        const bool shaderMutexLocked = javascript.shaderMutexLocked();
        ImGui::SetNextItemWidth(230.0f);
        ImGui::BeginDisabled(shaderMutexLocked);
        if (ImGui::BeginCombo("Preset", presetPreview))
        {
            if (ImGui::Selectable("Custom / current", settings.ShaderPresetIndex == 0)) { settings.ShaderPresetIndex = 0; settings.ShaderId.clear(); }
            for (std::size_t i = 0; i < ShaderPresets.size(); ++i)
            {
                const bool selected = settings.ShaderPresetIndex == static_cast<int>(i + 1);
                if (ImGui::Selectable(ShaderPresets[i].Name.c_str(), selected))
                {
                    switchShaderPreset(shaderFramebuffer, shaderTransition, shaderEditor, vertexShaderSource, fragmentShaderSource, settings, static_cast<int>(i + 1), glfwGetTime(), settings.ShaderTransitionSeconds);
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Key-state uniforms", &settings.ShaderKeyStateUniforms);
        ImGui::SameLine();
        ImGui::TextDisabled("evdev -> uCapsLock %.0f   uScrollLock %.0f", settings.ShaderKeyStateUniforms && capsLockActive ? 1.0f : 0.0f, settings.ShaderKeyStateUniforms && scrollLockActive ? 1.0f : 0.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reactive presets also receive uKeyState[112] and uKeyEvents[16]. Key capture comes from Linux evdev and works while Quartz is unfocused.");
        if (shaderMutexLocked)
        {
            const std::string owner = javascript.shaderMutexOwnerDisplayName();
            ImGui::TextDisabled(ui::i18n::tr("shaders.mutexLocked"), owner.c_str());
        }
        ImGui::Checkbox("Caps Lock fixed-color LED", &settings.ShaderCapsLockColorEnabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(145.0f);
        ImGui::ColorEdit3("##ShaderCapsLockColorIDE", settings.ShaderCapsLockColor.data(), ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::Checkbox("Scroll Lock fixed-color LED", &settings.ShaderScrollLockColorEnabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(145.0f);
        ImGui::ColorEdit3("##ShaderScrollColorIDE", settings.ShaderScrollLockColor.data(), ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::TextDisabled("Ctrl+wheel/+/- zoom  |  Ctrl+0 reset  |  Ctrl+F find  |  Ctrl+Z/Y undo/redo");
        if (ImGui::CollapsingHeader("Material parameters", ImGuiTreeNodeFlags_DefaultOpen))
        {
            drawShaderMaterialEditor(shaderFramebuffer, 120.0f);
            ImGui::TextDisabled("OpenGL reflects active uniform names/types; @ui comments add editor hints such as ranges, defaults, labels and color pickers.");
        }
        ImGui::Separator();

        if (ImGui::BeginTabBar("ShaderStages"))
        {
            if (ImGui::BeginTabItem("Fragment shader"))
            {
                shaderEditor.ActiveStage = 0;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 310.0f);
                ImGui::InputText("##FragmentLoadPathIDE", fragmentLoadPath.data(), fragmentLoadPath.size());
                ImGui::SameLine();
                if (ImGui::Button("Load / import##fragment"))
                {
                    if (loadTextFile(fragmentLoadPath.data(), fragmentShaderSource))
                    {
                        std::string importedId, importError;
                        if (importShaderToLibrary(fragmentLoadPath.data(), importedId, importError)) { settings.ShaderId = importedId; settings.ShaderPresetIndex = shaderPresetIndexById(importedId); }
                        else { settings.ShaderPresetIndex = 0; settings.ShaderId.clear(); }
                        shaderEditor.Fragment.SetText(fragmentShaderSource.data());
                        saveTextFile(fragmentShaderPath(), fragmentShaderSource);
                        if (settings.ShaderRecompileOnChange) compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                    }
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Annotated files are copied into the persistent shader library. Example: // @shader id=\"terraria.eye.phase1\" label=\"Eye of Cthulhu - Phase 1\"");
                ImGui::SameLine();
                if (ImGui::Button("Default##fragment"))
                {
                    settings.ShaderPresetIndex = 1; settings.ShaderId = ShaderPresets.front().Id;
                    setShaderSource(fragmentShaderSource, ShaderPresets.front().FragmentSource);
                    shaderEditor.Fragment.SetText(fragmentShaderSource.data());
                    compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                    saveShaderSources(vertexShaderSource, fragmentShaderSource);
                }
                const float previewReserve = 280.0f;
                const float editorHeight = std::max(140.0f, ImGui::GetContentRegionAvail().y - previewReserve - ImGui::GetTextLineHeightWithSpacing());
                const bool changed = renderShaderTextEditor(shaderEditor.Fragment, "##FragmentEditor", ImVec2(-1.0f, editorHeight), shaderEditor, settings);
                const auto fragmentCursor = shaderEditor.Fragment.GetCurrentCursorPosition();
                ImGui::TextDisabled("Fragment  |  Ln %zu, Col %zu  |  %zu lines  |  GLSL", fragmentCursor.line + 1, fragmentCursor.index + 1, shaderEditor.Fragment.GetLineCount());
                if (changed)
                {
                    settings.ShaderPresetIndex = 0; settings.ShaderId.clear();
                    setShaderSource(fragmentShaderSource, shaderEditor.Fragment.GetText());
                    saveTextFile(fragmentShaderPath(), fragmentShaderSource);
                    if (settings.ShaderRecompileOnChange)
                        compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                }
                drawShaderLivePanel(usb, deviceState, keyboardInput, framebuffer, appCpuUsage, scrollLockActive, capsLockActive, settings);
                ImGui::Dummy(ImVec2(0.0f, 10.0f));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Vertex shader"))
            {
                shaderEditor.ActiveStage = 1;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 310.0f);
                ImGui::InputText("##VertexLoadPathIDE", vertexLoadPath.data(), vertexLoadPath.size());
                ImGui::SameLine();
                if (ImGui::Button("Load from file##vertex"))
                {
                    if (loadTextFile(vertexLoadPath.data(), vertexShaderSource))
                    {
                        shaderEditor.Vertex.SetText(vertexShaderSource.data());
                        saveTextFile(vertexShaderPath(), vertexShaderSource);
                        if (settings.ShaderRecompileOnChange)
                            compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Default##vertex"))
                {
                    setShaderSource(vertexShaderSource, DefaultVertexShaderSource);
                    shaderEditor.Vertex.SetText(vertexShaderSource.data());
                    compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                    saveShaderSources(vertexShaderSource, fragmentShaderSource);
                }
                const float previewReserve = 280.0f;
                const float editorHeight = std::max(140.0f, ImGui::GetContentRegionAvail().y - previewReserve - ImGui::GetTextLineHeightWithSpacing());
                const bool changed = renderShaderTextEditor(shaderEditor.Vertex, "##VertexEditor", ImVec2(-1.0f, editorHeight), shaderEditor, settings);
                const auto vertexCursor = shaderEditor.Vertex.GetCurrentCursorPosition();
                ImGui::TextDisabled("Vertex  |  Ln %zu, Col %zu  |  %zu lines  |  GLSL", vertexCursor.line + 1, vertexCursor.index + 1, shaderEditor.Vertex.GetLineCount());
                if (changed)
                {
                    setShaderSource(vertexShaderSource, shaderEditor.Vertex.GetText());
                    saveTextFile(vertexShaderPath(), vertexShaderSource);
                    if (settings.ShaderRecompileOnChange)
                        compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                }
                drawShaderLivePanel(usb, deviceState, keyboardInput, framebuffer, appCpuUsage, scrollLockActive, capsLockActive, settings);
                ImGui::Dummy(ImVec2(0.0f, 10.0f));
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    void drawPermanentHeader(VisualizerSettings& settings, const std::array<Color32, MatrixSize>& framebuffer)
    {
        static bool showKeyboardPreview = false;
        static const VisualizerSettings defaults{};
        const char* keyboardLabel = ui::i18n::tr("header.keyboard");
        const char* appearanceLabel = ui::i18n::tr("header.appearance");
        const char* terminateLabel = ui::i18n::tr("header.terminate");
        const char* byline = ui::i18n::tr("header.byline");
        const std::string themeText = std::string(ui::i18n::tr("header.theme")) + ": " + ui::themeName(static_cast<ui::Theme>(std::clamp(settings.UiTheme, 0, static_cast<int>(ui::Theme::Count) - 1)));
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float left = ImGui::GetWindowContentRegionMin().x;
        const float right = ImGui::GetWindowContentRegionMax().x;
        const float titleWidth = ImGui::CalcTextSize("Quartz K552X").x;
        const float bylineWidth = ImGui::CalcTextSize(byline).x;
        const float themeWidth = ImGui::CalcTextSize(themeText.c_str()).x;
        const float buttonsWidth = ImGui::CalcTextSize(keyboardLabel).x + ImGui::CalcTextSize(appearanceLabel).x + ImGui::CalcTextSize(terminateLabel).x + ImGui::GetStyle().FramePadding.x * 6.0f + spacing * 2.0f;
        const float buttonStart = right - buttonsWidth;
        const float themeStart = buttonStart - themeWidth - spacing * 2.0f;
        const float bylineStart = left + (right - left - bylineWidth) * 0.5f;

        ImGui::TextUnformatted("Quartz K552X");
        if (bylineStart > left + titleWidth + spacing * 2.0f && bylineStart + bylineWidth < themeStart - spacing * 2.0f)
        {
            ImGui::SameLine(bylineStart);
            ImGui::TextDisabled("%s", byline);
        }
        if (themeStart > ImGui::GetCursorPosX() + spacing)
        {
            ImGui::SameLine(themeStart);
            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", themeText.c_str());
        }
        if (ImGui::GetCursorPosX() < buttonStart) ImGui::SameLine(buttonStart);
        if (ImGui::SmallButton(keyboardLabel)) showKeyboardPreview = !showKeyboardPreview;
        ImGui::SameLine();
        if (ImGui::SmallButton(appearanceLabel)) ImGui::OpenPopup("##QuartzAppearancePopup");
        ImGui::SameLine();
        if (ImGui::SmallButton(terminateLabel)) glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ui::i18n::tr("header.terminateTooltip"));

        if (ImGui::BeginPopup("##QuartzAppearancePopup"))
        {
            if (ImGui::BeginTabBar("QuartzSettingsTabs"))
            {
                if (ImGui::BeginTabItem(ui::i18n::tr("header.appearance")))
                {
                    ui::drawThemeSelector(settings);
                    ui::i18n::drawLanguageSelector();
                    ImGui::Separator();
                    float silhouettePercent = settings.UiSilhouetteOpacity * 100.0f;
                    ImGui::SetNextItemWidth(220.0f);
                    if (ImGui::SliderFloat(ui::i18n::tr("appearance.silhouetteOpacity"), &silhouettePercent, 0.0f, 300.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) { settings.UiSilhouetteOpacity = silhouettePercent / 100.0f; ui::saveThemePreferences(settings); }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ui::i18n::tr("appearance.silhouetteOpacityTooltip"));
                    ImGui::SetNextItemWidth(220.0f);
                    ImGui::SliderFloat(ui::i18n::tr("appearance.globalBrightness"), &settings.GlobalBrightness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                    ImGui::SetNextItemWidth(220.0f);
                    ImGui::SliderFloat(ui::i18n::tr("appearance.previewInterpolation"), &settings.LiveOutputInterpolation, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ui::i18n::tr("appearance.previewInterpolationTooltip"));
                    if (ImGui::Button(ui::i18n::tr("appearance.reset")))
                    {
                        settings.UiTheme = defaults.UiTheme;
                        settings.SuspiciousColorThemes = defaults.SuspiciousColorThemes;
                        settings.UiCornerRounding = defaults.UiCornerRounding;
                        settings.UiSilhouetteOpacity = defaults.UiSilhouetteOpacity;
                        settings.GlobalBrightness = defaults.GlobalBrightness;
                        settings.LiveOutputInterpolation = defaults.LiveOutputInterpolation;
                        ui::applyTheme(settings.UiTheme);
                        ui::applyCornerRounding(settings.UiCornerRounding);
                        ui::saveThemePreferences(settings);
                    }
                    ImGui::TextDisabled("%s", settingsPath().string().c_str());
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ui::i18n::tr("header.configuration"))) { ui::drawConfigurationSettings(); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
            ImGui::EndPopup();
        }
        ImGui::Separator();

        if (showKeyboardPreview)
        {
            std::string title = std::string(ui::i18n::tr("keyboardPreview.title")) + "###QuartzKeyboardPreview";
            ImGui::SetNextWindowSize(ImVec2(560.0f, 270.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f, 150.0f), ImVec2(FLT_MAX, FLT_MAX));
            if (ImGui::Begin(title.c_str(), &showKeyboardPreview, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            {
                constexpr float PreviewAspect = 19.0f / 6.35f;
                const ImVec2 available = ImGui::GetContentRegionAvail();
                const float widthFromHeight = available.y > 1.0f ? available.y * PreviewAspect : available.x;
                const float previewWidth = std::max(140.0f, std::min(available.x, widthFromHeight));
                const float offset = std::max(0.0f, (available.x - previewWidth) * 0.5f);
                if (offset > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
                drawFramebufferPreview(framebuffer, 1.0f, previewWidth, settings.LiveOutputInterpolation);
            }
            ImGui::End();
        }
    }

    void drawUi(RawUSB& usb, AudioSpectrum& audio, MediaColorProvider& mediaColor, const EvdevKeyboard& keyboardInput, ShaderFramebuffer& shaderFramebuffer, ShaderTransitionState& shaderTransition, ShaderEditorState& shaderEditor, ui::PageManager& pageManager, std::array<char, ShaderSourceCapacity>& vertexShaderSource, std::array<char, ShaderSourceCapacity>& fragmentShaderSource, std::array<char, ShaderPathCapacity>& vertexLoadPath, std::array<char, ShaderPathCapacity>& fragmentLoadPath, VisualizerSettings& settings, const std::array<float, FFTSize>& analysisBands, const std::array<float, Columns>& mappedBands, const std::array<float, Columns>& smoothedBands, const std::array<Color32, MatrixSize>& framebuffer, SharedDeviceState& deviceState, RuntimeBindingEngine& runtimeBindings, JavaScriptRuntime& javascript, RuntimeTelemetry& runtimeTelemetry, const AutoGainState& autoGain, const AudioLevelSnapshot& audioLevel, const ReactiveKeyState& reactiveKeys, const RuntimeInputAnalytics& inputAnalytics, const RuntimeRGBAnalytics& rgbAnalytics, std::uint64_t sentFrames, std::uint64_t droppedFrames, const float appCpuUsage, const bool scrollLockActive, const bool capsLockActive)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("Quartz K552X Visualizer", nullptr, windowFlags);
        drawPermanentHeader(settings, framebuffer);
        ImGui::BeginChild("MainScrollableBody", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);

        ui::PageContext context{usb, audio, mediaColor, keyboardInput, shaderFramebuffer, shaderTransition, shaderEditor, vertexShaderSource, fragmentShaderSource, vertexLoadPath, fragmentLoadPath, settings, analysisBands, mappedBands, smoothedBands, framebuffer, deviceState, runtimeBindings, javascript, runtimeTelemetry, autoGain, audioLevel, reactiveKeys, inputAnalytics, rgbAnalytics, sentFrames, droppedFrames, appCpuUsage, scrollLockActive, capsLockActive};
        if (pageManager.hasStandalonePage())
        {
            pageManager.render(context);
            ImGui::EndChild();
            ImGui::End();
            return;
        }
        pageManager.render(context);
        ImGui::EndChild();
        ImGui::End();
    }

}
