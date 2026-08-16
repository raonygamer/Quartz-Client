#include "quartz/client/ui/pages/VisualizerPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/I18n.hpp"

namespace quartz::client::ui
{
    void VisualizerPage::render(PageContext& context, PageManager& manager)
    {
        auto& settings = context.settings; auto& usb = context.usb; static const VisualizerSettings defaults{};
        ImGui::TextWrapped("%s",i18n::tr("visualizer.description")); ImGui::SeparatorText(i18n::tr("visualizer.output"));
        ImGui::Checkbox(i18n::tr("visualizer.enabled"),&settings.Enabled); defaultButton("Enabled",settings.Enabled,defaults.Enabled);
        ImGui::Checkbox(i18n::tr("visualizer.sendFramebuffer"),&settings.SendFramebuffer); defaultButton("SendFramebuffer",settings.SendFramebuffer,defaults.SendFramebuffer);
        ImGui::SetNextItemWidth(180.0f); ImGui::SliderInt(i18n::tr("visualizer.frameRate"),&settings.FrameRate,30,500,"%d Hz"); defaultButton("FrameRate",settings.FrameRate,defaults.FrameRate);
        ImGui::SetNextItemWidth(180.0f); ImGui::SliderFloat(i18n::tr("appearance.globalBrightness"),&settings.GlobalBrightness,0.0f,1.0f,"%.2f"); defaultButton("GlobalBrightness",settings.GlobalBrightness,defaults.GlobalBrightness);
        const char* modes[] = {i18n::tr("visualizer.rgbWave"),i18n::tr("visualizer.solid"),i18n::tr("visualizer.shader")}; ImGui::SetNextItemWidth(180.0f); ImGui::Combo(i18n::tr("visualizer.outputMode"),&settings.BaseColorMode,modes,3);
        if (settings.BaseColorMode==0) { ImGui::SetNextItemWidth(180.0f); ImGui::SliderFloat(i18n::tr("visualizer.waveSpeed"),&settings.WaveSpeed,-2.0f,2.0f,"%.3f"); } else if (settings.BaseColorMode==1) ImGui::ColorEdit3(i18n::tr("visualizer.solidColor"),settings.SolidColor.data());
        if (ImGui::Button(i18n::tr("visualizer.openShaders"))) manager.open("shaders"); ImGui::SameLine(); if (ImGui::Button(i18n::tr("visualizer.openSpectrum"))) manager.open("spectrum"); ImGui::SameLine(); if (ImGui::Button(i18n::tr("visualizer.openAudio"))) manager.open("audio"); ImGui::SameLine(); if (ImGui::Button(i18n::tr("visualizer.openRgb"))) manager.open("rgb");
        ImGui::SeparatorText(i18n::tr("visualizer.keyboardPreview")); drawFramebufferPreview(context.framebuffer,0.72f,720.0f,settings.LiveOutputInterpolation); ImGui::SliderFloat(i18n::tr("appearance.previewInterpolation"),&settings.LiveOutputInterpolation,0.0f,1.0f,"%.2f");
        ImGui::Text(i18n::tr("visualizer.frameStats"),static_cast<unsigned long long>(context.sentFrames),static_cast<unsigned long long>(context.droppedFrames),usb.isConnected()?i18n::tr("common.connected"):i18n::tr("common.disconnected"));
        if (settings.BaseColorMode==2) { const bool valid=settings.ShaderPresetIndex>0&&settings.ShaderPresetIndex<=static_cast<int>(ShaderPresets.size()); ImGui::Text(i18n::tr("visualizer.shaderName"),valid?ShaderPresets[static_cast<std::size_t>(settings.ShaderPresetIndex-1)].Name.c_str():(settings.ShaderId.empty()?i18n::tr("shaders.customExternal"):settings.ShaderId.c_str())); ImGui::SameLine(); ImGui::TextDisabled("%s",context.shaderFramebuffer.status().c_str()); }
        if (ImGui::Button(i18n::tr("visualizer.blackOut"))&&usb.isConnected()) { std::array<Color32,MatrixSize> black{}; sendFramebuffer(usb,black); }
    }
}
