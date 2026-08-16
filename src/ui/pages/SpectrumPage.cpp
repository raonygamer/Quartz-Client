#include "quartz/client/ui/pages/SpectrumPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/I18n.hpp"

namespace quartz::client::ui
{
    void SpectrumPage::render(PageContext& context, PageManager& manager)
    {
        auto& settings=context.settings; const auto& analysisBands=context.analysisBands; const auto& mappedBands=context.mappedBands; const auto& smoothedBands=context.smoothedBands; const auto& framebuffer=context.framebuffer; static const VisualizerSettings defaults{}; (void)manager;
        ImGui::Checkbox(i18n::tr("spectrum.analysisGraph"),&settings.ShowAnalysisSpectrum); defaultButton("ShowAnalysisSpectrum",settings.ShowAnalysisSpectrum,defaults.ShowAnalysisSpectrum);
        ImGui::Checkbox(i18n::tr("spectrum.mappedGraph"),&settings.ShowMappedSpectrum); defaultButton("ShowMappedSpectrum",settings.ShowMappedSpectrum,defaults.ShowMappedSpectrum);
        ImGui::Checkbox(i18n::tr("spectrum.framebufferPreview"),&settings.ShowFramebuffer); defaultButton("ShowFramebuffer",settings.ShowFramebuffer,defaults.ShowFramebuffer);
        if (settings.ShowAnalysisSpectrum) { ImGui::TextUnformatted(i18n::tr("spectrum.fftAnalysis")); ImGui::PlotLines("##analysis",analysisBands.data(),settings.AnalysisBandCount,0,nullptr,0.0f,1.0f,ImVec2(-1.0f,150.0f)); }
        if (settings.ShowMappedSpectrum) { ImGui::TextUnformatted(i18n::tr("spectrum.mappedColumns")); ImGui::PlotHistogram("##mapped",mappedBands.data(),static_cast<int>(Columns),0,nullptr,0.0f,1.0f,ImVec2(-1.0f,110.0f)); ImGui::TextUnformatted(i18n::tr("spectrum.smoothedOutput")); ImGui::PlotHistogram("##smoothed",smoothedBands.data(),static_cast<int>(Columns),0,nullptr,0.0f,1.0f,ImVec2(-1.0f,110.0f)); }
        if (settings.ShowFramebuffer) { ImGui::TextUnformatted(i18n::tr("spectrum.keyboardFramebuffer")); drawFramebufferPreview(framebuffer,0.55f,532.0f,settings.LiveOutputInterpolation); ImGui::TextDisabled(i18n::tr("spectrum.previewInterpolation"),settings.LiveOutputInterpolation*100.0f); }
    }
}
