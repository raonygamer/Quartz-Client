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
        auto& configuration = runtimeConfiguration();
        ImGui::SeparatorText(i18n::tr("configuration.patternScanning"));
        std::uint64_t bytes = configuration.SignatureScanChunkBytes;
        ImGui::SetNextItemWidth(220.0f);
        const std::uint64_t step = 64ULL * 1024ULL, fastStep = 1024ULL * 1024ULL;
        if (ImGui::InputScalar(i18n::tr("configuration.signatureScanChunk"), ImGuiDataType_U64, &bytes, &step, &fastStep, "%llu"))
        {
            configuration.SignatureScanChunkBytes = normalizeSignatureScanChunkBytes(static_cast<std::size_t>(std::min<std::uint64_t>(bytes, MaximumSignatureScanChunkBytes)));
            saveRuntimeConfiguration();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::tr("configuration.signatureScanChunkTooltip"));
        ImGui::TextDisabled(i18n::tr("configuration.signatureScanChunkValue"), static_cast<double>(configuration.SignatureScanChunkBytes) / 1048576.0, configuration.SignatureScanChunkBytes, SignatureScanChunkAlignment);
        ImGui::TextWrapped("%s", i18n::tr("configuration.signatureScanChunkTradeoff"));
        ImGui::TextDisabled("%s", i18n::tr("configuration.newScansOnly"));
        if (ImGui::Button(i18n::tr("configuration.reset"))) resetRuntimeConfiguration();
    }
}
