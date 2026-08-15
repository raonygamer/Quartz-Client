#include "quartz/client/ui/pages/VisualizerPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void VisualizerPage::render(PageContext& context, PageManager& manager)
    {
        auto& settings = context.settings;
        auto& usb = context.usb;
        static const VisualizerSettings defaults{};
        ImGui::TextWrapped("Live output. Detailed spectrum/audio tuning, shader management and RGB diagnostics live on their dedicated pages.");
        ImGui::SeparatorText("Output");
        ImGui::Checkbox("Enabled", &settings.Enabled); defaultButton("Enabled", settings.Enabled, defaults.Enabled);
        ImGui::Checkbox("Send framebuffer", &settings.SendFramebuffer); defaultButton("SendFramebuffer", settings.SendFramebuffer, defaults.SendFramebuffer);
        ImGui::SetNextItemWidth(180.0f); ImGui::SliderInt("Frame rate", &settings.FrameRate, 30, 500, "%d Hz"); defaultButton("FrameRate", settings.FrameRate, defaults.FrameRate);
        ImGui::SetNextItemWidth(180.0f); ImGui::SliderFloat("Global brightness", &settings.GlobalBrightness, 0.0f, 1.0f, "%.2f"); defaultButton("GlobalBrightness", settings.GlobalBrightness, defaults.GlobalBrightness);
        const char* modes[] = {"RGB wave", "Solid", "Shader"};
        ImGui::SetNextItemWidth(180.0f); ImGui::Combo("Output mode", &settings.BaseColorMode, modes, 3);
        if (settings.BaseColorMode == 0) { ImGui::SetNextItemWidth(180.0f); ImGui::SliderFloat("Wave speed", &settings.WaveSpeed, -2.0f, 2.0f, "%.3f"); }
        else if (settings.BaseColorMode == 1) ImGui::ColorEdit3("Solid color", settings.SolidColor.data());
        if (ImGui::Button("Shaders...")) manager.open("shaders"); ImGui::SameLine();
        if (ImGui::Button("Spectrum...")) manager.open("spectrum"); ImGui::SameLine();
        if (ImGui::Button("Audio...")) manager.open("audio"); ImGui::SameLine();
        if (ImGui::Button("RGB diagnostics...")) manager.open("rgb");

        ImGui::SeparatorText("Keyboard preview");
        drawFramebufferPreview(context.framebuffer, 0.72f, 720.0f, settings.LiveOutputInterpolation);
        ImGui::SliderFloat("Preview interpolation", &settings.LiveOutputInterpolation, 0.0f, 1.0f, "%.2f");
        ImGui::Text("Frames sent %llu   dropped %llu   USB %s", static_cast<unsigned long long>(context.sentFrames), static_cast<unsigned long long>(context.droppedFrames), usb.isConnected() ? "connected" : "disconnected");
        if (settings.BaseColorMode == 2)
        {
            const bool valid = settings.ShaderPresetIndex > 0 && settings.ShaderPresetIndex <= static_cast<int>(ShaderPresets.size());
            ImGui::Text("Shader: %s", valid ? ShaderPresets[static_cast<std::size_t>(settings.ShaderPresetIndex - 1)].Name.c_str() : (settings.ShaderId.empty() ? "Custom / external" : settings.ShaderId.c_str()));
            ImGui::SameLine(); ImGui::TextDisabled("%s", context.shaderFramebuffer.status().c_str());
        }
        if (ImGui::Button("Black out") && usb.isConnected()) { std::array<Color32, MatrixSize> black{}; sendFramebuffer(usb, black); }
    }
}
