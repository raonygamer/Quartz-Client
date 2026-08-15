#include "quartz/client/ui/pages/QRPCPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include <algorithm>
#include <cstdio>
#include <deque>
#include <imgui.h>

namespace quartz::client::ui
{
    void QRPCPage::render(PageContext& context, PageManager& manager)
    {
        auto& telemetry = context.runtimeTelemetry;
        (void)manager;

        static bool paused = false;
        static int selectedNewest = 0;
        static std::deque<RuntimePacketRecord> frozenPackets;

        const auto livePackets = telemetry.packets();
        if (ImGui::Button("Clear packets"))
        {
            telemetry.clearPackets();
            frozenPackets.clear();
            selectedNewest = 0;
            paused = false;
        }
        ImGui::SameLine();
        if (paused)
        {
            if (ImGui::Button("Resume live")) { paused = false; selectedNewest = 0; }
            ImGui::SameLine(); ImGui::TextDisabled("paused - incoming packets keep being captured");
        }
        else
        {
            if (ImGui::Button("Pause view")) { frozenPackets = livePackets; paused = true; }
            ImGui::SameLine(); ImGui::TextDisabled("selecting a packet pauses the list so it cannot scroll away");
        }

        const auto& packets = paused ? frozenPackets : livePackets;
        if (!packets.empty()) selectedNewest = std::clamp(selectedNewest, 0, static_cast<int>(packets.size()) - 1);
        else selectedNewest = 0;

        ImGui::SeparatorText("QRPC packet inspector");
        if (ImGui::BeginTable("QRPCPackets", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 330.0f)))
        {
            ImGui::TableSetupColumn("t");
            ImGui::TableSetupColumn("Dir");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Packet id");
            ImGui::TableSetupColumn("Response for");
            ImGui::TableSetupColumn("Payload");
            ImGui::TableSetupColumn("Packet");
            ImGui::TableSetupColumn("Version");
            ImGui::TableHeadersRow();
            int newestIndex = 0;
            for (auto it = packets.rbegin(); it != packets.rend(); ++it, ++newestIndex)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(newestIndex);
                const bool selected = selectedNewest == newestIndex;
                if (ImGui::Selectable("##packet", selected, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, ImGui::GetTextLineHeight())))
                {
                    selectedNewest = newestIndex;
                    if (!paused) { frozenPackets = livePackets; paused = true; }
                }
                ImGui::SameLine(); ImGui::Text("%.3f", it->Time);
                ImGui::PopID();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->Tx ? "Host -> Device" : "Device -> Host");
                ImGui::TableNextColumn(); ImGui::Text("%s (0x%04X)", packetTypeLabel(it->Type), it->Type);
                ImGui::TableNextColumn(); ImGui::Text("%u", it->PacketId);
                ImGui::TableNextColumn(); ImGui::Text("%u", it->ResponseFor);
                ImGui::TableNextColumn(); ImGui::Text("%u B", it->PayloadLength);
                ImGui::TableNextColumn(); ImGui::Text("%zu B", it->Bytes);
                ImGui::TableNextColumn(); ImGui::Text("%u", it->Version);
            }
            ImGui::EndTable();
        }

        if (paused) ImGui::Text("Frozen packets: %zu   live buffer: %zu / 512", packets.size(), livePackets.size());
        else ImGui::Text("Buffered packets: %zu / 512", livePackets.size());
        if (packets.empty()) return;

        const auto& packet = packets[packets.size() - 1 - static_cast<std::size_t>(selectedNewest)];
        ImGui::SeparatorText("Packet bytes");
        ImGui::Text("%s %s   packet %u   %zu/%zu B captured", packet.Tx ? "TX" : "RX", packetTypeLabel(packet.Type), packet.PacketId, packet.CapturedBytes, packet.Bytes);
        if (ImGui::BeginChild("##PacketHex", ImVec2(0.0f, 160.0f), true, ImGuiWindowFlags_HorizontalScrollbar))
        {
            for (std::size_t offset = 0; offset < packet.CapturedBytes; offset += 16)
            {
                char line[128]{};
                int written = std::snprintf(line, sizeof(line), "%04zX  ", offset);
                for (std::size_t i = 0; i < 16; ++i)
                {
                    if (offset + i < packet.CapturedBytes) written += std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written), "%02X ", std::to_integer<unsigned>(packet.Data[offset + i]));
                    else written += std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written), "   ");
                }
                written += std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written), " |");
                for (std::size_t i = 0; i < 16 && offset + i < packet.CapturedBytes; ++i)
                {
                    const unsigned byte = std::to_integer<unsigned>(packet.Data[offset + i]);
                    if (written + 2 < static_cast<int>(sizeof(line))) line[written++] = byte >= 32 && byte < 127 ? static_cast<char>(byte) : '.';
                }
                if (written + 2 < static_cast<int>(sizeof(line))) { line[written++] = '|'; line[written] = '\0'; }
                ImGui::TextUnformatted(line);
            }
            ImGui::EndChild();
        }
    }
}
