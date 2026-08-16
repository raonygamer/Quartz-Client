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
        auto& configuration=runtimeConfiguration(); bool changed=false;
        if (ImGui::BeginTabBar("##QuartzConfigurationTabs"))
        {
            if (ImGui::BeginTabItem(i18n::tr("configuration.patternScanning")))
            {
                std::uint64_t bytes=configuration.SignatureScanChunkBytes; ImGui::SetNextItemWidth(220.0f); const std::uint64_t step=64ULL*1024ULL,fastStep=1024ULL*1024ULL;
                if (ImGui::InputScalar(i18n::tr("configuration.signatureScanChunk"),ImGuiDataType_U64,&bytes,&step,&fastStep,"%llu")) { configuration.SignatureScanChunkBytes=normalizeSignatureScanChunkBytes(static_cast<std::size_t>(std::min<std::uint64_t>(bytes,MaximumSignatureScanChunkBytes))); changed=true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",i18n::tr("configuration.signatureScanChunkTooltip")); ImGui::TextDisabled(i18n::tr("configuration.signatureScanChunkValue"),static_cast<double>(configuration.SignatureScanChunkBytes)/1048576.0,configuration.SignatureScanChunkBytes,SignatureScanChunkAlignment); ImGui::TextWrapped("%s",i18n::tr("configuration.signatureScanChunkTradeoff")); ImGui::TextDisabled("%s",i18n::tr("configuration.newScansOnly")); ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(i18n::tr("configuration.disassembler")))
            {
                ImGui::TextWrapped("%s",i18n::tr("configuration.disassemblerDescription")); ImGui::SetNextItemWidth(190.0f); changed|=ImGui::SliderFloat(i18n::tr("configuration.disassemblyRefresh"),&configuration.DisassemblyRefreshHz,0.0f,10.0f,configuration.DisassemblyRefreshHz==0.0f?"off":"%.2f Hz",ImGuiSliderFlags_Logarithmic); ImGui::SetNextItemWidth(190.0f); changed|=ImGui::SliderFloat(i18n::tr("configuration.rawRefresh"),&configuration.RawBytesRefreshHz,0.0f,10.0f,configuration.RawBytesRefreshHz==0.0f?"off":"%.2f Hz",ImGuiSliderFlags_Logarithmic); changed|=ImGui::Checkbox(i18n::tr("configuration.functionHeuristics"),&configuration.FunctionHeuristics); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",i18n::tr("configuration.functionHeuristicsTooltip")); std::uint64_t analysisBytes=configuration.FunctionAnalysisWindowBytes; const std::uint64_t analysisStep=16ULL*1024ULL,analysisFast=64ULL*1024ULL; ImGui::SetNextItemWidth(220.0f); if (ImGui::InputScalar(i18n::tr("configuration.functionWindow"),ImGuiDataType_U64,&analysisBytes,&analysisStep,&analysisFast,"%llu")) { configuration.FunctionAnalysisWindowBytes=static_cast<std::size_t>(analysisBytes); changed=true; } ImGui::TextDisabled("%s",i18n::tr("configuration.functionWindowHint")); ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(i18n::tr("configuration.assemblerTweaks")))
            {
                ImGui::TextWrapped("%s",i18n::tr("configuration.assemblerDescription")); changed|=ImGui::Checkbox(i18n::tr("configuration.fillNops"),&configuration.AssemblerFillNops); changed|=ImGui::Checkbox(i18n::tr("configuration.wholeInstructions"),&configuration.AssemblerWholeInstructions); changed|=ImGui::Checkbox(i18n::tr("configuration.consumeFollowing"),&configuration.AssemblerConsumeFollowing); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",i18n::tr("configuration.consumeFollowingTooltip")); changed|=ImGui::Checkbox(i18n::tr("configuration.verifyBeforeWrite"),&configuration.AssemblerVerifyBeforeWrite); changed|=ImGui::Checkbox(i18n::tr("configuration.requireConfirmation"),&configuration.AssemblerRequireConfirmation); changed|=ImGui::Checkbox(i18n::tr("configuration.redisassemble"),&configuration.AssemblerAutoRedisassemble); changed|=ImGui::Checkbox(i18n::tr("configuration.keepOriginal"),&configuration.AssemblerKeepOriginalBytes); ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (changed) saveRuntimeConfiguration(); ImGui::Separator(); if (ImGui::Button(i18n::tr("configuration.reset"))) resetRuntimeConfiguration();
    }
}
