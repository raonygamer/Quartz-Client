#include "quartz/client/ui/Configuration.hpp"
#include "quartz/client/ui/I18n.hpp"
#include "quartz/client/settings/RuntimeConfiguration.hpp"
#include "quartz/client/native/SignatureScanner.hpp"
#include <imgui.h>
#include <algorithm>
#include <cstdint>

namespace quartz::client::ui
{
    void drawConfigurationSettings()
    {
        auto& configuration = runtimeConfiguration(); bool changed = false;
        if (ImGui::BeginTabBar("##QuartzConfigurationTabs"))
        {
            if (ImGui::BeginTabItem(i18n::tr("configuration.patternScanning")))
            {
                std::uint64_t bytes = configuration.SignatureScanChunkBytes; ImGui::SetNextItemWidth(220.0f);
                const std::uint64_t step = 64ULL * 1024ULL, fastStep = 1024ULL * 1024ULL;
                if (ImGui::InputScalar(i18n::tr("configuration.signatureScanChunk"), ImGuiDataType_U64, &bytes, &step, &fastStep, "%llu"))
                {
                    configuration.SignatureScanChunkBytes = normalizeSignatureScanChunkBytes(static_cast<std::size_t>(std::min<std::uint64_t>(bytes, MaximumSignatureScanChunkBytes))); changed = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::tr("configuration.signatureScanChunkTooltip"));
                ImGui::TextDisabled(i18n::tr("configuration.signatureScanChunkValue"), static_cast<double>(configuration.SignatureScanChunkBytes) / 1048576.0, configuration.SignatureScanChunkBytes, SignatureScanChunkAlignment);
                ImGui::TextWrapped("%s", i18n::tr("configuration.signatureScanChunkTradeoff")); ImGui::TextDisabled("%s", i18n::tr("configuration.newScansOnly"));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Disassembler"))
            {
                ImGui::TextWrapped("Slow refresh defaults keep process-memory reads out of the hot UI path. Set a rate to 0 Hz to disable automatic refresh.");
                ImGui::SetNextItemWidth(190.0f); changed |= ImGui::SliderFloat("Disassembly refresh", &configuration.DisassemblyRefreshHz, 0.0f, 10.0f, configuration.DisassemblyRefreshHz == 0.0f ? "off" : "%.2f Hz", ImGuiSliderFlags_Logarithmic);
                ImGui::SetNextItemWidth(190.0f); changed |= ImGui::SliderFloat("Raw bytes refresh", &configuration.RawBytesRefreshHz, 0.0f, 10.0f, configuration.RawBytesRefreshHz == 0.0f ? "off" : "%.2f Hz", ImGuiSliderFlags_Logarithmic);
                changed |= ImGui::Checkbox("Background function heuristics", &configuration.FunctionHeuristics);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Runs low-priority local function-boundary analysis on Quartz's shared thread pool. The disassembler never waits for it.");
                std::uint64_t analysisBytes = configuration.FunctionAnalysisWindowBytes; const std::uint64_t analysisStep = 16ULL * 1024ULL, analysisFast = 64ULL * 1024ULL; ImGui::SetNextItemWidth(220.0f);
                if (ImGui::InputScalar("Function analysis window", ImGuiDataType_U64, &analysisBytes, &analysisStep, &analysisFast, "%llu")) { configuration.FunctionAnalysisWindowBytes = static_cast<std::size_t>(analysisBytes); changed = true; }
                ImGui::TextDisabled("Local window around the current disassembly address. It is clamped and page-aligned when saved.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Assembler Tweaks"))
            {
                ImGui::TextWrapped("Controls how in-place patches are prepared. Quartz always previews the final byte span before a process write.");
                changed |= ImGui::Checkbox("Fill remaining bytes with NOPs", &configuration.AssemblerFillNops);
                changed |= ImGui::Checkbox("Keep selections on whole instructions", &configuration.AssemblerWholeInstructions);
                changed |= ImGui::Checkbox("Allow patches to consume following instructions", &configuration.AssemblerConsumeFollowing);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("If assembled code is larger than the selected span, Quartz may extend the patch over complete following instructions. Disabled by default.");
                changed |= ImGui::Checkbox("Verify original bytes before writing", &configuration.AssemblerVerifyBeforeWrite);
                changed |= ImGui::Checkbox("Require write confirmation", &configuration.AssemblerRequireConfirmation);
                changed |= ImGui::Checkbox("Re-disassemble immediately after patch", &configuration.AssemblerAutoRedisassemble);
                changed |= ImGui::Checkbox("Keep original bytes for Restore", &configuration.AssemblerKeepOriginalBytes);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (changed) saveRuntimeConfiguration();
        ImGui::Separator(); if (ImGui::Button(i18n::tr("configuration.reset"))) resetRuntimeConfiguration();
    }
}
