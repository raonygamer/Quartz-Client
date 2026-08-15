#include "quartz/client/Model.hpp"

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

    void drawShaderEditorPage(RawUSB& usb, SharedDeviceState& deviceState, const EvdevKeyboard& keyboardInput, ShaderFramebuffer& shaderFramebuffer, ShaderTransitionState& shaderTransition, ShaderEditorState& shaderEditor, ViewPage& page, std::array<char, ShaderSourceCapacity>& vertexShaderSource, std::array<char, ShaderSourceCapacity>& fragmentShaderSource, std::array<char, ShaderPathCapacity>& vertexLoadPath, std::array<char, ShaderPathCapacity>& fragmentLoadPath, VisualizerSettings& settings, const std::array<Color32, MatrixSize>& framebuffer, const float appCpuUsage, const bool scrollLockActive, const bool capsLockActive)
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
        ImGui::SetNextItemWidth(230.0f);
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
        ImGui::SameLine();
        ImGui::Checkbox("Key-state uniforms", &settings.ShaderKeyStateUniforms);
        ImGui::SameLine();
        ImGui::TextDisabled("evdev -> uCapsLock %.0f   uScrollLock %.0f", settings.ShaderKeyStateUniforms && capsLockActive ? 1.0f : 0.0f, settings.ShaderKeyStateUniforms && scrollLockActive ? 1.0f : 0.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reactive presets also receive uKeyState[112] and uKeyEvents[16]. Key capture comes from Linux evdev and works while Quartz is unfocused.");
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

    void hideWindow(GLFWwindow* window)
    {
        if (window) glfwHideWindow(window);
    }

    void restoreWindow(GLFWwindow* window)
    {
        if (!window) return;
        glfwShowWindow(window);
        glfwRestoreWindow(window);
        glfwFocusWindow(window);
    }

    void drawPermanentHeader(RawUSB& usb)
    {
        ImGui::Text("Quartz K552X  |  USB %s  |  FW %s  |  %04X:%04X", usb.isConnected() ? "connected" : "disconnected", FirmwareVersion, VendorId, ProductId);
        ImGui::SameLine();
        if (ImGui::SmallButton("Terminate")) glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Exit Quartz completely. The normal window close button only hides the window.");
        const char* credits = "Made by Raony Reis, not affiliated with Redragon";
        const float creditWidth = ImGui::CalcTextSize(credits).x;
        const float right = ImGui::GetWindowContentRegionMax().x;
        if (ImGui::GetCursorPosX() + creditWidth + 16.0f < right) ImGui::SameLine(right - creditWidth);
        ImGui::TextDisabled("%s", credits);
        ImGui::Separator();
    }

    void drawUi(RawUSB& usb, AudioSpectrum& audio, MediaColorProvider& mediaColor, const EvdevKeyboard& keyboardInput, ShaderFramebuffer& shaderFramebuffer, ShaderTransitionState& shaderTransition, ShaderEditorState& shaderEditor, ViewPage& page, std::array<char, ShaderSourceCapacity>& vertexShaderSource, std::array<char, ShaderSourceCapacity>& fragmentShaderSource, std::array<char, ShaderPathCapacity>& vertexLoadPath, std::array<char, ShaderPathCapacity>& fragmentLoadPath, VisualizerSettings& settings, const std::array<float, FFTSize>& analysisBands, const std::array<float, Columns>& mappedBands, const std::array<float, Columns>& smoothedBands, const std::array<Color32, MatrixSize>& framebuffer, SharedDeviceState& deviceState, RuntimeBindingEngine& runtimeBindings, RuntimeTelemetry& runtimeTelemetry, const AutoGainState& autoGain, const AudioLevelSnapshot& audioLevel, const ReactiveKeyState& reactiveKeys, const RuntimeInputAnalytics& inputAnalytics, const RuntimeRGBAnalytics& rgbAnalytics, std::uint64_t sentFrames, std::uint64_t droppedFrames, const float appCpuUsage, const bool scrollLockActive, const bool capsLockActive)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("Quartz K552X Visualizer", nullptr, windowFlags);
        drawPermanentHeader(usb);
        ImGui::BeginChild("MainScrollableBody", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
        if (page == ViewPage::ShaderEditor)
        {
            drawShaderEditorPage(usb, deviceState, keyboardInput, shaderFramebuffer, shaderTransition, shaderEditor, page, vertexShaderSource, fragmentShaderSource, vertexLoadPath, fragmentLoadPath, settings, framebuffer, appCpuUsage, scrollLockActive, capsLockActive);
            ImGui::EndChild();
            ImGui::End();
            return;
        }
        static const VisualizerSettings defaults{};
        const bool connected = usb.isConnected();
        ImGui::Text("USB: %s", connected ? "connected" : "disconnected");
        ImGui::SameLine();
        if (!connected && ImGui::Button("Connect"))
            usb.connect();
        if (connected)
        {
            ImGui::SameLine();
            if (ImGui::Button("Disconnect"))
            {
                settings.AutoReconnect = false;
                usb.disconnect();
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%04X:%04X", VendorId, ProductId);
        ImGui::SameLine();
        ImGui::Checkbox("Auto reconnect", &settings.AutoReconnect);
        defaultButton("AutoReconnect", settings.AutoReconnect, defaults.AutoReconnect);
        ImGui::SliderFloat("Global brightness", &settings.GlobalBrightness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        defaultButton("GlobalBrightness", settings.GlobalBrightness, defaults.GlobalBrightness);
        ImGui::SliderFloat("Live output interpolation", &settings.LiveOutputInterpolation, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview-only spatial color mixing. 0 = exact framebuffer, 1 = strongest neighboring-key blend. It never changes data sent over USB.");
        defaultButton("LiveOutputInterpolation", settings.LiveOutputInterpolation, defaults.LiveOutputInterpolation);
        ImGui::SameLine();
        if (ImGui::Button("Reset all settings"))
        {
            const bool restartAudio = std::strcmp(settings.AudioSource, defaults.AudioSource) != 0;
            settings = defaults;
            if (restartAudio) audio.start(settings.AudioSource);
        }
        if (!connected && usb.lastError() != LIBUSB_SUCCESS)
            ImGui::TextDisabled("libusb: %s", libusb_error_name(usb.lastError()));
        ImGui::Text("Frames sent: %llu   busy/dropped: %llu", static_cast<unsigned long long>(sentFrames), static_cast<unsigned long long>(droppedFrames));
        const std::string configPath = settingsPath().string();
        ImGui::TextDisabled("Settings: %s", configPath.c_str());
        ImGui::TextDisabled("%s", g_SettingsStatus.c_str());

        if (ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyResizeDown | ImGuiTabBarFlags_TabListPopupButton))
        {
            if (ImGui::BeginTabItem("Visualizer"))
            {
                ImGui::Checkbox("Enabled", &settings.Enabled);
                defaultButton("Enabled", settings.Enabled, defaults.Enabled);
                ImGui::Checkbox("Send framebuffer", &settings.SendFramebuffer);
                defaultButton("SendFramebuffer", settings.SendFramebuffer, defaults.SendFramebuffer);
                ImGui::SliderInt("Frame rate", &settings.FrameRate, 30, 500, "%d Hz");
                defaultButton("FrameRate", settings.FrameRate, defaults.FrameRate);
                ImGui::Checkbox("Yield main loop", &settings.LimitMainLoop);
                defaultButton("LimitMainLoop", settings.LimitMainLoop, defaults.LimitMainLoop);
                ImGui::SliderInt("Analysis bands", &settings.AnalysisBandCount, 32, static_cast<int>(FFTSize));
                defaultButton("AnalysisBandCount", settings.AnalysisBandCount, defaults.AnalysisBandCount);
                ImGui::Checkbox("Automatic overall gain", &settings.AutomaticOverallGain);
                defaultButton("AutomaticOverallGain", settings.AutomaticOverallGain, defaults.AutomaticOverallGain);
                if (settings.AutomaticOverallGain)
                {
                    ImGui::SliderFloat("Baseline gain", &settings.AutoGainBaseline, 0.10f, 6.00f, "%.2fx");
                    defaultButton("AutoGainBaseline", settings.AutoGainBaseline, defaults.AutoGainBaseline);
                    ImGui::Text("Auto gain: RMS %.4f  learned %.4f  correction %.3fx  effective %.3fx", audioLevel.Rms, autoGain.LongTermRms, autoGain.Correction, autoGain.EffectiveGain);
                    if (ImGui::TreeNode("Automatic gain tuning"))
                    {
                        ImGui::SliderFloat("Target RMS", &settings.AutoGainTargetRms, 0.01f, 0.50f, "%.3f");
                        defaultButton("AutoGainTargetRms", settings.AutoGainTargetRms, defaults.AutoGainTargetRms);
                        ImGui::SliderFloat("Adaptation", &settings.AutoGainAdaptation, 0.02f, 2.0f, "%.2f /s", ImGuiSliderFlags_Logarithmic);
                        defaultButton("AutoGainAdaptation", settings.AutoGainAdaptation, defaults.AutoGainAdaptation);
                        ImGui::SliderFloat("Min correction", &settings.AutoGainMinCorrection, 0.10f, 1.0f, "%.2fx");
                        defaultButton("AutoGainMinCorrection", settings.AutoGainMinCorrection, defaults.AutoGainMinCorrection);
                        ImGui::SliderFloat("Max correction", &settings.AutoGainMaxCorrection, 1.0f, 8.0f, "%.2fx");
                        defaultButton("AutoGainMaxCorrection", settings.AutoGainMaxCorrection, defaults.AutoGainMaxCorrection);
                        ImGui::SliderFloat("Silence gate", &settings.AutoGainSilenceGate, 0.0f, 0.05f, "%.4f");
                        defaultButton("AutoGainSilenceGate", settings.AutoGainSilenceGate, defaults.AutoGainSilenceGate);
                        ImGui::TextDisabled("Baseline is the normal gain; the long-term loudness estimate only applies a slow bounded correction around it.");
                        ImGui::TreePop();
                    }
                }
                else
                {
                    ImGui::SliderFloat("Overall gain", &settings.OverallGain, 0.10f, 4.00f, "%.2fx");
                    defaultButton("OverallGain", settings.OverallGain, defaults.OverallGain);
                }
                ImGui::SliderFloat("Attack", &settings.AttackSpeed, 0.1f, 80.0f, "%.2f");
                defaultButton("AttackSpeed", settings.AttackSpeed, defaults.AttackSpeed);
                ImGui::SliderFloat("Release", &settings.ReleaseSpeed, 0.1f, 80.0f, "%.2f");
                defaultButton("ReleaseSpeed", settings.ReleaseSpeed, defaults.ReleaseSpeed);
                ImGui::SliderFloat("Feather rows", &settings.FeatherRows, 0.10f, 7.0f, "%.2f");
                defaultButton("FeatherRows", settings.FeatherRows, defaults.FeatherRows);
                ImGui::SliderFloat("Saturation", &settings.Saturation, 0.0f, 4.0f, "%.2fx");
                defaultButton("Saturation", settings.Saturation, defaults.Saturation);
                ImGui::SeparatorText("Spectrum mapping");
                ImGui::SliderInt("Bass columns", &settings.BassColumns, 2, 8);
                defaultButton("BassColumns", settings.BassColumns, defaults.BassColumns);
                ImGui::SliderInt("Bass end band", &settings.BassEndBand, 0, std::max(1, settings.AnalysisBandCount - 1));
                defaultButton("BassEndBand", settings.BassEndBand, defaults.BassEndBand);
                ImGui::SliderFloat("Bass activation", &settings.BassActivationThreshold, 0.0f, 0.99f, "%.2f");
                defaultButton("BassActivationThreshold", settings.BassActivationThreshold, defaults.BassActivationThreshold);
                ImGui::SliderFloat("Bass max boost", &settings.BassMaxBoost, 1.0f, 4.0f, "%.2fx");
                defaultButton("BassMaxBoost", settings.BassMaxBoost, defaults.BassMaxBoost);
                if (ImGui::TreeNode("Per-column gain"))
                {
                    for (std::size_t i = 0; i < Columns; ++i)
                    {
                        char label[32];
                        std::snprintf(label, sizeof(label), "Column %zu", i);
                        ImGui::SliderFloat(label, &settings.ColumnGain[i], 0.0f, 2.5f, "%.2f");
                        char resetId[32];
                        std::snprintf(resetId, sizeof(resetId), "ColumnGain%zu", i);
                        defaultButton(resetId, settings.ColumnGain[i], defaults.ColumnGain[i]);
                    }
                    ImGui::TreePop();
                }
                ImGui::SeparatorText("Color");
                const char* modes[] = {"RGB wave", "Solid", "Shader (full framebuffer)"};
                ImGui::Combo("Base color", &settings.BaseColorMode, modes, 3);
                defaultButton("BaseColorMode", settings.BaseColorMode, defaults.BaseColorMode);
                if (settings.BaseColorMode == 0)
                {
                    ImGui::SliderFloat("Wave speed", &settings.WaveSpeed, -2.0f, 2.0f, "%.3f");
                    defaultButton("WaveSpeed", settings.WaveSpeed, defaults.WaveSpeed);
                }
                else if (settings.BaseColorMode == 1)
                {
                    ImGui::ColorEdit3("Solid color", settings.SolidColor.data());
                    defaultButton("SolidColor", settings.SolidColor, defaults.SolidColor);
                }
                else
                {
                    const char* downsampleModes[] = {"Average logical cell (smooth)", "Average center 4x4", "Center pixel (exact)"};
                    int requestedSize[2]{settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight};
                    if (ImGui::InputInt2("Shader framebuffer size", requestedSize))
                    {
                        settings.ShaderFramebufferWidth = std::clamp(requestedSize[0], static_cast<int>(Columns), MaxShaderDimension);
                        settings.ShaderFramebufferHeight = std::clamp(requestedSize[1], static_cast<int>(Rows), MaxShaderDimension);
                    }
                    defaultButton("ShaderFramebufferWidth", settings.ShaderFramebufferWidth, defaults.ShaderFramebufferWidth);
                    defaultButton("ShaderFramebufferHeight", settings.ShaderFramebufferHeight, defaults.ShaderFramebufferHeight);
                    ImGui::SameLine();
                    if (ImGui::Button("Regenerate framebuffer"))
                        shaderFramebuffer.regenerate(settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight);
                    ImGui::Text("Active shader surface: %dx%d -> 16x7 QRPC framebuffer", shaderFramebuffer.width(), shaderFramebuffer.height());
                    ImGui::Combo("Framebuffer approximation", &settings.ShaderDownsampleMode, downsampleModes, 3);
                    defaultButton("ShaderDownsampleMode", settings.ShaderDownsampleMode, defaults.ShaderDownsampleMode);
                    ImGui::SliderFloat("Shader crossfade", &settings.ShaderTransitionSeconds, 0.0f, 5.0f, "%.2f s");
                    defaultButton("ShaderTransitionSeconds", settings.ShaderTransitionSeconds, defaults.ShaderTransitionSeconds);
                    ImGui::SameLine();
                    ImGui::TextDisabled(shaderTransition.Active ? "transition active" : "manual/control shader switch transition");
                    ImGui::Checkbox("Recompile on text change", &settings.ShaderRecompileOnChange);
                    defaultButton("ShaderRecompileOnChange", settings.ShaderRecompileOnChange, defaults.ShaderRecompileOnChange);
                    ImGui::Checkbox("Key-state shader uniforms", &settings.ShaderKeyStateUniforms);
                    defaultButton("ShaderKeyStateUniforms", settings.ShaderKeyStateUniforms, defaults.ShaderKeyStateUniforms);
                    ImGui::SameLine();
                    ImGui::TextDisabled("evdev -> uCapsLock %.0f  uScrollLock %.0f", settings.ShaderKeyStateUniforms && capsLockActive ? 1.0f : 0.0f, settings.ShaderKeyStateUniforms && scrollLockActive ? 1.0f : 0.0f);
                    ImGui::Checkbox("Caps Lock uses fixed shader color", &settings.ShaderCapsLockColorEnabled);
                    defaultButton("ShaderCapsLockColorEnabled", settings.ShaderCapsLockColorEnabled, defaults.ShaderCapsLockColorEnabled);
                    ImGui::ColorEdit3("Caps Lock shader color", settings.ShaderCapsLockColor.data());
                    defaultButton("ShaderCapsLockColor", settings.ShaderCapsLockColor, defaults.ShaderCapsLockColor);
                    ImGui::Checkbox("Scroll Lock uses fixed shader color", &settings.ShaderScrollLockColorEnabled);
                    defaultButton("ShaderScrollLockColorEnabled", settings.ShaderScrollLockColorEnabled, defaults.ShaderScrollLockColorEnabled);
                    ImGui::ColorEdit3("Scroll Lock shader color", settings.ShaderScrollLockColor.data());
                    defaultButton("ShaderScrollLockColor", settings.ShaderScrollLockColor, defaults.ShaderScrollLockColor);

                    const char* presetPreview = settings.ShaderPresetIndex == 0 ? "Custom / current" : ShaderPresets[settings.ShaderPresetIndex - 1].Name.c_str();
                    if (ImGui::BeginCombo("Shader preset", presetPreview))
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
                    if (defaultButton("ShaderPresetIndex", settings.ShaderPresetIndex, defaults.ShaderPresetIndex))
                    {
                        const int presetIndex = std::clamp(settings.ShaderPresetIndex, 1, static_cast<int>(ShaderPresets.size()));
                        switchShaderPreset(shaderFramebuffer, shaderTransition, shaderEditor, vertexShaderSource, fragmentShaderSource, settings, presetIndex, glfwGetTime(), settings.ShaderTransitionSeconds);
                    }
                    ImGui::SeparatorText("Material parameters");
                    drawShaderMaterialEditor(shaderFramebuffer, 170.0f);
                    ImGui::TextDisabled("Arbitrary active float/int/bool/vector uniforms are reflected automatically. Engine uniforms stay reserved unless explicitly annotated with // @ui.");
                    ImGui::TextDisabled("Uniforms: uTime, uResolution, uBands[16], uMediaColor, uMediaAmount, uSolidColor, uWaveSpeed, uFeatherRows, uSaturation, uForceFullRow, uFullRow");
                    if (ImGui::Button("Compile shaders"))
                    {
                        if (compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource))
                            saveShaderSources(vertexShaderSource, fragmentShaderSource);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Default shaders"))
                    {
                        settings.ShaderPresetIndex = 1; settings.ShaderId = ShaderPresets.front().Id;
                        setShaderSource(vertexShaderSource, DefaultVertexShaderSource);
                        setShaderSource(fragmentShaderSource, ShaderPresets.front().FragmentSource);
                        if (shaderEditor.Initialized) { shaderEditor.Vertex.SetText(vertexShaderSource.data()); shaderEditor.Fragment.SetText(fragmentShaderSource.data()); }
                        compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                        saveShaderSources(vertexShaderSource, fragmentShaderSource);
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", shaderFramebuffer.status().c_str());

                    if (ImGui::Button("Edit shaders..."))
                    {
                        initializeShaderEditors(shaderEditor, vertexShaderSource.data(), fragmentShaderSource.data());
                        shaderEditor.Vertex.SetText(vertexShaderSource.data());
                        shaderEditor.Fragment.SetText(fragmentShaderSource.data());
                        updateShaderDiagnostics(shaderEditor, shaderFramebuffer.status());
                        page = ViewPage::ShaderEditor;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("GLSL IDE: syntax highlighting, line numbers, minimap and compiler diagnostics");
                }
                ImGui::Checkbox("Use MPRIS artwork color", &settings.MediaArtworkColor);
                defaultButton("MediaArtworkColor", settings.MediaArtworkColor, defaults.MediaArtworkColor);
                ImGui::SliderFloat("Artwork blend", &settings.MediaColorBlend, 0.0f, 1.0f, "%.2f");
                defaultButton("MediaColorBlend", settings.MediaColorBlend, defaults.MediaColorBlend);
                ImGui::SliderFloat("Color transition", &settings.ColorTransitionSpeed, 0.1f, 12.0f, "%.2f");
                defaultButton("ColorTransitionSpeed", settings.ColorTransitionSpeed, defaults.ColorTransitionSpeed);
                ImGui::Checkbox("Force one row full", &settings.ForceFullRow);
                defaultButton("ForceFullRow", settings.ForceFullRow, defaults.ForceFullRow);
                if (settings.ForceFullRow)
                {
                    ImGui::SliderInt("Full row", &settings.FullRow, 0, static_cast<int>(Rows) - 2);
                    defaultButton("FullRow", settings.FullRow, defaults.FullRow);
                }
                const auto mediaTarget = mediaColor.targetColor();
                const auto mediaStatus = mediaColor.status();
                const auto mediaTitle = mediaColor.mediaTitle();
                ImGui::Text("Media: %s", mediaStatus.c_str());
                if (!mediaTitle.empty())
                    ImGui::TextWrapped("%s", mediaTitle.c_str());
                if (mediaTarget)
                {
                    const float color[4] = {mediaTarget->R / 255.0f, mediaTarget->G / 255.0f, mediaTarget->B / 255.0f, 1.0f};
                    ImGui::ColorButton("Artwork color", ImVec4(color[0], color[1], color[2], color[3]), ImGuiColorEditFlags_NoTooltip, ImVec2(42, 22));
                }
                if (!MediaColorProvider::imageDecoderAvailable())
                    ImGui::TextDisabled("Artwork extraction disabled: stb_image.h was not found at compile time.");
                ImGui::SeparatorText("Audio");
                static std::vector<AudioSourceInfo> audioSources = enumerateAudioSources();
                const auto selectedSource = std::ranges::find_if(audioSources, [&](const auto& source) { return source.Name == settings.AudioSource; });
                const char* sourcePreview = selectedSource != audioSources.end() ? selectedSource->Description.c_str() : settings.AudioSource;
                if (ImGui::BeginCombo("Capture device", sourcePreview))
                {
                    for (const auto& source : audioSources)
                    {
                        const bool selected = source.Name == settings.AudioSource;
                        const std::string label = source.Description == source.Name ? source.Name : source.Description + "##" + source.Name;
                        if (ImGui::Selectable(label.c_str(), selected))
                        {
                            std::snprintf(settings.AudioSource, sizeof(settings.AudioSource), "%s", source.Name.c_str());
                            audio.start(settings.AudioSource);
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                        if (ImGui::IsItemHovered() && source.Description != source.Name)
                            ImGui::SetTooltip("%s", source.Name.c_str());
                    }
                    ImGui::EndCombo();
                }
                defaultAudioSourceButton("AudioSource", settings, defaults, audio);
                ImGui::SameLine();
                if (ImGui::Button("Refresh devices"))
                    audioSources = enumerateAudioSources();
                if (audioSources.empty())
                    ImGui::TextDisabled("No Pulse/PipeWire sources found (pactl).");
                ImGui::InputText("Source name", settings.AudioSource, sizeof(settings.AudioSource));
                defaultAudioSourceButton("AudioSourceText", settings, defaults, audio);
                ImGui::SameLine();
                if (ImGui::Button("Restart audio"))
                    audio.start(settings.AudioSource);
                ImGui::Text("Capture: %s", audio.isRunning() ? "running" : "stopped");
                if (!audio.error().empty())
                    ImGui::TextDisabled("%s", audio.error().c_str());
                ImGui::SliderFloat("Min frequency", &settings.MinFrequency, 20.0f, 1000.0f, "%.0f Hz");
                defaultButton("MinFrequency", settings.MinFrequency, defaults.MinFrequency);
                ImGui::SliderFloat("Max frequency", &settings.MaxFrequency, 1000.0f, 22000.0f, "%.0f Hz");
                defaultButton("MaxFrequency", settings.MaxFrequency, defaults.MaxFrequency);
                ImGui::SliderFloat("Min dB", &settings.MinDb, -120.0f, -20.0f, "%.1f dB");
                defaultButton("MinDb", settings.MinDb, defaults.MinDb);
                ImGui::SliderFloat("Max dB", &settings.MaxDb, -40.0f, 0.0f, "%.1f dB");
                defaultButton("MaxDb", settings.MaxDb, defaults.MaxDb);
                ImGui::SliderFloat("MPRIS poll", &settings.MediaPollInterval, 0.10f, 3.0f, "%.2f s");
                defaultButton("MediaPollInterval", settings.MediaPollInterval, defaults.MediaPollInterval);
                ImGui::SliderFloat("Stats poll", &settings.StatisticsInterval, 0.05f, 2.0f, "%.2f s");
                defaultButton("StatisticsInterval", settings.StatisticsInterval, defaults.StatisticsInterval);
                if (ImGui::Button("Black out") && connected)
                {
                    std::array<Color32, MatrixSize> black{};
                    sendFramebuffer(usb, black);
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Spectrum"))
            {
                ImGui::Checkbox("Analysis graph", &settings.ShowAnalysisSpectrum);
                defaultButton("ShowAnalysisSpectrum", settings.ShowAnalysisSpectrum, defaults.ShowAnalysisSpectrum);
                ImGui::Checkbox("Mapped graph", &settings.ShowMappedSpectrum);
                defaultButton("ShowMappedSpectrum", settings.ShowMappedSpectrum, defaults.ShowMappedSpectrum);
                ImGui::Checkbox("Framebuffer preview", &settings.ShowFramebuffer);
                defaultButton("ShowFramebuffer", settings.ShowFramebuffer, defaults.ShowFramebuffer);
                if (settings.ShowAnalysisSpectrum)
                {
                    ImGui::TextUnformatted("FFT / log-frequency analysis");
                    ImGui::PlotLines("##analysis", analysisBands.data(), settings.AnalysisBandCount, 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 150.0f));
                }
                if (settings.ShowMappedSpectrum)
                {
                    ImGui::TextUnformatted("Mapped 16 columns");
                    ImGui::PlotHistogram("##mapped", mappedBands.data(), static_cast<int>(Columns), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 110.0f));
                    ImGui::TextUnformatted("Smoothed output");
                    ImGui::PlotHistogram("##smoothed", smoothedBands.data(), static_cast<int>(Columns), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 110.0f));
                }
                if (settings.ShowFramebuffer)
                {
                    ImGui::TextUnformatted("Keyboard framebuffer");
                    drawFramebufferPreview(framebuffer, 0.55f, 532.0f, settings.LiveOutputInterpolation);
                    ImGui::TextDisabled("Preview interpolation %.0f%% (visual only)", settings.LiveOutputInterpolation * 100.0f);
                }
                ImGui::EndTabItem();
            }

            PerformanceSnapshot performance;
            MatrixTimingProbeResult<ActiveProbeRows> timingProbe;
            bool hasPerformance;
            bool hasTimingProbe;
            std::uint64_t receivedPackets;
            {
                std::lock_guard lock(deviceState.Mutex);
                performance = deviceState.Performance;
                timingProbe = deviceState.TimingProbe;
                hasPerformance = deviceState.HasPerformance;
                hasTimingProbe = deviceState.HasTimingProbe;
                receivedPackets = deviceState.ReceivedPackets;
            }

            if (ImGui::BeginTabItem("Device"))
            {
                const std::size_t framebufferPayloadBytes = sizeof(FramebufferSetPayload<MatrixSize>);
                const std::size_t framebufferPacketBytes = sizeof(PacketHeader) + framebufferPayloadBytes;
                const double configuredTxKiB = settings.Enabled && settings.SendFramebuffer ? framebufferPacketBytes * static_cast<double>(settings.FrameRate) / 1024.0 : 0.0;
                const char* outputMode = settings.BaseColorMode == 0 ? "RGB wave" : settings.BaseColorMode == 1 ? "Solid" : "Shader framebuffer";
                const char* shaderName = settings.ShaderPresetIndex == 0 ? "Custom / current" : ShaderPresets[static_cast<std::size_t>(settings.ShaderPresetIndex - 1)].Name.c_str();

                ImGui::SeparatorText("Identity");
                ImGui::Text("Product: %s", connected ? (usb.deviceName().empty() ? "Quartz K552X" : usb.deviceName().c_str()) : "Disconnected");
                ImGui::Text("Firmware: %s", FirmwareVersion);
                ImGui::Text("VID:PID: %04X:%04X", VendorId, ProductId);
                ImGui::Text("QRPC protocol: v%u   interface %d   OUT 0x%02X   IN 0x%02X", static_cast<unsigned>(ProtocolVersion), RPCInterfaceNumber, static_cast<unsigned>(RPCOutEndpoint), static_cast<unsigned>(RPCInEndpoint));
                ImGui::Text("Packet header: %zu B   framebuffer payload: %zu B   full frame packet: %zu B", sizeof(PacketHeader), framebufferPayloadBytes, framebufferPacketBytes);
                ImGui::Text("Logical framebuffer: %zux%zu = %zu cells   active RGB area: %zux%zu", Columns, Rows, MatrixSize, Columns, ActiveProbeRows);

                ImGui::SeparatorText("Session / output");
                ImGui::Text("Client uptime: %.1f s   App CPU: %.2f%%", ImGui::GetTime(), appCpuUsage);
                ImGui::Text("TX frames: %llu   dropped/busy: %llu   RX packets: %llu", static_cast<unsigned long long>(sentFrames), static_cast<unsigned long long>(droppedFrames), static_cast<unsigned long long>(receivedPackets));
                ImGui::Text("Output mode: %s   target: %d Hz   estimated framebuffer TX: %.1f KiB/s", outputMode, settings.FrameRate, configuredTxKiB);
                if (settings.BaseColorMode == 2) ImGui::Text("Shader: %s   render target: %dx%d   downsample mode: %d   material params: %zu", shaderName, settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight, settings.ShaderDownsampleMode, shaderFramebuffer.materialParameters().size());
                ImGui::Text("Brightness: %.0f%%   live-preview interpolation: %.0f%%", settings.GlobalBrightness * 100.0f, settings.LiveOutputInterpolation * 100.0f);

                ImGui::SeparatorText("Host input / window");
                ImGui::Text("evdev: %s", keyboardInput.connected() ? "connected" : "disconnected");
                if (!keyboardInput.deviceName().empty()) ImGui::Text("Input device: %s", keyboardInput.deviceName().c_str());
                ImGui::TextWrapped("%s", keyboardInput.status().c_str());
                ImGui::Text("Caps Lock: %s   Scroll Lock: %s", capsLockActive ? "on" : "off", scrollLockActive ? "on" : "off");
                ImGui::TextDisabled("Closing the GLFW window hides it. Ctrl + Alt + Shift + Q restores it globally. Use Terminate in the permanent header to actually exit.");

                ImGui::SeparatorText("Runtime files");
                const std::string configFile = settingsPath().string();
                const std::string vertexFile = vertexShaderPath().string();
                const std::string fragmentFile = fragmentShaderPath().string();
                const std::string materialFile = shaderMaterialPath().string();
                const std::string bindingsFile = runtimeBindings.path().string();
                ImGui::TextWrapped("Settings: %s", configFile.c_str());
                ImGui::TextWrapped("Vertex shader: %s", vertexFile.c_str());
                ImGui::TextWrapped("Fragment shader: %s", fragmentFile.c_str());
                ImGui::TextWrapped("Material parameters: %s", materialFile.c_str());
                ImGui::TextWrapped("Runtime bindings: %s", bindingsFile.c_str());
                ImGui::EndTabItem();
            }


            if (ImGui::BeginTabItem("RE / Bindings"))
            {
                drawRuntimeBindingsPage(runtimeBindings, shaderFramebuffer);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("QRPC Inspector"))
            {
                drawQRPCInspectorPage(runtimeTelemetry);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("USB"))
            {
                drawUSBProfilerPage(usb, runtimeBindings);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Input Analyzer"))
            {
                drawInputAnalyzerPage(keyboardInput, reactiveKeys, inputAnalytics);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("RGB Profiler"))
            {
                drawRGBProfilerPage(framebuffer, settings, rgbAnalytics);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Audio Lab"))
            {
                drawAudioLabPage(audio, audioLevel, autoGain, settings, analysisBands, mappedBands, smoothedBands);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Timeline"))
            {
                drawTimelinePage(runtimeTelemetry);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Firmware"))
            {
                drawFirmwarePage(performance, hasPerformance, timingProbe, hasTimingProbe);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Performance"))
            {
                const std::size_t framebufferPacketBytes = sizeof(PacketHeader) + sizeof(FramebufferSetPayload<MatrixSize>);
                const double configuredTxKiB = settings.Enabled && settings.SendFramebuffer ? framebufferPacketBytes * static_cast<double>(settings.FrameRate) / 1024.0 : 0.0;
                ImGui::Text("Host CPU: %.2f%%   target framebuffer rate: %d Hz   estimated TX: %.1f KiB/s", appCpuUsage, settings.FrameRate, configuredTxKiB);
                ImGui::Text("Packets received: %llu   frames sent: %llu   dropped/busy: %llu", static_cast<unsigned long long>(receivedPackets), static_cast<unsigned long long>(sentFrames), static_cast<unsigned long long>(droppedFrames));
                ImGui::Separator();
                if (hasPerformance)
                    drawPerformance(performance);
                else
                    ImGui::TextDisabled("Waiting for PerformanceResponse...");
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Matrix timing"))
            {
                if (hasTimingProbe)
                    drawTimingProbe(timingProbe);
                else
                    ImGui::TextDisabled("Waiting for MatrixTimingProbeResult...");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::End();
    }

}
