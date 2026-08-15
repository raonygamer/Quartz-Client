#include "quartz/client/ui/pages/VisualizerPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void VisualizerPage::render(PageContext& context, PageManager& manager)
    {
        auto& usb = context.usb;
        auto& audio = context.audio;
        auto& mediaColor = context.mediaColor;
        auto& shaderFramebuffer = context.shaderFramebuffer;
        auto& shaderTransition = context.shaderTransition;
        auto& shaderEditor = context.shaderEditor;
        auto& vertexShaderSource = context.vertexShaderSource;
        auto& fragmentShaderSource = context.fragmentShaderSource;
        auto& settings = context.settings;
        const auto& framebuffer = context.framebuffer;
        const auto& autoGain = context.autoGain;
        const auto& audioLevel = context.audioLevel;
        const auto scrollLockActive = context.scrollLockActive;
        const auto capsLockActive = context.capsLockActive;
        static const VisualizerSettings defaults{};
        const bool connected = usb.isConnected();

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
                manager.open("shader-editor");
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
    }
}
