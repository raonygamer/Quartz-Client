#include "quartz/client/Model.hpp"

namespace quartz::client
{
    void drawIndeterminateProgressBar(ImVec2 size)
    {
        if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;
        if (size.y <= 0.0f) size.y = ImGui::GetFrameHeight();
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max{min.x + size.x, min.y + size.y};
        ImGui::Dummy(size);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float rounding = ImGui::GetStyle().FrameRounding;
        drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
        const float segmentWidth = std::max(size.x * 0.28f, 24.0f);
        const float phase = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.85f, 1.0f);
        const float left = min.x - segmentWidth + (size.x + segmentWidth) * phase;
        const float clippedLeft = std::max(left, min.x);
        const float clippedRight = std::min(left + segmentWidth, max.x);
        if (clippedRight > clippedLeft) drawList->AddRectFilled({clippedLeft, min.y}, {clippedRight, max.y}, ImGui::GetColorU32(ImGuiCol_PlotHistogram), rounding);
    }

    void mapSpectrumToColumns(const std::span<const float> analysisBands, std::array<float, Columns>& bands, const VisualizerSettings& settings, const float overallGain)
    {
        if (analysisBands.empty())
        {
            bands.fill(0.0f);
            return;
        }
        const int bassColumns = std::clamp(settings.BassColumns, 2, static_cast<int>(Columns) - 2);
        const int bassEndBand = std::clamp(settings.BassEndBand, 0, static_cast<int>(analysisBands.size()) - 1);
        for (int column = 0; column < static_cast<int>(Columns); ++column)
        {
            int sourceBand;
            float level;
            float gain = settings.ColumnGain[column] * overallGain;
            if (column < bassColumns)
            {
                const float t = column / static_cast<float>(bassColumns - 1);
                sourceBand = static_cast<int>(std::lround(t * bassEndBand));
                level = analysisBands[sourceBand];
                float activation = std::clamp((level - settings.BassActivationThreshold) / std::max(0.001f, 1.0f - settings.BassActivationThreshold), 0.0f, 1.0f);
                activation = activation * activation * (3.0f - 2.0f * activation);
                gain *= 1.0f + (settings.BassMaxBoost - 1.0f) * activation;
            }
            else
            {
                const float t = (column - bassColumns) / static_cast<float>(Columns - bassColumns - 1);
                const int firstHighBand = std::min(bassEndBand + 2, static_cast<int>(analysisBands.size()) - 1);
                sourceBand = static_cast<int>(std::lround(firstHighBand + t * (static_cast<int>(analysisBands.size()) - 1 - firstHighBand)));
                level = analysisBands[sourceBand];
            }
            bands[column] = std::clamp(level * gain, 0.0f, 1.0f);
        }
    }

    void smoothBands(const std::array<float, Columns>& bands, std::array<float, Columns>& smoothedBands, const VisualizerSettings& settings, const float dt)
    {
        for (std::size_t i = 0; i < Columns; ++i)
        {
            const float target = bands[i];
            const float speed = target > smoothedBands[i] ? settings.AttackSpeed : settings.ReleaseSpeed;
            const float alpha = 1.0f - std::exp(-speed * dt);
            smoothedBands[i] += (target - smoothedBands[i]) * alpha;
        }
    }

    void renderAudioRGB(std::array<Color32, MatrixSize>& framebuffer, const std::array<float, Columns>& bands, const VisualizerSettings& settings, const std::optional<Color32> visualizerColor, const float mediaColorAmount, const float wavePhase)
    {
        framebuffer.fill({0, 0, 0});
        constexpr int VisibleRows = static_cast<int>(Rows) - 1;
        const Color32 solid = floatColor(settings.SolidColor);
        for (std::size_t column = 0; column < Columns; ++column)
        {
            const float level = std::clamp(bands[column], 0.0f, 1.0f);
            const float exactRows = level * VisibleRows;
            const Color32 waveColor = hsvToRgb(static_cast<float>(column) / static_cast<float>(Columns) - wavePhase, 1.0f, 1.0f);
            Color32 baseColor = settings.BaseColorMode == 0 ? waveColor : solid;
            if (visualizerColor)
                baseColor = lerpColor(baseColor, *visualizerColor, mediaColorAmount * settings.MediaColorBlend);
            for (int visualRow = 0; visualRow < VisibleRows; ++visualRow)
            {
                const int row = VisibleRows - 1 - visualRow;
                float amount = settings.ForceFullRow && row == settings.FullRow ? 1.0f : std::clamp((exactRows - visualRow) / settings.FeatherRows, 0.0f, 1.0f);
                amount = amount * amount * (3.0f - 2.0f * amount);
                float r = baseColor.R * amount;
                float g = baseColor.G * amount;
                float b = baseColor.B * amount;
                saturate(r, g, b, settings.Saturation);
                framebuffer[row * Columns + column] = {static_cast<std::uint8_t>(std::lround(r)), static_cast<std::uint8_t>(std::lround(g)), static_cast<std::uint8_t>(std::lround(b))};
            }
        }
    }

    bool sendFramebuffer(RawUSB& usb, const std::array<Color32, MatrixSize>& framebuffer)
    {
        FramebufferSetPayload<MatrixSize> payload{};
        payload.Framebuffer = framebuffer;
        return usb.send(makePacket(PacketType::FramebufferSet, payload));
    }

    void applyGlobalBrightness(std::array<Color32, MatrixSize>& framebuffer, const float brightness) noexcept
    {
        const float amount = std::clamp(brightness, 0.0f, 1.0f);
        if (amount >= 0.9999f) return;
        for (auto& color : framebuffer)
        {
            color.R = static_cast<std::uint8_t>(std::lround(color.R * amount));
            color.G = static_cast<std::uint8_t>(std::lround(color.G * amount));
            color.B = static_cast<std::uint8_t>(std::lround(color.B * amount));
        }
    }

    Color32 interpolatePreviewColor(const std::array<Color32, MatrixSize>& framebuffer, const std::uint8_t row, const std::uint8_t column, const float interpolation) noexcept
    {
        const Color32 center = framebuffer[static_cast<std::size_t>(row) * Columns + column];
        const float amount = std::clamp(interpolation, 0.0f, 1.0f);
        if (amount <= 0.0001f) return center;
        float r = 0.0f, g = 0.0f, b = 0.0f, totalWeight = 0.0f;
        for (int y = -1; y <= 1; ++y)
        {
            const int sampleRow = static_cast<int>(row) + y;
            if (sampleRow < 0 || sampleRow >= static_cast<int>(ActiveProbeRows)) continue;
            for (int x = -1; x <= 1; ++x)
            {
                const int sampleColumn = static_cast<int>(column) + x;
                if (sampleColumn < 0 || sampleColumn >= static_cast<int>(Columns)) continue;
                const float weight = (x == 0 ? 2.0f : 1.0f) * (y == 0 ? 2.0f : 1.0f);
                const Color32 sample = framebuffer[static_cast<std::size_t>(sampleRow) * Columns + static_cast<std::size_t>(sampleColumn)];
                r += sample.R * weight; g += sample.G * weight; b += sample.B * weight; totalWeight += weight;
            }
        }
        if (totalWeight <= 0.0f) return center;
        const auto mixChannel = [amount, totalWeight](const std::uint8_t source, const float accumulated) -> std::uint8_t { return static_cast<std::uint8_t>(std::lround(std::clamp(source + (accumulated / totalWeight - source) * amount, 0.0f, 255.0f))); };
        return {mixChannel(center.R, r), mixChannel(center.G, g), mixChannel(center.B, b)};
    }

    void drawRuntimeStateSquare(const char* id, const ImVec4 color)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
        const float size = ImGui::GetFrameHeight();
        ImGui::Button(id, ImVec2(size, size));
        ImGui::PopStyleColor(3);
    }
}
