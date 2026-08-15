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
                // The physical LED rows are framebuffer rows 0..5. Row 6 is the unused extra row.
                const int row = VisibleRows - 1 - visualRow;
                float amount = settings.ForceFullRow && row == settings.FullRow ? 1.0f : std::clamp((exactRows - visualRow) / settings.FeatherRows, 0.0f, 1.0f);
                amount = amount * amount * (3.0f - 2.0f * amount);
                float r = baseColor.R * amount;
                float g = baseColor.G * amount;
                float b = baseColor.B * amount;
                saturate(r, g, b, settings.Saturation);
                framebuffer[row * Columns + column] = {
                    static_cast<std::uint8_t>(std::lround(r)),
                    static_cast<std::uint8_t>(std::lround(g)),
                    static_cast<std::uint8_t>(std::lround(b))
                };
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
                r += sample.R * weight;
                g += sample.G * weight;
                b += sample.B * weight;
                totalWeight += weight;
            }
        }
        if (totalWeight <= 0.0f) return center;
        const auto mixChannel = [amount, totalWeight](const std::uint8_t source, const float accumulated) -> std::uint8_t
        {
            return static_cast<std::uint8_t>(std::lround(std::clamp(source + (accumulated / totalWeight - source) * amount, 0.0f, 255.0f)));
        };
        return {mixChannel(center.R, r), mixChannel(center.G, g), mixChannel(center.B, b)};
    }

    void drawFramebufferPreview(const std::array<Color32, MatrixSize>& framebuffer, const float widthFraction , const float maxWidth , const float interpolation)
    {
        // Approximate physical K552 ISO/TKL silhouette. Framebuffer rows 0..5 are the
        // six visible keyboard rows; row 6 is unused. Wide physical keys reuse the
        // nearest framebuffer column color because the framebuffer itself is still 7x16.
        static constexpr auto Keys = std::to_array<PreviewRect>({
            // Function row
            {0,  0,  0.00f, 0.00f, 1.00f, 0.95f},
            {0,  1,  2.00f, 0.00f, 1.00f, 0.95f}, {0,  2,  3.00f, 0.00f, 1.00f, 0.95f}, {0,  3,  4.00f, 0.00f, 1.00f, 0.95f}, {0,  4,  5.00f, 0.00f, 1.00f, 0.95f},
            {0,  5,  6.50f, 0.00f, 1.00f, 0.95f}, {0,  6,  7.50f, 0.00f, 1.00f, 0.95f}, {0,  7,  8.50f, 0.00f, 1.00f, 0.95f}, {0,  8,  9.50f, 0.00f, 1.00f, 0.95f},
            {0,  9, 11.00f, 0.00f, 1.00f, 0.95f}, {0, 10, 12.00f, 0.00f, 1.00f, 0.95f}, {0, 11, 13.00f, 0.00f, 1.00f, 0.95f}, {0, 12, 14.00f, 0.00f, 1.00f, 0.95f},
            {0, 13, 16.00f, 0.00f, 1.00f, 0.95f}, {0, 14, 17.00f, 0.00f, 1.00f, 0.95f}, {0, 15, 18.00f, 0.00f, 1.00f, 0.95f},

            // Number row: ` 1..0 - = Backspace | Ins Home PgUp
            {1,  0,  0.00f, 1.35f, 1.00f, 0.95f}, {1,  1,  1.00f, 1.35f, 1.00f, 0.95f}, {1,  2,  2.00f, 1.35f, 1.00f, 0.95f}, {1,  3,  3.00f, 1.35f, 1.00f, 0.95f},
            {1,  4,  4.00f, 1.35f, 1.00f, 0.95f}, {1,  5,  5.00f, 1.35f, 1.00f, 0.95f}, {1,  6,  6.00f, 1.35f, 1.00f, 0.95f}, {1,  7,  7.00f, 1.35f, 1.00f, 0.95f},
            {1,  8,  8.00f, 1.35f, 1.00f, 0.95f}, {1,  9,  9.00f, 1.35f, 1.00f, 0.95f}, {1, 10, 10.00f, 1.35f, 1.00f, 0.95f}, {1, 11, 11.00f, 1.35f, 1.00f, 0.95f},
            {1, 12, 12.00f, 1.35f, 1.00f, 0.95f}, {1, 12, 13.00f, 1.35f, 2.00f, 0.95f},
            {1, 13, 16.00f, 1.35f, 1.00f, 0.95f}, {1, 14, 17.00f, 1.35f, 1.00f, 0.95f}, {1, 15, 18.00f, 1.35f, 1.00f, 0.95f},

            // QWERTY row: Tab + 12-ish 1u keys + top of ISO Enter | Del End PgDn
            {2,  0,  0.00f, 2.35f, 1.50f, 0.95f},
            {2,  1,  1.50f, 2.35f, 1.00f, 0.95f}, {2,  2,  2.50f, 2.35f, 1.00f, 0.95f}, {2,  3,  3.50f, 2.35f, 1.00f, 0.95f}, {2,  4,  4.50f, 2.35f, 1.00f, 0.95f},
            {2,  5,  5.50f, 2.35f, 1.00f, 0.95f}, {2,  6,  6.50f, 2.35f, 1.00f, 0.95f}, {2,  7,  7.50f, 2.35f, 1.00f, 0.95f}, {2,  8,  8.50f, 2.35f, 1.00f, 0.95f},
            {2,  9,  9.50f, 2.35f, 1.00f, 0.95f}, {2, 10, 10.50f, 2.35f, 1.00f, 0.95f}, {2, 11, 11.50f, 2.35f, 1.00f, 0.95f}, {2, 12, 12.50f, 2.35f, 1.00f, 0.95f},
            {2, 12, 13.50f, 2.35f, 1.50f, 0.95f},
            {2, 13, 16.00f, 2.35f, 1.00f, 0.95f}, {2, 14, 17.00f, 2.35f, 1.00f, 0.95f}, {2, 15, 18.00f, 2.35f, 1.00f, 0.95f},

            // Home row: Caps + letters/punctuation + lower part of ISO Enter
            {3,  0,  0.00f, 3.35f, 1.75f, 0.95f},
            {3,  1,  1.75f, 3.35f, 1.00f, 0.95f}, {3,  2,  2.75f, 3.35f, 1.00f, 0.95f}, {3,  3,  3.75f, 3.35f, 1.00f, 0.95f}, {3,  4,  4.75f, 3.35f, 1.00f, 0.95f},
            {3,  5,  5.75f, 3.35f, 1.00f, 0.95f}, {3,  6,  6.75f, 3.35f, 1.00f, 0.95f}, {3,  7,  7.75f, 3.35f, 1.00f, 0.95f}, {3,  8,  8.75f, 3.35f, 1.00f, 0.95f},
            {3,  9,  9.75f, 3.35f, 1.00f, 0.95f}, {3, 10, 10.75f, 3.35f, 1.00f, 0.95f}, {3, 11, 11.75f, 3.35f, 1.00f, 0.95f}, {3, 12, 12.75f, 3.35f, 1.00f, 0.95f},
            {3, 13, 13.75f, 3.35f, 1.25f, 0.95f},

            // Shift row + Up
            {4,  0,  0.00f, 4.35f, 1.25f, 0.95f}, {4,  1,  1.25f, 4.35f, 1.00f, 0.95f},
            {4,  2,  2.25f, 4.35f, 1.00f, 0.95f}, {4,  3,  3.25f, 4.35f, 1.00f, 0.95f}, {4,  4,  4.25f, 4.35f, 1.00f, 0.95f}, {4,  5,  5.25f, 4.35f, 1.00f, 0.95f},
            {4,  6,  6.25f, 4.35f, 1.00f, 0.95f}, {4,  7,  7.25f, 4.35f, 1.00f, 0.95f}, {4,  8,  8.25f, 4.35f, 1.00f, 0.95f}, {4,  9,  9.25f, 4.35f, 1.00f, 0.95f},
            {4, 10, 10.25f, 4.35f, 1.00f, 0.95f}, {4, 11, 11.25f, 4.35f, 1.00f, 0.95f}, {4, 12, 12.25f, 4.35f, 1.00f, 0.95f},
            {4, 15, 13.25f, 4.35f, 1.75f, 0.95f}, {4, 14, 17.00f, 4.35f, 1.00f, 0.95f},

            // Bottom row + arrows
            {5,  0,  0.00f, 5.35f, 1.25f, 0.95f}, {5,  1,  1.25f, 5.35f, 1.25f, 0.95f}, {5,  2,  2.50f, 5.35f, 1.25f, 0.95f},
            {5,  6,  3.75f, 5.35f, 6.25f, 0.95f},
            {5,  9, 10.00f, 5.35f, 1.25f, 0.95f}, {5, 10, 11.25f, 5.35f, 1.25f, 0.95f}, {5, 11, 12.50f, 5.35f, 1.25f, 0.95f}, {5, 12, 13.75f, 5.35f, 1.25f, 0.95f},
            {5, 13, 16.00f, 5.35f, 1.00f, 0.95f}, {5, 14, 17.00f, 5.35f, 1.00f, 0.95f}, {5, 15, 18.00f, 5.35f, 1.00f, 0.95f}
        });

        constexpr float LayoutWidth = 19.00f;
        constexpr float LayoutHeight = 6.35f;
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float previewWidth = std::min(availableWidth * widthFraction, maxWidth);
        const float unit = std::clamp((previewWidth - 2.0f) / LayoutWidth, 7.0f, 28.0f);
        const float gap = unit * 0.045f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        for (const auto& key : Keys)
        {
            const Color32 color = interpolatePreviewColor(framebuffer, key.Row, key.Column, interpolation);
            // Preview interpolation is visual-only: it approximates light bleed/color mixing between
            // neighboring switches without touching the framebuffer actually sent to the keyboard.
            const ImU32 previewColor = color.R == 0 && color.G == 0 && color.B == 0 ? IM_COL32(24, 24, 24, 255) : IM_COL32(color.R, color.G, color.B, 255);
            const ImVec2 min(origin.x + key.X * unit + gap, origin.y + key.Y * unit + gap);
            const ImVec2 max(origin.x + (key.X + key.Width) * unit - gap, origin.y + (key.Y + key.Height) * unit - gap);
            drawList->AddRectFilled(min, max, previewColor, 0.0f);
        }

        ImGui::Dummy(ImVec2(LayoutWidth * unit, LayoutHeight * unit));
    }

    void drawShaderLivePanel(RawUSB& usb, SharedDeviceState& deviceState, const EvdevKeyboard& keyboardInput, const std::array<Color32, MatrixSize>& framebuffer, const float appCpuUsage, const bool scrollLockActive, const bool capsLockActive, VisualizerSettings& settings)
    {
        PerformanceSnapshot performance{};
        bool hasPerformance = false;
        {
            std::lock_guard lock(deviceState.Mutex);
            performance = deviceState.Performance;
            hasPerformance = deviceState.HasPerformance;
        }

        ImGui::SeparatorText("Live output");
        if (ImGui::BeginTable("ShaderLiveOutput", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 430.0f);
            ImGui::TableSetupColumn("Diagnostics", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            drawFramebufferPreview(framebuffer, 1.0f, 410.0f, settings.LiveOutputInterpolation);
            ImGui::SetNextItemWidth(410.0f);
            ImGui::SliderFloat("Live output interpolation##ShaderLive", &settings.LiveOutputInterpolation, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview-only neighboring-key color mixing; USB framebuffer data is unchanged.");

            ImGui::TableNextColumn();
            ImGui::Text("Current shader: %s", settings.ShaderPresetIndex == 0 ? "Custom / current" : ShaderPresets[static_cast<std::size_t>(settings.ShaderPresetIndex - 1)].Name.c_str()); ImGui::SameLine(); ImGui::TextDisabled("[%s]", settings.ShaderId.empty() ? "custom" : settings.ShaderId.c_str());
            ImGui::Text("Firmware: %s   QRPC: v%u", FirmwareVersion, static_cast<unsigned>(ProtocolVersion));
            ImGui::Text("VID:PID: %04X:%04X", VendorId, ProductId);
            ImGui::Text("Global brightness: %.0f%%   Preview interpolation: %.0f%%", settings.GlobalBrightness * 100.0f, settings.LiveOutputInterpolation * 100.0f);
            ImGui::Text("Framebuffer: %zux%zu (%zu cells, %zu active RGB rows)", Columns, Rows, MatrixSize, ActiveProbeRows);
            ImGui::TextUnformatted("Device");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", usb.isConnected() ? (usb.deviceName().empty() ? "Quartz K552X" : usb.deviceName().c_str()) : "Disconnected");
            if (keyboardInput.connected() && !keyboardInput.deviceName().empty())
                ImGui::Text("Input device: %s", keyboardInput.deviceName().c_str());
            ImGui::TextWrapped("Input: %s", keyboardInput.status().c_str());
            ImGui::Text("App CPU: %.2f%%", appCpuUsage);
            ImGui::Text("evdev: Caps %.0f   Scroll %.0f", capsLockActive ? 1.0f : 0.0f, scrollLockActive ? 1.0f : 0.0f);
            ImGui::TextDisabled("Shader: uCapsLock %.0f   uScrollLock %.0f", settings.ShaderKeyStateUniforms && capsLockActive ? 1.0f : 0.0f, settings.ShaderKeyStateUniforms && scrollLockActive ? 1.0f : 0.0f);

            if (!hasPerformance || performance.CoreClock == 0)
                ImGui::TextDisabled("Waiting for keyboard performance data...");
            else
            {
                const double ticksPerMicrosecond = performance.CoreClock / 1'000'000.0;
                const std::uint32_t matrixTicks = performance.BeginScanTicks + performance.ScanTicks + performance.EndScanTicks;
                const std::uint32_t totalTicks = matrixTicks + performance.StateUpdateTicks + performance.HIDTicks;
                const double keyboardCpu = performance.AverageScanPeriodTicks != 0 ? static_cast<double>(totalTicks) / performance.AverageScanPeriodTicks * 100.0 : 0.0;
                const double scanRate = performance.AverageScanPeriodTicks != 0 ? static_cast<double>(performance.CoreClock) / performance.AverageScanPeriodTicks : 0.0;
                ImGui::Text("Keyboard CPU: %.2f%%   Scan: %.1f Hz", keyboardCpu, scanRate);
                ImGui::Text("Matrix %.2f us  [begin %.2f / scan %.2f / end %.2f]", matrixTicks / ticksPerMicrosecond, performance.BeginScanTicks / ticksPerMicrosecond, performance.ScanTicks / ticksPerMicrosecond, performance.EndScanTicks / ticksPerMicrosecond);
                ImGui::Text("State %.2f us   HID %.2f us", performance.StateUpdateTicks / ticksPerMicrosecond, performance.HIDTicks / ticksPerMicrosecond);
                ImGui::Text("RGB %.2f us   slot max %.2f us   period %.2f us", performance.RGBTicks / ticksPerMicrosecond, performance.RGBSlotMaxTicks / ticksPerMicrosecond, performance.AverageScanPeriodTicks / ticksPerMicrosecond);
            }
            ImGui::EndTable();
        }
    }

    void drawPerformance(const PerformanceSnapshot& stats)
    {
        if (stats.CoreClock == 0)
        {
            ImGui::TextDisabled("No performance response yet.");
            return;
        }
        const double ticksPerMicrosecond = stats.CoreClock / 1'000'000.0;
        const std::uint32_t matrixTicks = stats.BeginScanTicks + stats.ScanTicks + stats.EndScanTicks;
        const std::uint32_t totalTicks = matrixTicks + stats.StateUpdateTicks + stats.HIDTicks;
        const double cpuUsage = stats.AverageScanPeriodTicks != 0 ? static_cast<double>(totalTicks) / stats.AverageScanPeriodTicks * 100.0 : 0.0;
        const double scanRate = stats.AverageScanPeriodTicks != 0 ? static_cast<double>(stats.CoreClock) / stats.AverageScanPeriodTicks : 0.0;
        ImGui::Text("Core clock %.2f MHz", stats.CoreClock / 1'000'000.0);
        ImGui::Text("Scan rate %.2f Hz", scanRate);
        ImGui::Text("CPU %.2f%%", cpuUsage);
        ImGui::ProgressBar(static_cast<float>(std::clamp(cpuUsage / 100.0, 0.0, 1.0)), ImVec2(-1.0f, 0.0f));
        if (ImGui::BeginTable("PerformanceTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Stage");
            ImGui::TableSetupColumn("Ticks");
            ImGui::TableSetupColumn("us");
            ImGui::TableHeadersRow();
            const auto row = [&](const char* name, const std::uint32_t ticks)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
                ImGui::TableNextColumn(); ImGui::Text("%u", ticks);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", ticks / ticksPerMicrosecond);
            };
            row("Begin", stats.BeginScanTicks);
            row("Scan", stats.ScanTicks);
            row("End", stats.EndScanTicks);
            row("State", stats.StateUpdateTicks);
            row("HID", stats.HIDTicks);
            row("RGB driver", stats.RGBTicks);
            row("RGB slot max", stats.RGBSlotMaxTicks);
            row("Total", totalTicks);
            row("Period", stats.AverageScanPeriodTicks);
            ImGui::EndTable();
        }
    }

    void drawTimingProbe(const MatrixTimingProbeResult<ActiveProbeRows>& probe)
    {
        if (probe.CoreClock == 0)
        {
            ImGui::TextDisabled("No matrix timing probe result yet.");
            return;
        }
        const double ticksPerMicrosecond = probe.CoreClock / 1'000'000.0;
        if (ImGui::BeginTable("TimingProbe", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Row");
            ImGui::TableSetupColumn("Col");
            ImGui::TableSetupColumn("Min us");
            ImGui::TableSetupColumn("Max us");
            ImGui::TableSetupColumn("Samples");
            ImGui::TableSetupColumn("Timeouts");
            ImGui::TableSetupColumn("Suggested us");
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < ActiveProbeRows; ++i)
            {
                const auto& result = probe.Rows[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%zu", i + 1);
                if (result.Column == 0xFF || result.Samples == 0)
                {
                    ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                    for (int column = 2; column < 7; ++column) { ImGui::TableNextColumn(); ImGui::TextDisabled("-"); }
                    continue;
                }
                const double minUs = result.MinTicks / ticksPerMicrosecond;
                const double maxUs = result.MaxTicks / ticksPerMicrosecond;
                ImGui::TableNextColumn(); ImGui::Text("%u", result.Column);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", minUs);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", maxUs);
                ImGui::TableNextColumn(); ImGui::Text("%u", result.Samples);
                ImGui::TableNextColumn(); ImGui::Text("%u", result.Timeouts);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", maxUs + 5.0);
            }
            ImGui::EndTable();
        }
    }


    const char* packetTypeLabel(const std::uint16_t type)
    {
        if (type == static_cast<std::uint16_t>(PacketType::FramebufferSet)) return "FramebufferSet";
        if (type == static_cast<std::uint16_t>(PacketType::PerformanceRequest)) return "PerformanceRequest";
        if (type == static_cast<std::uint16_t>(PacketType::PerformanceResponse)) return "PerformanceResponse";
        if (type == static_cast<std::uint16_t>(PacketType::MatrixTimingProbeResult)) return "MatrixTimingProbeResult";
        return "Other";
    }

    void selectRuntimeProcess(RuntimeBinding& binding, const RuntimeProcessInfo& process)
    {
        binding.ProcessId = static_cast<int>(process.Pid);
        std::snprintf(binding.ProcessName, sizeof(binding.ProcessName), "%s", process.Name.c_str());
        binding.NextProcessSearch = 0.0;
        captureRuntimeRebindPattern(binding, process);
    }

    bool drawRuntimeTargetSelector(RuntimeBinding& binding, ShaderFramebuffer& shaderFramebuffer)
    {
        bool changed = false;
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::BeginCombo("Uniform id", binding.TargetId[0] ? binding.TargetId : "<select material uniform>"))
        {
            for (const auto& parameter : shaderFramebuffer.materialParameters())
            {
                const bool selected = parameter.PersistenceKey == binding.TargetId;
                const std::string label = parameter.Label + "  [" + parameter.PersistenceKey + "]";
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    std::snprintf(binding.TargetId, sizeof(binding.TargetId), "%s", parameter.PersistenceKey.c_str());
                    binding.TargetComponent = std::clamp(binding.TargetComponent, 0, std::max(parameter.Components - 1, 0));
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (const auto* parameter = shaderFramebuffer.findMaterialParameter(binding.TargetId))
        {
            if (parameter->Components > 1)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100.0f);
                changed |= ImGui::SliderInt("Component", &binding.TargetComponent, 0, parameter->Components - 1);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s / %s", parameter->Label.c_str(), parameter->Integer ? (parameter->Boolean ? "bool" : "int") : "float");
        }
        else
            ImGui::TextDisabled("Current shader does not expose this material id; the binding will wait.");
        return changed;
    }

    bool drawRuntimeProcessSelector(RuntimeBinding& binding, std::vector<RuntimeProcessInfo>& processes)
    {
        bool changed = false;
        if (ImGui::SmallButton("Refresh processes")) processes = enumerateRuntimeProcesses();
        ImGui::SameLine();
        const std::string loweredSearch = runtimeLower(binding.ProcessSearch);
        const std::size_t matching = static_cast<std::size_t>(std::ranges::count_if(processes, [&](const RuntimeProcessInfo& process) { return runtimeProcessMatchesSearch(process, loweredSearch); }));
        ImGui::TextDisabled("%zu processes / %zu matching", processes.size(), matching);

        const auto selectedIt = std::ranges::find_if(processes, [&](const RuntimeProcessInfo& process) { return process.Pid == binding.ProcessId; });
        const std::string current = selectedIt != processes.end() ? std::to_string(selectedIt->Pid) + "  " + runtimeProcessDisplayTitle(*selectedIt) : binding.ProcessId > 0 ? std::to_string(binding.ProcessId) + "  " + binding.ProcessName : "<select process>";
        ImGui::SetNextItemWidth(690.0f);
        if (ImGui::BeginCombo("Process", current.c_str(), ImGuiComboFlags_HeightLargest))
        {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##ProcessSearch", "Find PID, process name, title/argv[0], executable path or command line...", binding.ProcessSearch, sizeof(binding.ProcessSearch));
            ImGui::Separator();
            if (ImGui::BeginTable("ProcessPicker", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(780.0f, 330.0f)))
            {
                ImGui::TableSetupColumn("Process / title", ImGuiTableColumnFlags_WidthStretch, 0.42f);
                ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 72.0f);
                ImGui::TableSetupColumn("Executable / command", ImGuiTableColumnFlags_WidthStretch, 0.58f);
                ImGui::TableHeadersRow();
                for (const auto& process : processes)
                {
                    if (!runtimeProcessMatchesSearch(process, loweredSearch)) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const std::string display = runtimeProcessDisplayTitle(process) + "##Process" + std::to_string(process.Pid);
                    if (ImGui::Selectable(display.c_str(), process.Pid == binding.ProcessId))
                    {
                        selectRuntimeProcess(binding, process);
                        changed = true;
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("PID: %d", static_cast<int>(process.Pid));
                        ImGui::Text("Name: %s", process.Name.c_str());
                        ImGui::TextWrapped("Title / argv[0]: %s", process.Title.c_str());
                        ImGui::TextWrapped("Executable: %s", process.Exe.c_str());
                        ImGui::TextWrapped("Command line: %s", process.CommandLine.c_str());
                        ImGui::EndTooltip();
                    }
                    ImGui::TableNextColumn(); ImGui::Text("%d", static_cast<int>(process.Pid));
                    ImGui::TableNextColumn();
                    if (!process.Exe.empty()) ImGui::TextUnformatted(process.Exe.c_str());
                    if (!process.CommandLine.empty() && process.CommandLine != process.Exe)
                    {
                        if (!process.Exe.empty()) ImGui::TextDisabled("%s", process.CommandLine.c_str());
                        else ImGui::TextUnformatted(process.CommandLine.c_str());
                    }
                }
                ImGui::EndTable();
            }
            if (matching == 0) ImGui::TextDisabled("No process matches '%s'.", binding.ProcessSearch);
            ImGui::EndCombo();
        }

        changed |= ImGui::Checkbox("Auto rebind when process restarts", &binding.AutoReattach);
        if (binding.AutoReattach)
        {
            int mode = static_cast<int>(binding.RebindMode);
            ImGui::SetNextItemWidth(300.0f);
            if (ImGui::BeginCombo("Rebind using", runtimeRebindModeName(binding.RebindMode)))
            {
                for (int i = 0; i <= static_cast<int>(ProcessRebindMode::AnyRegex); ++i)
                {
                    const auto candidate = static_cast<ProcessRebindMode>(i);
                    if (ImGui::Selectable(runtimeRebindModeName(candidate), mode == i))
                    {
                        binding.RebindMode = candidate;
                        mode = i;
                        const auto selected = std::ranges::find_if(processes, [&](const RuntimeProcessInfo& process) { return process.Pid == binding.ProcessId; });
                        if (selected != processes.end()) captureRuntimeRebindPattern(binding, *selected);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(690.0f);
            changed |= ImGui::InputText(runtimeRebindModeIsRegex(binding.RebindMode) ? "Rebind regex" : "Rebind value", binding.ProcessRebindPattern, sizeof(binding.ProcessRebindPattern));
            if (runtimeRebindModeIsRegex(binding.RebindMode) && binding.ProcessRebindPattern[0])
            {
                try { [[maybe_unused]] const std::regex test(binding.ProcessRebindPattern, std::regex::ECMAScript | std::regex::icase); ImGui::SameLine(); ImGui::TextDisabled("regex OK"); }
                catch (const std::regex_error& e) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.48f, 1.0f), "invalid regex: %s", e.what()); }
            }
            ImGui::TextDisabled("If the PID disappears Quartz searches about once per second and rebinds to the newest matching process.");
        }
        return changed;
    }

    const char* runtimeParameterSlotName(const RuntimeParameterSlot slot) noexcept
    {
        switch (slot)
        {
        case RuntimeParameterSlot::Normalize: return "Normalize";
        case RuntimeParameterSlot::InputMin: return "Input min";
        case RuntimeParameterSlot::InputMax: return "Input max";
        case RuntimeParameterSlot::Invert: return "Invert";
        case RuntimeParameterSlot::Scale: return "Scale";
        case RuntimeParameterSlot::Offset: return "Offset";
        case RuntimeParameterSlot::Clamp: return "Clamp";
        case RuntimeParameterSlot::OutputMin: return "Output min";
        case RuntimeParameterSlot::OutputMax: return "Output max";
        case RuntimeParameterSlot::SmoothingHz: return "Smoothing";
        case RuntimeParameterSlot::UpdateHz: return "Update rate";
        case RuntimeParameterSlot::Count: break;
        }
        return "Parameter";
    }

    bool drawRuntimeParameterLink(RuntimeBindingEngine& engine, RuntimeBinding& owner, const RuntimeParameterSlot slot)
    {
        bool changed = false;
        auto& link = owner.ParameterLinks[static_cast<std::size_t>(slot)];
        std::string preview = "Local";
        if (link.Enabled)
        {
            if (const auto* source = engine.findBinding(link.BindingId))
            {
                preview = source->Name[0] ? std::string(source->Name) : ("Binding " + std::to_string(source->Id));
                if (source->HasValue)
                {
                    char value[48];
                    std::snprintf(value, sizeof(value), " = %.4g", source->Value);
                    preview += value;
                }
            }
            else preview = "Missing binding";
        }
        ImGui::PushID(static_cast<int>(slot));
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("##ParameterSource", preview.c_str()))
        {
            if (ImGui::Selectable("Local value", !link.Enabled))
            {
                engine.setParameterLink(owner, slot, 0);
                changed = true;
            }
            for (const auto& candidate : engine.bindings())
            {
                if (candidate.Id == owner.Id) continue;
                const bool selected = link.Enabled && link.BindingId == candidate.Id;
                const bool allowed = selected || engine.canParameterLink(owner.Id, candidate.Id);
                if (!allowed) ImGui::BeginDisabled();
                std::string label = std::string(candidate.Name[0] ? candidate.Name : "Binding") + "  [#" + std::to_string(candidate.Id) + "]";
                if (ImGui::Selectable(label.c_str(), selected) && allowed)
                {
                    engine.setParameterLink(owner, slot, candidate.Id);
                    changed = true;
                }
                if (!allowed && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Blocked because this dependency would create a circular binding loop.");
                if (!allowed) ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use another binding's output as %s. Local value is used until the linked binding has produced a value.", runtimeParameterSlotName(slot));
        ImGui::PopID();
        return changed;
    }

    bool drawRuntimeLinkedFloat(RuntimeBindingEngine& engine, RuntimeBinding& binding, const RuntimeParameterSlot slot, const char* label, float& value, const float speed , const char* format)
    {
        bool changed = false;
        ImGui::SetNextItemWidth(180.0f);
        changed |= ImGui::DragFloat(label, &value, speed, 0.0f, 0.0f, format);
        ImGui::SameLine();
        changed |= drawRuntimeParameterLink(engine, binding, slot);
        return changed;
    }

    bool drawRuntimeLinkedBool(RuntimeBindingEngine& engine, RuntimeBinding& binding, const RuntimeParameterSlot slot, const char* label, bool& value)
    {
        bool changed = ImGui::Checkbox(label, &value);
        ImGui::SameLine();
        changed |= drawRuntimeParameterLink(engine, binding, slot);
        return changed;
    }

    bool drawRuntimeBindingReferenceCombo(RuntimeBindingEngine& engine, const char* label, std::uint64_t& id, const std::uint64_t excludeId , const bool unboundOnly)
    {
        bool changed = false;
        const RuntimeBinding* selected = engine.findBinding(id);
        ImGui::SetNextItemWidth(320.0f);
        if (ImGui::BeginCombo(label, selected ? selected->Name : "<select binding>"))
        {
            if (ImGui::Selectable("<none>", id == 0)) { id = 0; changed = true; }
            for (const auto& candidate : engine.bindings())
            {
                if (candidate.Id == excludeId || (unboundOnly && candidate.Source != RuntimeSourceKind::Unbound)) continue;
                const bool isSelected = candidate.Id == id;
                std::string display = std::string(candidate.Name) + "  [#" + std::to_string(candidate.Id) + ", p" + std::to_string(candidate.Priority) + "]";
                if (ImGui::Selectable(display.c_str(), isSelected)) { id = candidate.Id; changed = true; }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    bool drawRuntimeControlReferenceCombo(RuntimeBindingEngine& engine, const char* label, std::uint64_t& id)
    {
        bool changed = false;
        const RuntimeControlRule* selected = engine.findControl(id);
        ImGui::SetNextItemWidth(320.0f);
        if (ImGui::BeginCombo(label, selected ? selected->Name : "<select control>"))
        {
            if (ImGui::Selectable("<none>", id == 0)) { id = 0; changed = true; }
            for (const auto& candidate : engine.controls())
            {
                const bool isSelected = candidate.Id == id;
                std::string display = std::string(candidate.Name) + "  [#" + std::to_string(candidate.Id) + ", p" + std::to_string(candidate.Priority) + "]";
                if (ImGui::Selectable(display.c_str(), isSelected)) { id = candidate.Id; changed = true; }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    bool drawRuntimeBankReferenceCombo(RuntimeBindingEngine& engine, const char* label, std::uint64_t& id)
    {
        bool changed = false;
        const RuntimeValueBankEntry* selected = engine.findBankValue(id);
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::BeginCombo(label, selected ? selected->Name : "<select bank value>"))
        {
            if (ImGui::Selectable("<none>", id == 0)) { id = 0; changed = true; }
            for (const auto& value : engine.bank())
            {
                const bool isSelected = value.Id == id;
                std::string display = std::string(value.Name) + "  [" + runtimeBankValueTypeName(value.Type) + "]";
                if (ImGui::Selectable(display.c_str(), isSelected)) { id = value.Id; changed = true; }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    ImVec4 runtimeStateColor(const bool enabled, const bool good, const bool error , const bool waiting)
    {
        if (!enabled) return ImVec4(0.38f, 0.38f, 0.38f, 1.0f);
        if (error) return ImVec4(0.92f, 0.18f, 0.18f, 1.0f);
        if (waiting) return ImVec4(0.95f, 0.65f, 0.14f, 1.0f);
        return good ? ImVec4(0.15f, 0.78f, 0.30f, 1.0f) : ImVec4(0.82f, 0.20f, 0.20f, 1.0f);
    }

    bool runtimeBindingLooksBoolean(const RuntimeBinding& binding) noexcept
    {
        if (binding.Source == RuntimeSourceKind::MassCompare)
            return binding.CompareResult == RuntimeMassCompareResult::Any || binding.CompareResult == RuntimeMassCompareResult::All || binding.CompareResult == RuntimeMassCompareResult::None;
        if (binding.Source == RuntimeSourceKind::Aggregate)
            return binding.AggregateOperation == RuntimeAggregateOperation::Any || binding.AggregateOperation == RuntimeAggregateOperation::All;
        if (binding.Source == RuntimeSourceKind::BindingStatus) return (binding.Signal >= 0 && binding.Signal <= 6) || binding.Signal == 9;
        if (binding.Source == RuntimeSourceKind::BindingValue) return binding.Signal >= 2 && binding.Signal <= 4;
        if (binding.Source == RuntimeSourceKind::ControlStatus) return binding.Signal >= 0 && binding.Signal <= 2;
        if (binding.Source == RuntimeSourceKind::ObjectStatus) return binding.Signal == 0 || binding.Signal == 3 || binding.Signal == 4;
        if (binding.Source == RuntimeSourceKind::USB && binding.Signal == 0) return true;
        if (binding.Source == RuntimeSourceKind::Keyboard && (binding.Signal == 0 || binding.Signal == 1)) return true;
        if (binding.Source == RuntimeSourceKind::Media && binding.Signal == 4) return true;
        if (binding.Source == RuntimeSourceKind::ShaderState && (binding.Signal == 1 || binding.Signal == 2 || binding.Signal == 6)) return true;
        if (binding.Source == RuntimeSourceKind::ValueBank && (binding.Signal == 1 || binding.Signal == 3 || binding.Signal == 4 || binding.Signal == 6)) return true;
        if (binding.Source == RuntimeSourceKind::StringConstant && binding.Signal == 0) return true;
        if (binding.Source == RuntimeSourceKind::ProfileState && (binding.Signal == 1 || binding.Signal == 2)) return true;
        return false;
    }
    void drawRuntimeStateSquare(const char* id, const ImVec4 color)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
        ImGui::Button(id, ImVec2(11.0f, 11.0f));
        ImGui::PopStyleColor(3);
    }

    bool drawRuntimeActionList(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer, std::vector<RuntimeAction>& actions, const std::uint64_t sourceBindingId, const bool controlOwner)
    {
        bool changed = false;
        if (ImGui::SmallButton("Add action")) { actions.emplace_back(); actions.back().When = controlOwner ? RuntimeActionWhen::OnTrigger : RuntimeActionWhen::OnUpdate; changed = true; }
        std::optional<std::size_t> erase;
        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            auto& action = actions[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Separator();
            changed |= ImGui::Checkbox("##actionEnabled", &action.Enabled);
            ImGui::SameLine(); ImGui::TextDisabled("Action %zu", i + 1);
            ImGui::SameLine(); int when = static_cast<int>(action.When); ImGui::SetNextItemWidth(125.0f);
            if (ImGui::Combo("##actionWhen", &when, "While active\0On trigger\0On update\0On change\0While truthy\0While falsy\0")) { action.When = static_cast<RuntimeActionWhen>(when); changed = true; }
            ImGui::SameLine(); int target = static_cast<int>(action.Target); ImGui::SetNextItemWidth(170.0f);
            if (ImGui::Combo("##actionTarget", &target, "Active shader\0Binding enabled\0Global brightness\0Send framebuffer\0Base color mode\0Material parameter\0Unbound binding value\0Value bank\0Control enabled\0Refresh binding\0Force binding update\0Invalidate binding\0Reset binding state\0Retry register capture\0Rescan binding pattern\0Rebind process\0Clear binding error\0")) { action.Target = static_cast<RuntimeActionTarget>(target); changed = true; }
            ImGui::SameLine(); if (ImGui::SmallButton("Remove action")) erase = i;

            const bool valueUseful = !runtimeActionTargetIsBindingOperation(action.Target);
            if (valueUseful)
            {
                int mode = static_cast<int>(action.ValueMode); ImGui::SetNextItemWidth(180.0f);
                if (ImGui::Combo("Value source", &mode, "Constant\0Owner/source value\0Another binding\0Value bank\0")) { action.ValueMode = static_cast<RuntimeActionValueMode>(mode); changed = true; }
                if (action.ValueMode == RuntimeActionValueMode::BindingValue) changed |= drawRuntimeBindingReferenceCombo(engine, "Value binding", action.ValueBindingId, sourceBindingId);
                else if (action.ValueMode == RuntimeActionValueMode::BankValue) changed |= drawRuntimeBankReferenceCombo(engine, "Value bank source", action.BankValueId);
            }

            if (action.Target == RuntimeActionTarget::ActiveShader)
            {
                if (action.ValueMode == RuntimeActionValueMode::Constant)
                {
                    const ShaderPreset* shaderById = action.ShaderId[0] ? findShaderPresetById(action.ShaderId) : nullptr;
                    const char* preview = action.ShaderPresetIndex == -1 ? "<previous shader>" : shaderById ? shaderById->Name.c_str() : action.ShaderPresetIndex > 0 && action.ShaderPresetIndex <= static_cast<int>(ShaderPresets.size()) ? ShaderPresets[static_cast<std::size_t>(action.ShaderPresetIndex - 1)].Name.c_str() : "<select shader>";
                    ImGui::SetNextItemWidth(300.0f);
                    if (ImGui::BeginCombo("Shader", preview))
                    {
                        if (ImGui::Selectable("<previous shader>", action.ShaderPresetIndex == -1)) { action.ShaderPresetIndex = -1; action.ShaderId[0] = '\0'; changed = true; }
                        for (std::size_t p = 0; p < ShaderPresets.size(); ++p) { const bool selected = action.ShaderPresetIndex == static_cast<int>(p + 1); if (ImGui::Selectable(ShaderPresets[p].Name.c_str(), selected)) { action.ShaderPresetIndex = static_cast<int>(p + 1); std::snprintf(action.ShaderId, sizeof(action.ShaderId), "%s", ShaderPresets[p].Id.c_str()); changed = true; } }
                        ImGui::EndCombo();
                    }
                }
                ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Crossfade", &action.TransitionSeconds, 0.02f, 0.0f, 10.0f, "%.2f s");
            }
            else if (action.Target == RuntimeActionTarget::BindingEnabled)
            {
                changed |= drawRuntimeBindingReferenceCombo(engine, "Target binding", action.TargetBindingId, sourceBindingId);
                if (action.ValueMode == RuntimeActionValueMode::Constant) changed |= ImGui::Checkbox("Enabled value", &action.BoolValue);
            }
            else if (action.Target == RuntimeActionTarget::GlobalBrightness)
            {
                if (action.ValueMode == RuntimeActionValueMode::Constant) changed |= ImGui::SliderFloat("Brightness", &action.Value, 0.0f, 1.0f, "%.3f");
            }
            else if (action.Target == RuntimeActionTarget::SendFramebuffer)
            {
                if (action.ValueMode == RuntimeActionValueMode::Constant) changed |= ImGui::Checkbox("Send framebuffer", &action.BoolValue);
            }
            else if (action.Target == RuntimeActionTarget::BaseColorMode)
            {
                if (action.ValueMode == RuntimeActionValueMode::Constant) { int mode = std::clamp(static_cast<int>(std::lround(action.Value)), 0, 2); if (ImGui::Combo("Base color mode", &mode, "RGB wave\0Solid\0Shader\0")) { action.Value = static_cast<float>(mode); changed = true; } }
            }
            else if (action.Target == RuntimeActionTarget::MaterialParameter)
            {
                ImGui::SetNextItemWidth(300.0f);
                if (ImGui::BeginCombo("Material id", action.TargetId[0] ? action.TargetId : "<select material uniform>"))
                {
                    for (const auto& parameter : shaderFramebuffer.materialParameters())
                    {
                        const bool selected = parameter.PersistenceKey == action.TargetId;
                        const std::string label = parameter.Label + "  [" + parameter.PersistenceKey + "]";
                        if (ImGui::Selectable(label.c_str(), selected)) { std::snprintf(action.TargetId, sizeof(action.TargetId), "%s", parameter.PersistenceKey.c_str()); action.TargetComponent = std::clamp(action.TargetComponent, 0, std::max(parameter.Components - 1, 0)); changed = true; }
                    }
                    ImGui::EndCombo();
                }
                if (const auto* parameter = shaderFramebuffer.findMaterialParameter(action.TargetId); parameter && parameter->Components > 1) { ImGui::SameLine(); changed |= ImGui::SliderInt("Component", &action.TargetComponent, 0, parameter->Components - 1); }
                if (action.ValueMode == RuntimeActionValueMode::Constant) { ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Value", &action.Value, 0.01f); }
            }
            else if (action.Target == RuntimeActionTarget::BindingValue)
            {
                changed |= drawRuntimeBindingReferenceCombo(engine, "Unbound target", action.TargetBindingId, sourceBindingId, true);
                if (action.ValueMode == RuntimeActionValueMode::Constant) { ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Value", &action.Value, 0.01f); }
            }
            else if (action.Target == RuntimeActionTarget::ValueBank)
            {
                changed |= drawRuntimeBankReferenceCombo(engine, "Bank target", action.TargetBankValueId);
                if (action.ValueMode == RuntimeActionValueMode::Constant)
                {
                    if (const RuntimeValueBankEntry* bank = engine.findBankValue(action.TargetBankValueId); bank && bank->Type == RuntimeBankValueType::String) changed |= ImGui::InputText("String value", action.StringValue, sizeof(action.StringValue));
                    else { ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Value", &action.Value, 0.01f); }
                }
            }
            else if (action.Target == RuntimeActionTarget::ControlEnabled)
            {
                changed |= drawRuntimeControlReferenceCombo(engine, "Target control", action.TargetControlId);
                if (action.ValueMode == RuntimeActionValueMode::Constant) changed |= ImGui::Checkbox("Enabled value", &action.BoolValue);
            }
            else if (runtimeActionTargetIsBindingOperation(action.Target))
            {
                changed |= drawRuntimeBindingReferenceCombo(engine, "Target binding", action.TargetBindingId, sourceBindingId);
                ImGui::TextDisabled("Binding operation; value source is ignored.");
            }
            ImGui::PopID();
        }
        if (erase) { actions.erase(actions.begin() + static_cast<std::ptrdiff_t>(*erase)); changed = true; }
        return changed;
    }

    bool drawRuntimeReferenceList(RuntimeBindingEngine& engine, RuntimeBinding& binding, const bool showWeights, const bool massCompare)
    {
        bool changed = false;
        if (ImGui::SmallButton("Add binding member")) { binding.References.push_back({RuntimeReferenceKind::Binding, 0, 0, 1.0f, true}); changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Add control member")) { binding.References.push_back({RuntimeReferenceKind::Control, 0, 0, 1.0f, true}); changed = true; }
        std::optional<std::size_t> erase;
        for (std::size_t i = 0; i < binding.References.size(); ++i)
        {
            auto& reference = binding.References[i];
            ImGui::PushID(static_cast<int>(i));
            changed |= ImGui::Checkbox("##enabled", &reference.Enabled);
            ImGui::SameLine();
            int kind = static_cast<int>(reference.Kind);
            ImGui::SetNextItemWidth(95.0f);
            if (ImGui::Combo("##kind", &kind, "Binding\0Control\0")) { reference.Kind = static_cast<RuntimeReferenceKind>(kind); reference.Id = 0; reference.Signal = 0; changed = true; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(245.0f);
            if (reference.Kind == RuntimeReferenceKind::Binding)
            {
                const RuntimeBinding* selected = engine.findBinding(reference.Id);
                if (ImGui::BeginCombo("##member", selected ? selected->Name : "<select binding>"))
                {
                    for (const auto& candidate : engine.bindings())
                    {
                        if (candidate.Id == binding.Id) continue;
                        const bool isSelected = candidate.Id == reference.Id;
                        if (ImGui::Selectable(candidate.Name, isSelected)) { reference.Id = candidate.Id; changed = true; }
                        if (isSelected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(135.0f);
                static constexpr const char* Signals[] = {"Value", "Raw", "Has value", "Read ok", "Enabled", "Priority", "Has address", "Has error", "Seconds since success"};
                reference.Signal = std::clamp(reference.Signal, 0, static_cast<int>(std::size(Signals)) - 1);
                if (ImGui::Combo("##memberSignal", &reference.Signal, Signals, static_cast<int>(std::size(Signals)))) changed = true;
            }
            else
            {
                const RuntimeControlRule* selected = engine.findControl(reference.Id);
                if (ImGui::BeginCombo("##member", selected ? selected->Name : "<select control>"))
                {
                    for (const auto& candidate : engine.controls())
                    {
                        const bool isSelected = candidate.Id == reference.Id;
                        if (ImGui::Selectable(candidate.Name, isSelected)) { reference.Id = candidate.Id; changed = true; }
                        if (isSelected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(135.0f);
                static constexpr const char* Signals[] = {"Active", "Triggered", "Enabled", "Trigger count", "Priority", "Seconds since trigger", "Source value"};
                reference.Signal = std::clamp(reference.Signal, 0, static_cast<int>(std::size(Signals)) - 1);
                if (ImGui::Combo("##memberSignal", &reference.Signal, Signals, static_cast<int>(std::size(Signals)))) changed = true;
            }
            if (showWeights)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                changed |= ImGui::DragFloat("##weight", &reference.Weight, 0.01f, -1000.0f, 1000.0f, "w %.2f");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) erase = i;

            if (massCompare)
            {
                ImGui::Indent(24.0f);
                changed |= ImGui::Checkbox("Own comparison", &reference.UseOwnComparison);
                if (reference.UseOwnComparison)
                {
                    ImGui::SameLine();
                    int condition = static_cast<int>(reference.CompareCondition);
                    ImGui::SetNextItemWidth(105.0f);
                    if (ImGui::Combo("##compareCondition", &condition, "==\0!=\0<\0<=\0>\0>=\0between\0outside\0")) { reference.CompareCondition = static_cast<RuntimeCompareCondition>(condition); changed = true; }
                    ImGui::SameLine(); ImGui::SetNextItemWidth(100.0f); changed |= ImGui::DragFloat("A##memberCompare", &reference.CompareA, 0.01f);
                    if (reference.CompareCondition == RuntimeCompareCondition::Between || reference.CompareCondition == RuntimeCompareCondition::Outside)
                    { ImGui::SameLine(); ImGui::SetNextItemWidth(100.0f); changed |= ImGui::DragFloat("B##memberCompare", &reference.CompareB, 0.01f); }
                    if (reference.CompareCondition == RuntimeCompareCondition::Equal || reference.CompareCondition == RuntimeCompareCondition::NotEqual)
                    { ImGui::SameLine(); ImGui::SetNextItemWidth(100.0f); changed |= ImGui::DragFloat("Tol##memberCompare", &reference.CompareTolerance, 0.0001f, 0.000001f, 1000.0f, "%.4g"); }
                }
                ImGui::Unindent(24.0f);
            }
            ImGui::PopID();
        }
        if (erase) { binding.References.erase(binding.References.begin() + static_cast<std::ptrdiff_t>(*erase)); changed = true; }
        return changed;
    }

    bool drawRuntimeBinding(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer, RuntimeBinding& binding, bool& erase)
    {
        bool changed = false;
        ImGui::PushID("RuntimeBinding");
        ImGui::PushID(static_cast<int>(binding.Id & 0x7fffffffULL));
        const bool booleanState = runtimeBindingLooksBoolean(binding);
        const bool runtimeEnabled = binding.Enabled && binding.RuntimeEnabled;
        const bool waitingState = runtimeEnabled && !binding.HasValue && binding.Error.empty();
        const bool goodState = binding.HasValue && (!booleanState || binding.Value >= 0.5f);
        const ImVec4 stateColor = runtimeStateColor(runtimeEnabled, goodState, !binding.Error.empty(), waitingState);
        drawRuntimeStateSquare("##bindingState", stateColor);
        ImGui::SameLine();
        std::string bindingHeader = std::string(binding.Name[0] ? binding.Name : "Binding") + "  [p" + std::to_string(binding.Priority) + "]";
        if (!binding.Enabled) bindingHeader += "  DISABLED";
        else if (!binding.RuntimeEnabled) bindingHeader += "  RUNTIME OFF";
        else if (!binding.Error.empty()) bindingHeader += "  ERROR";
        else if (!binding.HasValue) bindingHeader += "  WAITING";
        else if (booleanState) bindingHeader += binding.Value >= 0.5f ? "  TRUE" : "  FALSE";
        else { char value[64]; std::snprintf(value, sizeof(value), "  %.5g", binding.Value); bindingHeader += value; }
        bindingHeader += "###RuntimeBinding" + std::to_string(binding.Id);
        if (!ImGui::CollapsingHeader(bindingHeader.c_str()))
        {
            ImGui::PopID();
            ImGui::PopID();
            return false;
        }
        ImGui::Indent(10.0f);
        ImGui::TextColored(stateColor, "%s", !binding.Enabled ? "Disabled" : !binding.RuntimeEnabled ? "Runtime disabled by control" : !binding.Error.empty() ? "Error" : !binding.HasValue ? "Waiting for value" : booleanState ? (binding.Value >= 0.5f ? "True / active" : "False / inactive") : "Ready");
        ImGui::SameLine();
        if (binding.HasValue) ImGui::TextDisabled("raw %.6g  value %.6g", binding.RawValue, binding.Value);
        if (binding.HasString) { ImGui::SameLine(); ImGui::TextDisabled("string: %s", binding.StringValue.c_str()); }
        if (!binding.Error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "%s", binding.Error.c_str());
        changed |= ImGui::Checkbox("Enabled", &binding.Enabled);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) erase = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        changed |= ImGui::InputText("Name", binding.Name, sizeof(binding.Name));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        changed |= ImGui::InputInt("Priority", &binding.Priority);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lower priorities run first. Higher priorities run later and win material/output conflicts.");
        ImGui::SameLine(); ImGui::SetNextItemWidth(160.0f); changed |= ImGui::InputText("Group", binding.Group, sizeof(binding.Group));
        ImGui::SameLine(); ImGui::SetNextItemWidth(80.0f); changed |= ImGui::InputInt("Order", &binding.Order); ImGui::SameLine(); if (ImGui::SmallButton("Up##bindingOrder")) { --binding.Order; changed = true; } ImGui::SameLine(); if (ImGui::SmallButton("Down##bindingOrder")) { ++binding.Order; changed = true; }

        ImGui::SeparatorText("Source");
        int source = static_cast<int>(binding.Source);
        ImGui::SetNextItemWidth(250.0f);
        if (ImGui::BeginCombo("Source", runtimeSourceName(binding.Source)))
        {
            for (int i = 0; i <= static_cast<int>(RuntimeSourceKind::ProfileState); ++i)
            {
                const auto candidate = static_cast<RuntimeSourceKind>(i);
                if (ImGui::Selectable(runtimeSourceName(candidate), i == source))
                {
                    binding.Source = candidate;
                    binding.Signal = 0;
                    binding.HasValue = false;
                    binding.LastReadSucceeded = false;
                    binding.HasAddress = false;
                    binding.AddressValue = 0;
                    binding.NextUpdate = 0.0;
                    source = i;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }

        const auto signals = runtimeSignalNames(binding.Source);
        binding.Signal = std::clamp(binding.Signal, 0, std::max(static_cast<int>(signals.size()) - 1, 0));
        if (signals.size() > 1)
        {
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::BeginCombo("Signal", signals[static_cast<std::size_t>(binding.Signal)].data()))
            {
                for (int i = 0; i < static_cast<int>(signals.size()); ++i)
                    if (ImGui::Selectable(signals[static_cast<std::size_t>(i)].data(), binding.Signal == i)) { binding.Signal = i; changed = true; }
                ImGui::EndCombo();
            }
        }

        if (binding.Source == RuntimeSourceKind::Constant)
            changed |= ImGui::DragFloat("Constant value", &binding.Constant, 0.01f);
        else if (binding.Source == RuntimeSourceKind::Unbound)
        {
            ImGui::SeparatorText("Writable value");
            changed |= ImGui::DragFloat("Stored value", &binding.UnboundValue, 0.01f);
            ImGui::TextDisabled("Controls can write this binding directly. Other bindings can consume it through passthrough, aggregate, compare or parameter links.");
        }
        else if (binding.Source == RuntimeSourceKind::ValueBank)
        {
            ImGui::SeparatorText("Value bank");
            changed |= drawRuntimeBankReferenceCombo(engine, "Bank value", binding.BankValueId);
            if (const auto* value = engine.findBankValue(binding.BankValueId)) ImGui::TextDisabled("%s%s", runtimeBankValueTypeName(value->Type), value->ChangedThisFrame ? "  changed this frame" : "");
        }
        else if (binding.Source == RuntimeSourceKind::StringConstant)
        {
            ImGui::SeparatorText("String value");
            changed |= ImGui::InputText("Text", binding.StringConstant, sizeof(binding.StringConstant));
            ImGui::TextDisabled("String bindings expose a numeric presence/length signal plus a real string side-channel for string-aware controls and the value bank.");
        }
        else if (binding.Source == RuntimeSourceKind::ProfileState)
        {
            ImGui::SeparatorText("Binding profile state");
            if (binding.Signal != 0)
            {
                const RuntimeBindingProfile* profile = engine.findProfile(binding.ProfileId);
                ImGui::SetNextItemWidth(300.0f);
                if (ImGui::BeginCombo("Profile", profile ? profile->Name : "<select profile>"))
                {
                    for (const auto& candidate : engine.profiles()) { const bool selected = candidate.Id == binding.ProfileId; if (ImGui::Selectable(candidate.Name, selected)) { binding.ProfileId = candidate.Id; changed = true; } if (selected) ImGui::SetItemDefaultFocus(); }
                    ImGui::EndCombo();
                }
            }
            ImGui::TextDisabled("Profile bindings can drive indicators or automation from the currently active graph profile.");
        }
        else if (binding.Source == RuntimeSourceKind::BindingValue)
        {
            ImGui::SeparatorText("Binding passthrough");
            changed |= drawRuntimeBindingReferenceCombo(engine, "Source binding", binding.ValueBindingId, binding.Id);
        }
        else if (binding.Source == RuntimeSourceKind::ControlStatus)
        {
            ImGui::SeparatorText("Control status");
            changed |= drawRuntimeControlReferenceCombo(engine, "Control to inspect", binding.ControlStatusId);
        }
        else if (binding.Source == RuntimeSourceKind::Aggregate)
        {
            ImGui::SeparatorText("Aggregate");
            int operation = static_cast<int>(binding.AggregateOperation);
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("Operation", &operation, "Sum\0Average\0Minimum\0Maximum\0Product\0Count\0Count truthy\0Fraction truthy\0Any\0All\0")) { binding.AggregateOperation = static_cast<RuntimeAggregateOperation>(operation); changed = true; }
            changed |= drawRuntimeReferenceList(engine, binding, binding.AggregateOperation <= RuntimeAggregateOperation::Product);
            ImGui::TextDisabled("Members may be bindings or controls. Control members expose active/trigger state as numeric values.");
        }
        else if (binding.Source == RuntimeSourceKind::MassCompare)
        {
            ImGui::SeparatorText("Comparator / mass compare");
            ImGui::TextDisabled("Compare a set of bindings/controls and reduce the matches to any/all/none/count/fraction/first-index.");
            int condition = static_cast<int>(binding.CompareCondition);
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("Condition", &condition, "==\0!=\0<\0<=\0>\0>=\0between\0outside\0")) { binding.CompareCondition = static_cast<RuntimeCompareCondition>(condition); changed = true; }
            ImGui::SetNextItemWidth(220.0f); changed |= ImGui::DragFloat("Compare value A", &binding.CompareA, 0.01f);
            if (binding.CompareCondition == RuntimeCompareCondition::Between || binding.CompareCondition == RuntimeCompareCondition::Outside) { ImGui::SetNextItemWidth(220.0f); changed |= ImGui::DragFloat("Compare value B", &binding.CompareB, 0.01f); }
            if (binding.CompareCondition == RuntimeCompareCondition::Equal || binding.CompareCondition == RuntimeCompareCondition::NotEqual) { ImGui::SetNextItemWidth(220.0f); changed |= ImGui::DragFloat("Tolerance", &binding.CompareTolerance, 0.0001f, 0.000001f, 1000.0f, "%.6f"); }
            int result = static_cast<int>(binding.CompareResult);
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::Combo("Reduction result", &result, "Any\0All\0None\0Count\0Fraction\0First match index\0")) { binding.CompareResult = static_cast<RuntimeMassCompareResult>(result); changed = true; }
            ImGui::Spacing();
            changed |= drawRuntimeReferenceList(engine, binding, false, true);
        }
        else if (binding.Source == RuntimeSourceKind::ObjectField || binding.Source == RuntimeSourceKind::ObjectStatus)
        {
            ImGui::SeparatorText(binding.Source == RuntimeSourceKind::ObjectField ? "Object field" : "Object / pointer status");
            RuntimeObjectPointer* pointer = engine.findPointer(binding.ObjectPointerId);
            RuntimeObjectDescriptor* object = pointer ? engine.findObject(pointer->DescriptorId) : engine.findObject(binding.ObjectId);
            ImGui::SetNextItemWidth(360.0f);
            if (ImGui::BeginCombo("Pointer instance", pointer ? pointer->Name : "<select pointer>"))
            {
                for (auto& candidate : engine.pointers())
                {
                    const bool selected = candidate.Id == binding.ObjectPointerId;
                    const auto* descriptor = engine.findObject(candidate.DescriptorId);
                    std::string label = std::string(candidate.Name) + (descriptor ? "  [" + std::string(descriptor->Name) + "]" : "  [missing model]");
                    if (ImGui::Selectable(label.c_str(), selected)) { binding.ObjectPointerId = candidate.Id; binding.ObjectId = candidate.DescriptorId; binding.ObjectFieldId = 0; object = engine.findObject(candidate.DescriptorId); changed = true; }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (binding.Source == RuntimeSourceKind::ObjectField && object)
            {
                const RuntimeObjectField* field = engine.findObjectField(*object, binding.ObjectFieldId);
                ImGui::SetNextItemWidth(320.0f);
                if (ImGui::BeginCombo("Field", field ? field->Name : "<select readable field>"))
                {
                    for (const auto& candidate : object->Fields)
                    {
                        if (!candidate.Enabled || runtimeObjectFieldIsFiller(candidate.Type)) continue;
                        const bool selected = candidate.Id == binding.ObjectFieldId;
                        std::size_t objectSize = 0;
                        const auto offset = runtimeObjectFieldOffset(*object, candidate.Id, &objectSize);
                        std::ostringstream label; label << candidate.Name << "  +0x" << std::hex << offset << "  [" << runtimeObjectFieldTypeName(candidate.Type) << "]";
                        if (ImGui::Selectable(label.str().c_str(), selected)) { binding.ObjectFieldId = candidate.Id; changed = true; }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            if (pointer) ImGui::TextDisabled("%s", pointer->Status.c_str()); else if (object) ImGui::TextDisabled("model: %s", object->Name);
        }

        static std::vector<RuntimeProcessInfo> processes;
        if ((binding.Source == RuntimeSourceKind::NativeProcess || binding.Source == RuntimeSourceKind::NativeAddress) && processes.empty())
            processes = enumerateRuntimeProcesses();

        if (binding.Source == RuntimeSourceKind::NativeProcess || binding.Source == RuntimeSourceKind::NativeAddress)
        {
            ImGui::SeparatorText(binding.Source == RuntimeSourceKind::NativeProcess ? "Native process memory" : "Native process address");
            changed |= drawRuntimeProcessSelector(binding, processes);
            static std::vector<RuntimeProcessModule> modules;
            static int modulePid = -1;
            if (ImGui::SmallButton("Refresh modules") || modulePid != binding.ProcessId)
            {
                modules = binding.ProcessId > 0 ? enumerateRuntimeModules(static_cast<pid_t>(binding.ProcessId)) : std::vector<RuntimeProcessModule>{};
                modulePid = binding.ProcessId;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%zu mapped modules", modules.size());
            ImGui::SetNextItemWidth(410.0f);
            const char* noModuleLabel = binding.AddressMode == ProcessAddressMode::Signature ? "<all mappings>" : "<absolute address>";
            if (ImGui::BeginCombo("Module / base", binding.Module[0] ? binding.Module : noModuleLabel))
            {
                if (ImGui::Selectable(noModuleLabel, binding.Module[0] == '\0')) { binding.Module[0] = '\0'; changed = true; }
                for (const auto& module : modules)
                {
                    std::ostringstream address;
                    address << module.Name << "  0x" << std::hex << module.Base;
                    if (ImGui::Selectable(address.str().c_str(), std::string_view(binding.Module) == module.Name))
                    {
                        std::snprintf(binding.Module, sizeof(binding.Module), "%s", module.Name.c_str());
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            static constexpr const char* AddressModes[] = {"Address / pointer chain", "Pattern scan"};
            int addressMode = static_cast<int>(binding.AddressMode);
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::Combo("Address source", &addressMode, AddressModes, static_cast<int>(std::size(AddressModes))))
            {
                binding.AddressMode = static_cast<ProcessAddressMode>(addressMode);
                resetRuntimeSignatureScan(binding);
                binding.SignatureConfigHash = 0;
                changed = true;
            }
            if (binding.AddressMode == ProcessAddressMode::AddressChain)
            {
                changed |= ImGui::InputText("Address / pointer chain", binding.Address, sizeof(binding.Address));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Examples: +0x1234   |   +0x1234 -> +0x28 -> +0x18   |   libgame.so+0x1234 -> +0x20");
            }
            else
            {
                int patternKind = static_cast<int>(binding.SignaturePatternKind);
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::Combo("Pattern kind", &patternKind, "Hexadecimal pattern\0Opcode pattern\0")) { binding.SignaturePatternKind = static_cast<RuntimeSignaturePatternKind>(patternKind); resetRuntimeSignatureScan(binding); binding.SignatureConfigHash = 0; changed = true; }
                if (binding.SignaturePatternKind == RuntimeSignaturePatternKind::HexadecimalPattern)
                {
                    const bool signatureChanged = ImGui::InputTextMultiline("Hexadecimal pattern", binding.Signature, sizeof(binding.Signature), ImVec2(-1.0f, 72.0f));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hex bytes with wildcards: DE AD ? BE EF | 48 8B 05 ?? ?? ?? ?? | nibble wildcards A? and ?F.");
                    changed |= signatureChanged;
                }
                else
                {
                    const bool signatureChanged = ImGui::InputTextMultiline("Opcode pattern", binding.Signature, sizeof(binding.Signature), ImVec2(-1.0f, 120.0f));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("One Intel-syntax instruction per line. * matches any text and ? matches one character. Example:\nmov rax, r15\nmovsxd rax, dword ptr [rax+*]\ncvtsi2ss xmm0, eax");
                    changed |= signatureChanged;
#if !QUARTZ_HAS_ZYDIS
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), "Opcode patterns require Zydis.");
#endif
                }
                changed |= ImGui::Checkbox("Executable mappings only", &binding.SignatureExecutableOnly);
                static constexpr const char* ResolveModes[] = {"Match address + offset", "x86-64 RIP-relative disp32", "Pointer stored at match + offset", "x86-64 register-relative capture", "32-bit address stored at match + offset"};
                int resolveMode = static_cast<int>(binding.SignatureResolve);
                ImGui::SetNextItemWidth(300.0f);
                if (ImGui::Combo("Pattern result", &resolveMode, ResolveModes, static_cast<int>(std::size(ResolveModes)))) { binding.SignatureResolve = static_cast<SignatureResultMode>(resolveMode); changed = true; }
                ImGui::SetNextItemWidth(180.0f);
                const char* resultOffsetLabel = binding.SignatureResolve == SignatureResultMode::RipRelative32 ? "Displacement offset" : binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture ? "Instruction offset" : "Result offset";
                changed |= ImGui::InputInt(resultOffsetLabel, &binding.SignatureResultOffset);
                if (binding.SignatureResolve == SignatureResultMode::Address32AtOffset) ImGui::TextDisabled("Reads a little-endian uint32_t at match + result offset, zero-extends it to uintptr_t, and uses that value as the resolved address.");
                if (binding.SignatureResolve == SignatureResultMode::RipRelative32)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(140.0f);
                    changed |= ImGui::InputInt("Instruction size", &binding.SignatureInstructionSize);
                    binding.SignatureInstructionSize = std::max(binding.SignatureInstructionSize, 1);
                }
                else if (binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture)
                {
                    static constexpr const char* Registers[] = {"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};
                    int reg = static_cast<int>(binding.SignatureRegister);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(110.0f);
                    if (ImGui::Combo("Base register", &reg, Registers, static_cast<int>(std::size(Registers)))) { binding.SignatureRegister = static_cast<RuntimeX64Register>(reg); changed = true; }
                    static constexpr const char* Displacements[] = {"signed disp8", "signed disp32", "manual"};
                    int displacementType = static_cast<int>(binding.SignatureDisplacementType);
                    ImGui::SetNextItemWidth(180.0f);
                    if (ImGui::Combo("Displacement", &displacementType, Displacements, static_cast<int>(std::size(Displacements)))) { binding.SignatureDisplacementType = static_cast<RuntimeDisplacementType>(displacementType); changed = true; }
                    ImGui::Indent(18.0f);
                    ImGui::SetNextItemWidth(220.0f);
                    if (binding.SignatureDisplacementType == RuntimeDisplacementType::Manual) changed |= ImGui::InputInt("Manual displacement", &binding.SignatureManualDisplacement);
                    else changed |= ImGui::InputInt("Displacement byte offset", &binding.SignatureRegisterDisplacementOffset);
                    ImGui::Unindent(18.0f);
                    ImGui::SetNextItemWidth(180.0f);
                    changed |= ImGui::DragFloat("Capture timeout", &binding.SignatureCaptureTimeoutSeconds, 0.1f, 0.1f, 120.0f, "%.1f s");
                    ImGui::TextDisabled("A temporary hardware execution breakpoint captures the selected register when the matched instruction executes. The breakpoint is removed immediately after capture; normal reads then use process_vm_readv().");
                }
                ImGui::SetNextItemWidth(180.0f);
                changed |= ImGui::DragFloat("Retry interval", &binding.SignatureRetrySeconds, 0.05f, 0.1f, 60.0f, "%.2f s");
                changed |= ImGui::InputText("Result / pointer chain", binding.Address, sizeof(binding.Address));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("The resolved pattern address becomes the base. Use +0x0 for the result itself or chains such as +0x0 -> +0x18.");
                if (ImGui::Button("Rescan pattern"))
                {
                    resetRuntimeSignatureScan(binding);
                    binding.SignatureConfigHash = 0;
                    binding.NextUpdate = 0.0;
                }
                if (binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture && binding.SignatureInstructionAddress != 0 && binding.SignatureResolvedAddress == 0)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Retry register capture"))
                    {
                        binding.SignatureRegisterCapture.reset();
                        binding.NextRegisterCapture = 0.0;
                        binding.NextUpdate = 0.0;
                    }
                }
                ImGui::SameLine();
                if (binding.SignatureResolvedAddress != 0) ImGui::TextDisabled("resolved: 0x%llX", static_cast<unsigned long long>(binding.SignatureResolvedAddress));
                else if (!binding.SignatureStatus.empty()) ImGui::TextDisabled("%s", binding.SignatureStatus.c_str());
                if (binding.SignatureScanRunning) drawIndeterminateProgressBar(ImVec2(320.0f, 0.0f));
                if (binding.SignatureScanAverageMiBs > 0.0)
                {
                    if (binding.SignatureScanAverageMiBs >= 1024.0) ImGui::TextDisabled("scan avg %.2f GiB/s%s", binding.SignatureScanAverageMiBs / 1024.0, binding.SignatureScanRunning ? "  (running)" : "");
                    else ImGui::TextDisabled("scan avg %.1f MiB/s%s", binding.SignatureScanAverageMiBs, binding.SignatureScanRunning ? "  (running)" : "");
                    if (!binding.SignatureScanRunning && binding.SignatureScanLastSeconds > 0.0) ImGui::SameLine(), ImGui::TextDisabled("%.1f MiB in %.3f s", binding.SignatureScanLastBytes / (1024.0 * 1024.0), binding.SignatureScanLastSeconds);
                }
                if (binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture)
                {
                    if (binding.SignatureMatchAddress != 0) ImGui::TextDisabled("Match 0x%llX   instruction 0x%llX", static_cast<unsigned long long>(binding.SignatureMatchAddress), static_cast<unsigned long long>(binding.SignatureInstructionAddress));
                    if (binding.SignatureCapturedRegister != 0 || binding.SignatureResolvedAddress != 0) ImGui::TextDisabled("%s 0x%llX   displacement %lld", runtimeX64RegisterName(binding.SignatureRegister), static_cast<unsigned long long>(binding.SignatureCapturedRegister), static_cast<long long>(binding.SignatureCapturedDisplacement));
                }
                else ImGui::TextDisabled("The pattern locates a stable instruction or data reference. Hexadecimal patterns match bytes; opcode patterns match readable Intel-syntax instructions through Zydis.");
            }
            if (binding.Source == RuntimeSourceKind::NativeProcess)
            {
                static constexpr const char* Types[] = {"u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "float", "double", "bool"};
                int valueType = static_cast<int>(binding.ValueType);
                if (ImGui::Combo("Value type", &valueType, Types, static_cast<int>(std::size(Types)))) { binding.ValueType = static_cast<ProcessValueType>(valueType); changed = true; }
                ImGui::TextDisabled("Read-only through process_vm_readv(); ptrace_scope/permissions still apply.");
            }
            else
            {
                if (binding.HasAddress) ImGui::Text("Exact address: 0x%llX", static_cast<unsigned long long>(binding.AddressValue));
                ImGui::TextDisabled("Address bindings preserve uintptr_t precision for object descriptors instead of squeezing pointers through float values.");
            }
        }
        else if (binding.Source == RuntimeSourceKind::BindingStatus)
        {
            ImGui::SeparatorText("Binding status");
            const RuntimeBinding* target = engine.findBinding(binding.StatusBindingId);
            ImGui::SetNextItemWidth(320.0f);
            if (ImGui::BeginCombo("Binding to inspect", target ? target->Name : "<select binding>"))
            {
                for (const auto& candidate : engine.bindings())
                {
                    if (candidate.Id == binding.Id) continue;
                    const bool selected = candidate.Id == binding.StatusBindingId;
                    if (ImGui::Selectable(candidate.Name, selected)) { binding.StatusBindingId = candidate.Id; changed = true; }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Status values are normal 0/1 binding sources, so they can drive shader uniforms, parameter links, or control rules.");
        }

        if (ImGui::TreeNode("Value origin"))
        {
            const auto sourceSignals = runtimeSignalNames(binding.Source);
            const int signalIndex = std::clamp(binding.Signal, 0, std::max(static_cast<int>(sourceSignals.size()) - 1, 0));
            ImGui::Text("Source: %s", runtimeSourceName(binding.Source));
            if (!sourceSignals.empty()) ImGui::Text("Signal: %s", sourceSignals[static_cast<std::size_t>(signalIndex)].data());
            if (binding.Source == RuntimeSourceKind::BindingValue)
            {
                const RuntimeBinding* sourceBinding = engine.findBinding(binding.ValueBindingId);
                ImGui::Text("From binding: %s (#%llu)", sourceBinding ? sourceBinding->Name : "<missing>", static_cast<unsigned long long>(binding.ValueBindingId));
            }
            else if (binding.Source == RuntimeSourceKind::BindingStatus)
            {
                const RuntimeBinding* sourceBinding = engine.findBinding(binding.StatusBindingId);
                ImGui::Text("Status of: %s (#%llu)", sourceBinding ? sourceBinding->Name : "<missing>", static_cast<unsigned long long>(binding.StatusBindingId));
            }
            else if (binding.Source == RuntimeSourceKind::ObjectField || binding.Source == RuntimeSourceKind::ObjectStatus)
            {
                const RuntimeObjectPointer* pointer = engine.findPointer(binding.ObjectPointerId);
                const RuntimeObjectDescriptor* model = pointer ? engine.findObject(pointer->DescriptorId) : engine.findObject(binding.ObjectId);
                ImGui::Text("Pointer instance: %s (#%llu)", pointer ? pointer->Name : "<missing>", static_cast<unsigned long long>(binding.ObjectPointerId));
                ImGui::Text("Descriptor model: %s", model ? model->Name : "<missing>");
                if (binding.Source == RuntimeSourceKind::ObjectField && model)
                {
                    const RuntimeObjectField* field = engine.findObjectField(*model, binding.ObjectFieldId);
                    ImGui::Text("Field: %s (#%llu)", field ? field->Name : "<missing>", static_cast<unsigned long long>(binding.ObjectFieldId));
                }
            }
            else if (binding.Source == RuntimeSourceKind::ValueBank)
            {
                const RuntimeValueBankEntry* bank = engine.findBankValue(binding.BankValueId);
                ImGui::Text("Bank value: %s (#%llu)", bank ? bank->Name : "<missing>", static_cast<unsigned long long>(binding.BankValueId));
            }
            else if (binding.Source == RuntimeSourceKind::Aggregate || binding.Source == RuntimeSourceKind::MassCompare)
            {
                ImGui::Text("Members: %zu", binding.References.size());
                for (const auto& reference : binding.References)
                {
                    if (!reference.Enabled) continue;
                    if (reference.Kind == RuntimeReferenceKind::Binding) { const RuntimeBinding* member = engine.findBinding(reference.Id); ImGui::BulletText("binding %s (#%llu)", member ? member->Name : "<missing>", static_cast<unsigned long long>(reference.Id)); }
                    else { const RuntimeControlRule* member = engine.findControl(reference.Id); ImGui::BulletText("control %s (#%llu)", member ? member->Name : "<missing>", static_cast<unsigned long long>(reference.Id)); }
                }
            }
            if (binding.HasString) ImGui::TextWrapped("String value: %s", binding.StringValue.c_str());
            if (binding.HasValue) ImGui::Text("Raw %.9g  ->  value %.9g", binding.RawValue, binding.Value);
            else ImGui::TextDisabled("No readable value yet.");
            ImGui::TreePop();
        }

        if (binding.HasAddress)
        {
            if (ImGui::TreeNode("Address origin"))
            {
                ImGui::Text("Current exact address: 0x%llX", static_cast<unsigned long long>(binding.AddressValue));
                if (binding.AddressProvenance.empty()) ImGui::TextDisabled("No provenance trace recorded yet.");
                for (std::size_t i = 0; i < binding.AddressProvenance.size(); ++i)
                {
                    ImGui::PushID(static_cast<int>(i)); const bool leaf = i + 1 == binding.AddressProvenance.size();
                    ImGui::TreeNodeEx("##origin", ImGuiTreeNodeFlags_DefaultOpen | (leaf ? ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen : ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen), "%s", binding.AddressProvenance[i].c_str()); ImGui::PopID();
                }
                ImGui::TreePop();
            }
        }

        ImGui::SeparatorText("Material binding");
        changed |= ImGui::Checkbox("Write value to shader material", &binding.WriteMaterial);
        if (binding.WriteMaterial) changed |= drawRuntimeTargetSelector(binding, shaderFramebuffer);
        else ImGui::TextDisabled("Value-only binding: still available to links, bank values, status sources, controls and actions.");

        if (ImGui::BeginTabBar("BindingOptions", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyResizeDown))
        {
            if (ImGui::BeginTabItem("Output & actions"))
            {
                changed |= ImGui::Checkbox("Store successful value in bank", &binding.StoreToBank);
                if (binding.StoreToBank) changed |= drawRuntimeBankReferenceCombo(engine, "Bank destination", binding.StoreBankValueId);
                ImGui::SeparatorText("Actions");
                ImGui::TextDisabled("Bindings can fan out to multiple side effects on update/change/truthy/falsy without creating helper controls.");
                changed |= drawRuntimeActionList(engine, shaderFramebuffer, binding.Actions, binding.Id, false);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Transform"))
            {
                ImGui::TextDisabled("Transform controls can use local values or another binding. This tab stays out of the way for normal graph editing.");
                changed |= drawRuntimeLinkedBool(engine, binding, RuntimeParameterSlot::Normalize, "Normalize input", binding.Normalize);
                if (binding.Normalize || binding.ParameterLinks[static_cast<std::size_t>(RuntimeParameterSlot::Normalize)].Enabled)
                {
                    changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::InputMin, "Input min", binding.InputMin);
                    changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::InputMax, "Input max", binding.InputMax);
                }
                changed |= drawRuntimeLinkedBool(engine, binding, RuntimeParameterSlot::Invert, "Invert", binding.Invert);
                changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::Scale, "Scale", binding.Scale);
                changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::Offset, "Offset", binding.Offset);
                changed |= drawRuntimeLinkedBool(engine, binding, RuntimeParameterSlot::Clamp, "Clamp output", binding.Clamp);
                if (binding.Clamp || binding.ParameterLinks[static_cast<std::size_t>(RuntimeParameterSlot::Clamp)].Enabled)
                {
                    changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::OutputMin, "Output min", binding.OutputMin);
                    changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::OutputMax, "Output max", binding.OutputMax);
                }
                changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::SmoothingHz, "Smoothing", binding.SmoothingHz, 0.1f, "%.1f Hz");
                changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::UpdateHz, "Update rate", binding.UpdateHz, 0.5f, "%.1f Hz");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        if (binding.HasAddress) ImGui::TextDisabled("Exact address: 0x%llX", static_cast<unsigned long long>(binding.AddressValue));
        ImGui::Unindent(10.0f);
        ImGui::Dummy(ImVec2(0.0f, 5.0f));
        ImGui::Separator();

        if (changed) engine.markChanged();
        ImGui::PopID();
        ImGui::PopID();
        return changed;
    }
    const char* runtimeControlConditionName(const RuntimeControlCondition condition)
    {
        static constexpr const char* Names[] = {"==", "!=", "<", "<=", ">", ">=", "between", "outside", "rising edge", "falling edge", "on change", "changed to", "changed from", "becomes true", "becomes false", "string ==", "string !=", "string contains"};
        return Names[std::clamp(static_cast<int>(condition), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    const char* runtimeControlTargetName(const RuntimeControlTarget target)
    {
        static constexpr const char* Names[] = {"Active shader", "Binding enabled", "Global brightness", "Send framebuffer", "Base color mode", "Material parameter", "Unbound binding value", "Value bank", "Control enabled", "Refresh binding", "Force binding update", "Invalidate binding", "Reset binding state", "Retry register capture", "Rescan binding pattern", "Rebind process", "Clear binding error"};
        return Names[std::clamp(static_cast<int>(target), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    bool drawRuntimeControlMaterialTarget(RuntimeControlRule& control, ShaderFramebuffer& shaderFramebuffer)
    {
        bool changed = false;
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::BeginCombo("Uniform id##control", control.TargetId[0] ? control.TargetId : "<select material uniform>"))
        {
            for (const auto& parameter : shaderFramebuffer.materialParameters())
            {
                const bool selected = parameter.PersistenceKey == control.TargetId;
                const std::string label = parameter.Label + "  [" + parameter.PersistenceKey + "]";
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    std::snprintf(control.TargetId, sizeof(control.TargetId), "%s", parameter.PersistenceKey.c_str());
                    control.TargetComponent = std::clamp(control.TargetComponent, 0, std::max(parameter.Components - 1, 0));
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (const auto* parameter = shaderFramebuffer.findMaterialParameter(control.TargetId); parameter && parameter->Components > 1)
        {
            ImGui::SameLine();
            changed |= ImGui::SliderInt("Component##control", &control.TargetComponent, 0, parameter->Components - 1);
        }
        return changed;
    }

    bool drawRuntimeControlRule(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer, RuntimeControlRule& control, bool& erase)
    {
        bool changed = false;
        ImGui::PushID("RuntimeControl");
        ImGui::PushID(static_cast<int>(control.Id & 0x7fffffffULL));
        const RuntimeBinding* source = engine.findBinding(control.SourceBindingId);
        const bool ready = source && source->Enabled && source->RuntimeEnabled && source->HasValue;
        const bool runtimeEnabled = control.Enabled && control.RuntimeEnabled;
        const bool on = control.ConditionActive || control.TriggeredThisFrame;
        const ImVec4 stateColor = runtimeStateColor(runtimeEnabled, on, false, runtimeEnabled && !ready);
        drawRuntimeStateSquare("##controlState", stateColor);
        ImGui::SameLine();
        std::string header = std::string(control.Name[0] ? control.Name : "Control") + "  [p" + std::to_string(control.Priority) + "]  ";
        if (!control.Enabled) header += "DISABLED";
        else if (!control.RuntimeEnabled) header += "RUNTIME OFF";
        else if (!ready) header += "WAITING";
        else if (control.TriggeredThisFrame) header += "TRIGGERED";
        else header += control.ConditionActive ? "TRUE" : "FALSE";
        header += "###RuntimeControl" + std::to_string(control.Id);
        if (!ImGui::CollapsingHeader(header.c_str())) { ImGui::PopID(); ImGui::PopID(); return false; }
        ImGui::Indent(10.0f);
        ImGui::TextColored(stateColor, "%s", !control.Enabled ? "Disabled" : !control.RuntimeEnabled ? "Runtime disabled by control" : !ready ? "Waiting for source" : control.TriggeredThisFrame ? "Triggered this frame" : control.ConditionActive ? "Condition true" : "Condition false");
        if (source) { ImGui::SameLine(); ImGui::TextDisabled("source %.6g%s", source->Value, source->HasString ? " + string" : ""); }

        changed |= ImGui::Checkbox("Enabled", &control.Enabled);
        ImGui::SameLine(); if (ImGui::SmallButton("Remove")) erase = true;
        ImGui::SameLine(); ImGui::SetNextItemWidth(220.0f); changed |= ImGui::InputText("Name", control.Name, sizeof(control.Name));
        ImGui::SameLine(); ImGui::SetNextItemWidth(110.0f); changed |= ImGui::InputInt("Priority", &control.Priority);
        ImGui::SetNextItemWidth(160.0f); changed |= ImGui::InputText("Group", control.Group, sizeof(control.Group)); ImGui::SameLine(); ImGui::SetNextItemWidth(80.0f); changed |= ImGui::InputInt("Order", &control.Order); ImGui::SameLine(); if (ImGui::SmallButton("Up##controlOrder")) { --control.Order; changed = true; } ImGui::SameLine(); if (ImGui::SmallButton("Down##controlOrder")) { ++control.Order; changed = true; }

        ImGui::SeparatorText("Input & condition");
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::BeginCombo("Input binding", source ? source->Name : "<select binding>"))
        {
            for (const auto& candidate : engine.bindings()) { const bool selected = candidate.Id == control.SourceBindingId; if (ImGui::Selectable(candidate.Name, selected)) { control.SourceBindingId = candidate.Id; changed = true; } if (selected) ImGui::SetItemDefaultFocus(); }
            ImGui::EndCombo();
        }

        int condition = static_cast<int>(control.Condition);
        ImGui::SetNextItemWidth(175.0f);
        if (ImGui::Combo("Condition", &condition, "==\0!=\0<\0<=\0>\0>=\0between\0outside\0rising edge\0falling edge\0on change\0changed to\0changed from\0becomes true\0becomes false\0string ==\0string !=\0string contains\0")) { control.Condition = static_cast<RuntimeControlCondition>(condition); changed = true; }
        const bool stringCondition = control.Condition == RuntimeControlCondition::StringEqual || control.Condition == RuntimeControlCondition::StringNotEqual || control.Condition == RuntimeControlCondition::StringContains;
        const bool eventCondition = control.Condition == RuntimeControlCondition::RisingEdge || control.Condition == RuntimeControlCondition::FallingEdge || control.Condition == RuntimeControlCondition::OnChange || control.Condition == RuntimeControlCondition::ChangedTo || control.Condition == RuntimeControlCondition::ChangedFrom || control.Condition == RuntimeControlCondition::BecomesTrue || control.Condition == RuntimeControlCondition::BecomesFalse;
        const bool needsValue = control.Condition <= RuntimeControlCondition::FallingEdge || control.Condition == RuntimeControlCondition::ChangedTo || control.Condition == RuntimeControlCondition::ChangedFrom;
        if (stringCondition) changed |= ImGui::InputText("Text target", control.StringCompare, sizeof(control.StringCompare));
        else if (needsValue)
        {
            const bool twoValues = control.Condition == RuntimeControlCondition::Between || control.Condition == RuntimeControlCondition::Outside;
            ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat(twoValues ? "Minimum" : "Target / threshold", &control.ValueA, 0.01f);
            if (twoValues) { ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Maximum", &control.ValueB, 0.01f); }
            if (control.Condition == RuntimeControlCondition::Equal || control.Condition == RuntimeControlCondition::NotEqual || control.Condition == RuntimeControlCondition::ChangedTo || control.Condition == RuntimeControlCondition::ChangedFrom)
            { ImGui::SameLine(); ImGui::SetNextItemWidth(135.0f); changed |= ImGui::DragFloat("Tolerance", &control.Tolerance, 0.0001f, 0.000001f, 1000.0f, "%.6f"); }
        }
        if (eventCondition) { changed |= ImGui::Checkbox("Fire on first matching sample", &control.FireOnFirstSample); ImGui::SameLine(); ImGui::TextDisabled("events are one-shot; iterative passes are capped"); }
        else if (!stringCondition) { ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Hysteresis", &control.Hysteresis, 0.001f, 0.0f, 1000.0f, "%.4f"); }

        ImGui::SeparatorText("Target & actions");
        if (ImGui::BeginTabBar("ControlOptions", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyResizeDown))
        {
            if (ImGui::BeginTabItem("Target"))
            {
                ImGui::TextDisabled("Compatibility action for existing configs. Extra actions below can run alongside it.");
                int target = static_cast<int>(control.Target); ImGui::SetNextItemWidth(220.0f);
                if (ImGui::Combo("Target", &target, "Active shader\0Binding enabled\0Global brightness\0Send framebuffer\0Base color mode\0Material parameter\0Unbound binding value\0Value bank\0Control enabled\0Refresh binding\0Force binding update\0Invalidate binding\0Reset binding state\0Retry register capture\0Rescan binding pattern\0Rebind process\0Clear binding error\0")) { control.Target = static_cast<RuntimeControlTarget>(target); changed = true; }
                if (control.Target == RuntimeControlTarget::ActiveShader)
                {
                    const ShaderPreset* shaderById = control.ShaderId[0] ? findShaderPresetById(control.ShaderId) : nullptr;
                    const char* preview = control.ShaderPresetIndex == -1 ? "<previous shader>" : shaderById ? shaderById->Name.c_str() : control.ShaderPresetIndex > 0 && control.ShaderPresetIndex <= static_cast<int>(ShaderPresets.size()) ? ShaderPresets[static_cast<std::size_t>(control.ShaderPresetIndex - 1)].Name.c_str() : "<select shader>";
                    ImGui::SetNextItemWidth(300.0f);
                    if (ImGui::BeginCombo("Shader", preview))
                    {
                        if (ImGui::Selectable("<previous shader>", control.ShaderPresetIndex == -1)) { control.ShaderPresetIndex = -1; control.ShaderId[0] = '\0'; changed = true; }
                        for (std::size_t i = 0; i < ShaderPresets.size(); ++i) { const bool selected = control.ShaderPresetIndex == static_cast<int>(i + 1); if (ImGui::Selectable(ShaderPresets[i].Name.c_str(), selected)) { control.ShaderPresetIndex = static_cast<int>(i + 1); std::snprintf(control.ShaderId, sizeof(control.ShaderId), "%s", ShaderPresets[i].Id.c_str()); changed = true; } }
                        ImGui::EndCombo();
                    }
                    ImGui::SetNextItemWidth(160.0f); changed |= ImGui::DragFloat("Crossfade", &control.TransitionSeconds, 0.02f, 0.0f, 10.0f, "%.2f s");
                }
                else if (control.Target == RuntimeControlTarget::BindingEnabled) { changed |= drawRuntimeBindingReferenceCombo(engine, "Target binding", control.TargetBindingId, control.SourceBindingId); changed |= ImGui::Checkbox("Runtime enabled", &control.TargetBool); }
                else if (control.Target == RuntimeControlTarget::GlobalBrightness) changed |= ImGui::SliderFloat("Brightness", &control.TargetValue, 0.0f, 1.0f, "%.3f");
                else if (control.Target == RuntimeControlTarget::SendFramebuffer) changed |= ImGui::Checkbox("Send framebuffer", &control.TargetBool);
                else if (control.Target == RuntimeControlTarget::BaseColorMode) { int mode = std::clamp(static_cast<int>(std::lround(control.TargetValue)), 0, 2); if (ImGui::Combo("Base color mode", &mode, "RGB wave\0Solid\0Shader\0")) { control.TargetValue = static_cast<float>(mode); changed = true; } }
                else if (control.Target == RuntimeControlTarget::MaterialParameter) { changed |= drawRuntimeControlMaterialTarget(control, shaderFramebuffer); ImGui::SetNextItemWidth(180.0f); changed |= ImGui::DragFloat("Target value", &control.TargetValue, 0.01f); }
                else if (control.Target == RuntimeControlTarget::BindingValue) { changed |= drawRuntimeBindingReferenceCombo(engine, "Unbound target", control.TargetBindingId, control.SourceBindingId, true); changed |= ImGui::Checkbox("Use source value", &control.TargetUseSourceValue); if (!control.TargetUseSourceValue) { ImGui::SetNextItemWidth(180.0f); changed |= ImGui::DragFloat("Written value", &control.TargetValue, 0.01f); } }
                else if (control.Target == RuntimeControlTarget::ValueBank) { changed |= drawRuntimeBankReferenceCombo(engine, "Bank target", control.TargetBankValueId); changed |= ImGui::Checkbox("Use source value", &control.TargetUseSourceValue); if (!control.TargetUseSourceValue) { ImGui::SetNextItemWidth(180.0f); changed |= ImGui::DragFloat("Written value", &control.TargetValue, 0.01f); } }
                else if (control.Target == RuntimeControlTarget::ControlEnabled) { changed |= drawRuntimeControlReferenceCombo(engine, "Target control", control.TargetControlId); changed |= ImGui::Checkbox("Runtime enabled", &control.TargetBool); }
                else if (runtimeControlTargetIsBindingOperation(control.Target)) { changed |= drawRuntimeBindingReferenceCombo(engine, "Target binding", control.TargetBindingId, control.SourceBindingId); ImGui::TextDisabled("Primary binding operations fire when the condition enters true (or an event fires). Use an additional action with While active for repeated operations."); }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Extra actions"))
            {
                ImGui::TextDisabled("Actions execute in priority order. On-trigger actions are ideal for saving/restoring shader state through the value bank.");
                changed |= drawRuntimeActionList(engine, shaderFramebuffer, control.Actions, control.SourceBindingId, true);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::SeparatorText("Runtime state");
        ImGui::TextDisabled("Triggers: %llu   %s", static_cast<unsigned long long>(control.TriggerCount), control.LastTriggerTime > 0.0 ? "has fired" : "never fired");
        if (changed) engine.markChanged();
        ImGui::Unindent(10.0f);
        ImGui::Dummy(ImVec2(0.0f, 5.0f));
        ImGui::Separator();
        ImGui::PopID(); ImGui::PopID();
        return changed;
    }

    bool runtimeWriteProcessMemory(const pid_t pid, const std::uintptr_t address, const std::span<const std::uint8_t> bytes, std::string& error)
    {
        if (bytes.empty()) { error = "nothing to write"; return false; }
        iovec local{const_cast<std::uint8_t*>(bytes.data()), bytes.size()}; iovec remote{reinterpret_cast<void*>(address), bytes.size()}; errno = 0;
        const ssize_t count = ::process_vm_writev(pid, &local, 1, &remote, 1, 0);
        if (count == static_cast<ssize_t>(bytes.size())) { error.clear(); return true; }
        const std::string vmError = count < 0 ? std::string(std::strerror(errno)) : "short write (" + std::to_string(count) + "/" + std::to_string(bytes.size()) + ")";
        const std::string memPath = "/proc/" + std::to_string(pid) + "/mem"; const int fd = ::open(memPath.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) { error = "process_vm_writev: " + vmError + "; /proc/pid/mem: " + std::strerror(errno); return false; }
        errno = 0; const ssize_t written = ::pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(address)); const int savedErrno = errno; ::close(fd);
        if (written == static_cast<ssize_t>(bytes.size())) { error.clear(); return true; }
        const std::string memError = written < 0 ? std::string(std::strerror(savedErrno)) : "short write (" + std::to_string(written) + "/" + std::to_string(bytes.size()) + ")";
        error = "process_vm_writev: " + vmError + "; /proc/pid/mem: " + memError; return false;
    }

    bool runtimeParseHexBytes(const std::string_view text, std::vector<std::uint8_t>& bytes, std::string& error)
    {
        bytes.clear(); std::istringstream stream{std::string(text)}; std::string token;
        while (stream >> token)
        {
            if (token.size() != 2 || runtimeHexNibble(token[0]) < 0 || runtimeHexNibble(token[1]) < 0) { error = "expected hexadecimal bytes such as 90 90 CC"; return false; }
            bytes.push_back(static_cast<std::uint8_t>((runtimeHexNibble(token[0]) << 4) | runtimeHexNibble(token[1])));
        }
        if (bytes.empty()) { error = "no bytes entered"; return false; } error.clear(); return true;
    }

    std::string runtimeFormatHexBytes(const std::span<const std::uint8_t> bytes)
    {
        std::ostringstream out; out << std::hex << std::uppercase << std::setfill('0');
        for (std::size_t i = 0; i < bytes.size(); ++i) { if (i) out << ' '; out << std::setw(2) << static_cast<unsigned>(bytes[i]); } return out.str();
    }

    void runtimeRefreshMemoryInspector(RuntimeMemoryInspectorState& state)
    {
        state.Original.assign(static_cast<std::size_t>(std::clamp(state.ReadSize, 1, 4096)), 0); std::string error;
        if (!readProcessMemoryBlock(state.Pid, state.Address, state.Original, error)) { state.Original.clear(); state.Status = "read failed: " + error; return; }
        const std::string formatted = runtimeFormatHexBytes(state.Original); std::snprintf(state.HexEdit.data(), state.HexEdit.size(), "%s", formatted.c_str()); state.Status = "read " + std::to_string(state.Original.size()) + " bytes"; state.WriteConfirm = 0;
#if QUARTZ_HAS_ZYDIS
        std::ostringstream disassembly; std::size_t offset = 0;
        while (offset < state.Original.size())
        {
            std::string text; std::size_t length = 0; if (!runtimeDecodeInstructionText(std::span<const std::uint8_t>(state.Original).subspan(offset), state.Address + offset, text, length) || length == 0) break;
            disassembly << runtimeHexAddress(state.Address + offset) << "  " << text << '\n'; offset += length;
        }
        if (!state.EditorInitialized) { state.Disassembly.SetPalette(shaderEditorPalette()); state.Disassembly.SetShowLineNumbersEnabled(false); state.Disassembly.SetWordWrapEnabled(false); state.EditorInitialized = true; }
        state.Disassembly.SetText(disassembly.str());
#else
        if (!state.EditorInitialized) { state.Disassembly.SetPalette(shaderEditorPalette()); state.Disassembly.SetShowLineNumbersEnabled(false); state.Disassembly.SetWordWrapEnabled(false); state.EditorInitialized = true; }
        state.Disassembly.SetText("Zydis is not available. Install/link Zydis to enable disassembly.");
#endif
    }

    RuntimeMemoryInspectorState& runtimeMemoryInspectorState()
    {
        static RuntimeMemoryInspectorState state;
        return state;
    }

    void drawRuntimeMemoryInspector(RuntimeMemoryInspectorState& state)
    {
        ImGui::SeparatorText("Memory / disassembly");
        int pidValue = static_cast<int>(state.Pid); ImGui::SetNextItemWidth(100.0f); if (ImGui::InputInt("PID##memory", &pidValue)) state.Pid = static_cast<pid_t>(std::max(pidValue, 0)); ImGui::SameLine();
        unsigned long long address = state.Address; ImGui::SetNextItemWidth(190.0f); if (ImGui::InputScalar("Address##memory", ImGuiDataType_U64, &address, nullptr, nullptr, "0x%llX", ImGuiInputTextFlags_CharsHexadecimal)) state.Address = static_cast<std::uintptr_t>(address); ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f); ImGui::InputInt("Bytes", &state.ReadSize); state.ReadSize = std::clamp(state.ReadSize, 1, 4096); ImGui::SameLine(); if (ImGui::Button("Read / disassemble")) runtimeRefreshMemoryInspector(state);
        if (!state.Status.empty()) ImGui::TextDisabled("%s", state.Status.c_str());
        if (!state.Original.empty())
        {
            ImGui::TextDisabled("Editable bytes (snapshot is retained separately for verification/restore)"); ImGui::InputTextMultiline("##hexPatch", state.HexEdit.data(), state.HexEdit.size(), ImVec2(-1.0f, 80.0f));
            ImGui::TextDisabled("Disassembly"); state.Disassembly.Render("##disassembly", ImVec2(-1.0f, 220.0f));
            std::vector<std::uint8_t> patch; std::string error; const bool patchValid = runtimeParseHexBytes(state.HexEdit.data(), patch, error) && patch.size() <= state.Original.size();
            if (!patchValid) ImGui::TextColored(ImVec4(1.0f,0.55f,0.2f,1.0f), "%s", error.empty() ? "patch is larger than the read window" : error.c_str());
            if (state.WriteConfirm == 0) { if (ImGui::Button("WRITE") && patchValid) state.WriteConfirm = 1; }
            else if (state.WriteConfirm == 1) { ImGui::TextColored(ImVec4(1.0f,0.65f,0.2f,1.0f), "Are you sure? PID %d @ %s", state.Pid, runtimeHexAddress(state.Address).c_str()); if (ImGui::Button("Nope")) state.WriteConfirm = 0; ImGui::SameLine(); if (ImGui::Button("Yes, continue")) state.WriteConfirm = 2; }
            else if (state.WriteConfirm == 2) { ImGui::TextColored(ImVec4(1.0f,0.45f,0.2f,1.0f), "Hmm... really really sure?"); if (ImGui::Button("Abort")) state.WriteConfirm = 0; ImGui::SameLine(); if (ImGui::Button("REALLY WRITE")) state.WriteConfirm = 3; }
            else
            {
                ImGui::TextColored(ImVec4(1.0f,0.25f,0.25f,1.0f), "Final check: this writes directly into another process.");
                if (ImGui::Button("I changed my mind")) state.WriteConfirm = 0; ImGui::SameLine();
                if (ImGui::Button("DO IT"))
                {
                    std::vector<std::uint8_t> current(patch.size());
                    if (!readProcessMemoryBlock(state.Pid, state.Address, current, error)) state.Status = "write refused: verification read failed: " + error;
                    else if (!std::equal(current.begin(), current.end(), state.Original.begin())) state.Status = "write refused: target bytes changed since preview";
                    else if (runtimeWriteProcessMemory(state.Pid, state.Address, patch, error)) { state.Patched = patch; state.Status = "it somehow worked"; state.WriteConfirm = 0; }
                    else state.Status = "write failed: " + error;
                }
            }
            if (!state.Patched.empty())
            {
                if (ImGui::Button("Restore original bytes if not ded to hell"))
                {
                    std::vector<std::uint8_t> current(state.Patched.size());
                    if (!runtimeProcessIsAlive(state.Pid)) state.Status = "target process no longer exists: ded to hell";
                    else if (!readProcessMemoryBlock(state.Pid, state.Address, current, error)) state.Status = "restore refused: read failed: " + error;
                    else if (current != state.Patched) state.Status = "restore refused: memory no longer matches patched bytes";
                    else if (runtimeWriteProcessMemory(state.Pid, state.Address, std::span<const std::uint8_t>(state.Original).first(state.Patched.size()), error)) { state.Status = "original bytes restored"; state.Patched.clear(); runtimeRefreshMemoryInspector(state); }
                    else state.Status = "restore failed: " + error;
                }
            }
        }
    }

    void drawRuntimePointers(RuntimeBindingEngine& engine)
    {
        auto& inspector = runtimeMemoryInspectorState();
        ImGui::TextUnformatted("Pointer assignments"); ImGui::SameLine(); if (ImGui::Button("+ Add pointer")) engine.addPointer();
        ImGui::SameLine(); ImGui::TextDisabled("Descriptors are models; pointers assign a model to a real process address and retain provenance.");
        std::optional<std::size_t> erase;
        std::vector<RuntimeObjectPointer*> order; for (auto& pointer : engine.pointers()) order.push_back(&pointer); runtimeSortUiNodes(order);
        auto renderPointer = [&](RuntimeObjectPointer* pointer)
        {
            const std::size_t index = static_cast<std::size_t>(pointer - engine.pointers().data()); ImGui::PushID(static_cast<int>(pointer->Id & 0x7fffffffULL));
            const std::string header = std::string(pointer->Name) + (pointer->Resolved ? "  " + runtimeHexAddress(pointer->Address) : "  UNRESOLVED") + "###Pointer" + std::to_string(pointer->Id);
            if (ImGui::CollapsingHeader(header.c_str()))
            {
                bool changed = false; changed |= ImGui::Checkbox("Enabled", &pointer->Enabled); ImGui::SameLine(); if (ImGui::SmallButton("Remove")) erase = index; ImGui::SameLine(); ImGui::SetNextItemWidth(220.0f); changed |= ImGui::InputText("Name", pointer->Name, sizeof(pointer->Name));
                ImGui::SetNextItemWidth(170.0f); changed |= ImGui::InputText("Group", pointer->Group, sizeof(pointer->Group)); ImGui::SameLine(); ImGui::SetNextItemWidth(80.0f); changed |= ImGui::InputInt("Order", &pointer->Order); ImGui::SameLine(); if (ImGui::SmallButton("Up##pointerOrder")) { --pointer->Order; changed = true; } ImGui::SameLine(); if (ImGui::SmallButton("Down##pointerOrder")) { ++pointer->Order; changed = true; }
                RuntimeObjectDescriptor* descriptor = engine.findObject(pointer->DescriptorId); ImGui::SetNextItemWidth(320.0f);
                if (ImGui::BeginCombo("Descriptor model", descriptor ? descriptor->Name : "<select model>")) { for (auto& model : engine.objects()) { const bool selected = model.Id == pointer->DescriptorId; if (ImGui::Selectable(model.Name, selected)) { pointer->DescriptorId = model.Id; changed = true; } if (selected) ImGui::SetItemDefaultFocus(); } ImGui::EndCombo(); }
                changed |= drawRuntimeBindingReferenceCombo(engine, "Base address binding", pointer->BaseBindingId); const RuntimeBinding* process = pointer->ProcessBindingId ? engine.findBinding(pointer->ProcessBindingId) : engine.findBinding(pointer->BaseBindingId); ImGui::SetNextItemWidth(320.0f);
                if (ImGui::BeginCombo("Process binding", pointer->ProcessBindingId == 0 ? "<same as base>" : process ? process->Name : "<missing>")) { if (ImGui::Selectable("<same as base>", pointer->ProcessBindingId == 0)) { pointer->ProcessBindingId = 0; changed = true; } for (const auto& candidate : engine.bindings()) { if (candidate.ProcessId <= 0) continue; const bool selected = candidate.Id == pointer->ProcessBindingId; if (ImGui::Selectable(candidate.Name, selected)) { pointer->ProcessBindingId = candidate.Id; changed = true; } } ImGui::EndCombo(); }
                const int step = 1, fast = 16; ImGui::SetNextItemWidth(150.0f); changed |= ImGui::InputScalar("Base offset", ImGuiDataType_S32, &pointer->BaseOffset, &step, &fast, "0x%X", ImGuiInputTextFlags_CharsHexadecimal);
                ImGui::Text("Resolved: %s   PID: %d", pointer->Resolved ? runtimeHexAddress(pointer->Address).c_str() : "<none>", static_cast<int>(pointer->ProcessId)); if (!pointer->Status.empty()) ImGui::TextDisabled("%s", pointer->Status.c_str());
                if (ImGui::TreeNode("Where did this pointer come from?")) { if (pointer->Provenance.empty()) ImGui::TextDisabled("Waiting for provenance..."); for (const auto& stepText : pointer->Provenance) ImGui::BulletText("%s", stepText.c_str()); ImGui::TreePop(); }
                if (pointer->Resolved && ImGui::Button("Inspect this address")) { inspector.Pid = pointer->ProcessId; inspector.Address = pointer->Address; runtimeRefreshMemoryInspector(inspector); }
                if (descriptor && pointer->Resolved)
                {
                    ImGui::SeparatorText("Fields at this instance");
                    if (ImGui::BeginTable("PointerFields", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable))
                    {
                        ImGui::TableSetupColumn("Field"); ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 80); ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 150); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110); ImGui::TableSetupColumn("Raw", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 65); ImGui::TableHeadersRow();
                        for (auto& field : descriptor->Fields)
                        {
                            if (!field.Enabled || runtimeObjectFieldIsFiller(field.Type)) continue; const auto offset = runtimeObjectFieldOffset(*descriptor, field.Id); const auto addressValue = pointer->Address + offset; ImGui::PushID(static_cast<int>(field.Id & 0x7fffffffULL)); ImGui::TableNextRow();
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(field.Name); ImGui::TableNextColumn(); ImGui::Text("+0x%zX", offset); ImGui::TableNextColumn(); ImGui::Text("0x%llX", static_cast<unsigned long long>(addressValue)); ImGui::TableNextColumn(); ImGui::TextUnformatted(runtimeObjectFieldTypeName(field.Type)); ImGui::TableNextColumn();
                            const std::size_t size = std::min<std::size_t>(runtimeObjectFieldSize(field), 32); std::array<std::uint8_t, 32> raw{}; std::string error; const bool rawOk = readProcessMemoryBlock(pointer->ProcessId, addressValue, std::span<std::uint8_t>(raw).first(size), error); if (rawOk) ImGui::TextUnformatted(runtimeFormatHexBytes(std::span<const std::uint8_t>(raw).first(size)).c_str()); else ImGui::TextDisabled("%s", error.c_str());
                            ImGui::TableNextColumn();
                            const bool pointerLike = field.Type == RuntimeObjectFieldType::Pointer || field.Type == RuntimeObjectFieldType::CStringPointer || field.Type == RuntimeObjectFieldType::WStringPointer;
                            if (pointerLike) { std::uintptr_t target = 0; if (readProcessMemoryValue(pointer->ProcessId, addressValue, target, error)) { ImGui::Text("0x%llX", static_cast<unsigned long long>(target)); if (target != 0 && ImGui::IsItemHovered()) ImGui::SetTooltip("Dereferenced pointer target"); } else ImGui::TextDisabled("%s", error.c_str()); }
                            else ImGui::TextDisabled("-");
                            ImGui::TableNextColumn(); if (ImGui::SmallButton("Inspect")) { inspector.Pid = pointer->ProcessId; inspector.Address = addressValue; runtimeRefreshMemoryInspector(inspector); } ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
                if (changed) engine.markChanged();
            }
            ImGui::Separator(); ImGui::PopID();
                };
        for (auto* pointer : order) if (pointer->Group[0] == '\0') renderPointer(pointer);
        std::vector<std::string> pointerGroups;
        for (const auto* pointer : order) if (pointer->Group[0] != '\0' && std::ranges::find(pointerGroups, std::string(pointer->Group)) == pointerGroups.end()) pointerGroups.emplace_back(pointer->Group);
        for (const auto& group : pointerGroups)
        {
            if (!ImGui::CollapsingHeader((group + "###PointerGroup" + group).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
            ImGui::Indent(8.0f);
            for (auto* pointer : order) if (group == pointer->Group) renderPointer(pointer);
            ImGui::Unindent(8.0f);
        }
        if (erase) engine.erasePointer(*erase); if (engine.pointers().empty()) ImGui::TextDisabled("No pointer assignments yet. Add one and point it at a binding with an exact address.");
    }

    void drawRuntimeObjectDescriptors(RuntimeBindingEngine& engine)
    {
        ImGui::SeparatorText("Object descriptors");
        ImGui::TextWrapped("Object descriptors are reusable layout models only. They do not own a runtime address; assign a model to a real address in the Pointers tab, then bind fields from that pointer instance.");
        if (ImGui::Button("Add object descriptor")) engine.addObject();
        ImGui::SameLine();
        ImGui::TextDisabled("Use filler fields and packing/alignment controls to mirror native layouts, including unwanted padding.");

        constexpr int offsetStep = 1, offsetFastStep = 16;
        std::optional<std::size_t> eraseObject;
        std::vector<RuntimeObjectDescriptor*> objectOrder; for (auto& model : engine.objects()) objectOrder.push_back(&model); runtimeSortUiNodes(objectOrder);
        auto renderObject = [&](RuntimeObjectDescriptor* objectPtr)
        {
            auto& object = *objectPtr; const std::size_t objectIndex = static_cast<std::size_t>(objectPtr - engine.objects().data());
            ImGui::PushID("RuntimeObject");
            ImGui::PushID(static_cast<int>(object.Id & 0x7fffffffULL));
            const std::string header = std::string(object.Name[0] ? object.Name : "Object") + "###RuntimeObject" + std::to_string(object.Id);
            if (!ImGui::CollapsingHeader(header.c_str())) { ImGui::PopID(); ImGui::PopID(); return; }
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled", &object.Enabled);
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove object")) eraseObject = objectIndex;
            ImGui::SameLine(); ImGui::SetNextItemWidth(220.0f); changed |= ImGui::InputText("Name", object.Name, sizeof(object.Name));
            ImGui::SetNextItemWidth(690.0f); changed |= ImGui::InputText("Description", object.Description, sizeof(object.Description));
            ImGui::SetNextItemWidth(180.0f); changed |= ImGui::InputText("Group", object.Group, sizeof(object.Group)); ImGui::SameLine(); ImGui::SetNextItemWidth(80.0f); changed |= ImGui::InputInt("Order", &object.Order); ImGui::SameLine(); if (ImGui::SmallButton("Up##objectOrder")) { --object.Order; changed = true; } ImGui::SameLine(); if (ImGui::SmallButton("Down##objectOrder")) { ++object.Order; changed = true; }

            /* v9 compatibility assignment UI removed: address assignment now lives in the Pointers tab. */
            if (false) changed |= drawRuntimeBindingReferenceCombo(engine, "Base address binding", object.BaseBindingId);
            int packing = static_cast<int>(object.Packing); ImGui::SetNextItemWidth(150.0f);
            if (ImGui::Combo("Packing", &packing, "Natural\0Pack 1\0Pack 2\0Pack 4\0Pack 8\0Pack 16\0")) { object.Packing = static_cast<RuntimeObjectPacking>(packing); changed = true; }
            std::size_t objectSize = 0; runtimeObjectFieldOffset(object, 0, &objectSize);
            ImGui::SameLine(); ImGui::TextDisabled("Model size: %zu B | pointer instances: %zu", objectSize, static_cast<std::size_t>(std::count_if(engine.pointers().begin(), engine.pointers().end(), [&](const RuntimeObjectPointer& p) { return p.DescriptorId == object.Id; })));

            if (ImGui::SmallButton("Add field")) { engine.addObjectField(object); changed = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Add 4-byte filler"))
            {
                auto& field = engine.addObjectField(object);
                std::snprintf(field.Name, sizeof(field.Name), "Padding");
                field.Type = RuntimeObjectFieldType::Filler4;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Add 8-byte filler"))
            {
                auto& field = engine.addObjectField(object);
                std::snprintf(field.Name, sizeof(field.Name), "Padding");
                field.Type = RuntimeObjectFieldType::Filler8;
                changed = true;
            }

            std::optional<std::size_t> eraseField;
            if (ImGui::BeginTable("ObjectFields", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 32.0f);
                ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Align", ImGuiTableColumnFlags_WidthFixed, 75.0f);
                ImGui::TableSetupColumn("Manual", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("String", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 58.0f);
                ImGui::TableHeadersRow();
                for (std::size_t fieldIndex = 0; fieldIndex < object.Fields.size(); ++fieldIndex)
                {
                    auto& field = object.Fields[fieldIndex];
                    ImGui::PushID(static_cast<int>(field.Id & 0x7fffffffULL));
                    const std::size_t offset = runtimeObjectFieldOffset(object, field.Id, &objectSize);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); changed |= ImGui::Checkbox("##on", &field.Enabled);
                    ImGui::TableNextColumn(); ImGui::Text("0x%zX", offset == std::numeric_limits<std::size_t>::max() ? 0 : offset);
                    ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f); changed |= ImGui::InputText("##name", field.Name, sizeof(field.Name));
                    ImGui::TableNextColumn();
                    int type = static_cast<int>(field.Type);
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::Combo("##type", &type, "u8\0i8\0u16\0i16\0u32\0i32\0u64\0i64\0float\0double\0bool\0pointer\0filler 1\0filler 2\0filler 4\0filler 8\0filler 16\0filler 32\0filler custom\0const char*\0const wchar_t*\0char[N]\0wchar_t[N]\0")) { field.Type = static_cast<RuntimeObjectFieldType>(type); changed = true; }
                    ImGui::TableNextColumn();
                    int alignment = static_cast<int>(field.Alignment);
                    ImGui::SetNextItemWidth(-1.0f);
                    static constexpr const char* Alignments[] = {"Auto", "1", "2", "4", "8", "16"};
                    if (ImGui::Combo("##alignment", &alignment, Alignments, static_cast<int>(std::size(Alignments)))) { field.Alignment = static_cast<RuntimeObjectAlignment>(alignment); changed = true; }
                    ImGui::TableNextColumn();
                    changed |= ImGui::Checkbox("##manual", &field.ManualOffset);
                    if (field.ManualOffset) { ImGui::SameLine(); ImGui::SetNextItemWidth(65.0f); changed |= ImGui::InputScalar("##offset", ImGuiDataType_S32, &field.Offset, &offsetStep, &offsetFastStep, "0x%X", ImGuiInputTextFlags_CharsHexadecimal); }
                    ImGui::TableNextColumn();
                    if (field.Type == RuntimeObjectFieldType::FillerCustom)
                    {
                        ImGui::SetNextItemWidth(-1.0f);
                        changed |= ImGui::InputInt("##customSize", &field.CustomFillerBytes);
                        field.CustomFillerBytes = std::max(field.CustomFillerBytes, 1);
                    }
                    else ImGui::Text("%zu", runtimeObjectFieldSize(field));
                    ImGui::TableNextColumn();
                    if (field.Type == RuntimeObjectFieldType::CStringPointer || field.Type == RuntimeObjectFieldType::WStringPointer) { ImGui::SetNextItemWidth(-1.0f); changed |= ImGui::InputInt("##stringMax", &field.StringMaxLength); field.StringMaxLength = std::clamp(field.StringMaxLength, 1, 4096); }
                    else if (field.Type == RuntimeObjectFieldType::FixedCString || field.Type == RuntimeObjectFieldType::FixedWString) { ImGui::SetNextItemWidth(-1.0f); changed |= ImGui::InputInt("##elements", &field.FixedElementCount); field.FixedElementCount = std::clamp(field.FixedElementCount, 1, 4096); }
                    else ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    if (!runtimeObjectFieldIsFiller(field.Type))
                    {
                        if (ImGui::SmallButton("+"))
                        {
                            auto& binding = engine.add();
                            std::snprintf(binding.Name, sizeof(binding.Name), "%s.%s", object.Name, field.Name);
                            binding.Source = RuntimeSourceKind::ObjectField;
                            binding.ObjectId = object.Id;
                            if (const auto it = std::ranges::find_if(engine.pointers(), [&](const RuntimeObjectPointer& p) { return p.DescriptorId == object.Id; }); it != engine.pointers().end()) binding.ObjectPointerId = it->Id;
                            binding.ObjectFieldId = field.Id;
                            binding.WriteMaterial = false;
                            binding.Clamp = false;
                            binding.SmoothingHz = 0.0f;
                            binding.Priority = 10;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create an Object field binding for this field. Pointer fields preserve the exact pointer for chaining another descriptor.");
                        ImGui::SameLine();
                    }
                    if (ImGui::SmallButton("x")) eraseField = fieldIndex;
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (eraseField) { engine.eraseObjectField(object, *eraseField); changed = true; }
            runtimeObjectFieldOffset(object, 0, &object.Size);
            if (changed) engine.markChanged();
            ImGui::PopID(); ImGui::PopID();
                };
        for (auto* object : objectOrder) if (object->Group[0] == '\0') renderObject(object);
        std::vector<std::string> objectGroups;
        for (const auto* object : objectOrder) if (object->Group[0] != '\0' && std::ranges::find(objectGroups, std::string(object->Group)) == objectGroups.end()) objectGroups.emplace_back(object->Group);
        for (const auto& group : objectGroups)
        {
            if (!ImGui::CollapsingHeader((group + "###ObjectGroup" + group).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
            ImGui::Indent(8.0f);
            for (auto* object : objectOrder) if (group == object->Group) renderObject(object);
            ImGui::Unindent(8.0f);
        }
        if (eraseObject) engine.eraseObject(*eraseObject);
        if (engine.objects().empty()) ImGui::TextDisabled("No object descriptors yet. Create a Native process address binding first, capture the object pointer, then describe its fields here.");
    }
    bool drawProfileHotkey(RuntimeBindingProfile& profile)
    {
        struct KeyOption { const char* Name; int Key; };
        static constexpr KeyOption Keys[] = {
            {"None", 0}, {"F1", GLFW_KEY_F1}, {"F2", GLFW_KEY_F2}, {"F3", GLFW_KEY_F3}, {"F4", GLFW_KEY_F4}, {"F5", GLFW_KEY_F5}, {"F6", GLFW_KEY_F6},
            {"F7", GLFW_KEY_F7}, {"F8", GLFW_KEY_F8}, {"F9", GLFW_KEY_F9}, {"F10", GLFW_KEY_F10}, {"F11", GLFW_KEY_F11}, {"F12", GLFW_KEY_F12},
            {"1", GLFW_KEY_1}, {"2", GLFW_KEY_2}, {"3", GLFW_KEY_3}, {"4", GLFW_KEY_4}, {"5", GLFW_KEY_5}, {"6", GLFW_KEY_6}, {"7", GLFW_KEY_7}, {"8", GLFW_KEY_8}, {"9", GLFW_KEY_9}, {"0", GLFW_KEY_0},
            {"A", GLFW_KEY_A}, {"B", GLFW_KEY_B}, {"C", GLFW_KEY_C}, {"D", GLFW_KEY_D}, {"E", GLFW_KEY_E}, {"F", GLFW_KEY_F}, {"G", GLFW_KEY_G}, {"H", GLFW_KEY_H}, {"I", GLFW_KEY_I}, {"J", GLFW_KEY_J},
            {"K", GLFW_KEY_K}, {"L", GLFW_KEY_L}, {"M", GLFW_KEY_M}, {"N", GLFW_KEY_N}, {"O", GLFW_KEY_O}, {"P", GLFW_KEY_P}, {"Q", GLFW_KEY_Q}, {"R", GLFW_KEY_R}, {"S", GLFW_KEY_S}, {"T", GLFW_KEY_T},
            {"U", GLFW_KEY_U}, {"V", GLFW_KEY_V}, {"W", GLFW_KEY_W}, {"X", GLFW_KEY_X}, {"Y", GLFW_KEY_Y}, {"Z", GLFW_KEY_Z}
        };
        bool changed = false;
        changed |= ImGui::Checkbox("Ctrl", &profile.HotkeyCtrl); ImGui::SameLine(); changed |= ImGui::Checkbox("Alt", &profile.HotkeyAlt); ImGui::SameLine(); changed |= ImGui::Checkbox("Shift", &profile.HotkeyShift); ImGui::SameLine();
        const char* preview = "None"; for (const auto& key : Keys) if (key.Key == profile.HotkeyKey) { preview = key.Name; break; }
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::BeginCombo("Key", preview))
        {
            for (const auto& key : Keys) { const bool selected = key.Key == profile.HotkeyKey; if (ImGui::Selectable(key.Name, selected)) { profile.HotkeyKey = key.Key; changed = true; } if (selected) ImGui::SetItemDefaultFocus(); }
            ImGui::EndCombo();
        }
        return changed;
    }

    void drawRuntimeValueBank(RuntimeBindingEngine& engine)
    {
        ImGui::TextUnformatted("Value bank"); ImGui::SameLine(); if (ImGui::Button("+ Add value")) engine.addBankValue();
        ImGui::SameLine(); ImGui::TextDisabled("Persistent scratch values survive source loss/restarts and can hold numbers, booleans, strings, or exact addresses.");
        std::optional<std::size_t> erase;
        if (ImGui::BeginTable("RuntimeValueBank", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 48.0f); ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110.0f); ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Description"); ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, 55.0f); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 34.0f); ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < engine.bank().size(); ++i)
            {
                auto& value = engine.bank()[i]; bool changed = false, valueChanged = false; ImGui::PushID(static_cast<int>(value.Id & 0x7fffffffULL)); ImGui::TableNextRow();
                ImGui::TableNextColumn(); changed |= ImGui::Checkbox("##enabled", &value.Enabled); ImGui::SameLine(); const bool bankGood = value.HasValue && (value.Type == RuntimeBankValueType::Boolean ? value.Boolean : value.Type == RuntimeBankValueType::Address ? value.Address != 0 : value.Type == RuntimeBankValueType::String ? value.String[0] != '\0' : true); drawRuntimeStateSquare("##bankState", runtimeStateColor(value.Enabled, bankGood, false, !value.HasValue));
                ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f); changed |= ImGui::InputText("##name", value.Name, sizeof(value.Name));
                ImGui::TableNextColumn(); int type = static_cast<int>(value.Type); ImGui::SetNextItemWidth(-1.0f); if (ImGui::Combo("##type", &type, "Number\0Integer\0Boolean\0String\0Address\0")) { value.Type = static_cast<RuntimeBankValueType>(type); changed = valueChanged = true; }
                ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f);
                if (value.Type == RuntimeBankValueType::Number) { if (ImGui::DragFloat("##number", &value.Number, 0.01f)) changed = valueChanged = true; }
                else if (value.Type == RuntimeBankValueType::Integer) { long long temp = static_cast<long long>(value.Integer); if (ImGui::InputScalar("##integer", ImGuiDataType_S64, &temp)) { value.Integer = static_cast<std::int64_t>(temp); changed = valueChanged = true; } }
                else if (value.Type == RuntimeBankValueType::Boolean) { if (ImGui::Checkbox("##boolean", &value.Boolean)) changed = valueChanged = true; }
                else if (value.Type == RuntimeBankValueType::String) { if (ImGui::InputText("##string", value.String, sizeof(value.String))) changed = valueChanged = true; }
                else { unsigned long long address = static_cast<unsigned long long>(value.Address); if (ImGui::InputScalar("##address", ImGuiDataType_U64, &address, nullptr, nullptr, "0x%llX", ImGuiInputTextFlags_CharsHexadecimal)) { value.Address = static_cast<std::uintptr_t>(address); changed = valueChanged = true; } }
                ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f); changed |= ImGui::InputText("##description", value.Description, sizeof(value.Description));
                ImGui::TableNextColumn(); ImGui::Text("#%llu", static_cast<unsigned long long>(value.Id));
                ImGui::TableNextColumn(); if (ImGui::SmallButton("x")) erase = i;
                if (valueChanged) { value.HasValue = true; value.ChangedThisFrame = true; }
                if (changed) engine.markChanged();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (erase) engine.eraseBankValue(*erase);
        if (engine.bank().empty()) ImGui::TextDisabled("No bank values yet. Add one to remember state such as the shader that was active before a game took over.");
    }

    void drawRuntimeProfiles(RuntimeBindingEngine& engine)
    {
        ImGui::TextUnformatted("Binding profiles"); ImGui::SameLine(); if (ImGui::Button("+ Add profile")) engine.addProfile();
        ImGui::SameLine(); ImGui::TextDisabled("Profiles mass-enable/disable graph nodes and can be activated with a key combination while Quartz has keyboard focus.");
        if (!engine.profiles().empty())
        {
            const RuntimeBindingProfile* active = engine.findProfile(engine.activeProfileId());
            ImGui::SetNextItemWidth(280.0f);
            if (ImGui::BeginCombo("Active profile", active ? active->Name : "<manual / none>"))
            {
                if (ImGui::Selectable("<manual / none>", engine.activeProfileId() == 0)) engine.clearActiveProfile();
                for (auto& profile : engine.profiles()) { const bool selected = profile.Id == engine.activeProfileId(); if (ImGui::Selectable(profile.Name, selected)) engine.applyProfile(profile); if (selected) ImGui::SetItemDefaultFocus(); }
                ImGui::EndCombo();
            }
        }
        std::optional<std::size_t> erase;
        for (std::size_t i = 0; i < engine.profiles().size(); ++i)
        {
            auto& profile = engine.profiles()[i]; ImGui::PushID(static_cast<int>(profile.Id & 0x7fffffffULL));
            drawRuntimeStateSquare("##profileState", runtimeStateColor(profile.Enabled, profile.Id == engine.activeProfileId())); ImGui::SameLine();
            const std::string header = std::string(profile.Name) + (profile.Id == engine.activeProfileId() ? "  ACTIVE" : "") + "###Profile" + std::to_string(profile.Id);
            if (ImGui::CollapsingHeader(header.c_str()))
            {
                bool changed = false; changed |= ImGui::Checkbox("Enabled", &profile.Enabled); ImGui::SameLine(); ImGui::SetNextItemWidth(220.0f); changed |= ImGui::InputText("Name", profile.Name, sizeof(profile.Name));
                changed |= ImGui::Checkbox("Exclusive activation", &profile.Exclusive); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Exclusive profiles disable every binding/control first, then enable their selected members.");
                if (ImGui::Button("Activate")) engine.applyProfile(profile); ImGui::SameLine(); if (ImGui::Button("Enable members")) engine.setProfileMembersEnabled(profile, true); ImGui::SameLine(); if (ImGui::Button("Disable members")) engine.setProfileMembersEnabled(profile, false); ImGui::SameLine(); if (ImGui::Button("Remove profile")) erase = i;
                ImGui::SeparatorText("Hotkey"); changed |= drawProfileHotkey(profile);
                ImGui::SeparatorText("Bindings");
                if (ImGui::BeginTable("ProfileBindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable))
                {
                    ImGui::TableSetupColumn("Member", ImGuiTableColumnFlags_WidthFixed, 65.0f); ImGui::TableSetupColumn("Binding"); ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthFixed, 80.0f); ImGui::TableHeadersRow();
                    for (const auto& binding : engine.bindings()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); bool member = std::find(profile.BindingIds.begin(), profile.BindingIds.end(), binding.Id) != profile.BindingIds.end(); if (ImGui::Checkbox(("##pb" + std::to_string(binding.Id)).c_str(), &member)) { if (member) profile.BindingIds.push_back(binding.Id); else std::erase(profile.BindingIds, binding.Id); changed = true; } ImGui::TableNextColumn(); ImGui::TextUnformatted(binding.Name); ImGui::TableNextColumn(); ImGui::TextColored(binding.Enabled ? ImVec4(0.2f,0.8f,0.3f,1) : ImVec4(0.8f,0.25f,0.25f,1), "%s", binding.Enabled ? "enabled" : "disabled"); }
                    ImGui::EndTable();
                }
                ImGui::SeparatorText("Controls");
                if (ImGui::BeginTable("ProfileControls", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable))
                {
                    ImGui::TableSetupColumn("Member", ImGuiTableColumnFlags_WidthFixed, 65.0f); ImGui::TableSetupColumn("Control"); ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthFixed, 80.0f); ImGui::TableHeadersRow();
                    for (const auto& control : engine.controls()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); bool member = std::find(profile.ControlIds.begin(), profile.ControlIds.end(), control.Id) != profile.ControlIds.end(); if (ImGui::Checkbox(("##pc" + std::to_string(control.Id)).c_str(), &member)) { if (member) profile.ControlIds.push_back(control.Id); else std::erase(profile.ControlIds, control.Id); changed = true; } ImGui::TableNextColumn(); ImGui::TextUnformatted(control.Name); ImGui::TableNextColumn(); ImGui::TextColored(control.Enabled ? ImVec4(0.2f,0.8f,0.3f,1) : ImVec4(0.8f,0.25f,0.25f,1), "%s", control.Enabled ? "enabled" : "disabled"); }
                    ImGui::EndTable();
                }
                if (changed) engine.markChanged();
            }
            ImGui::Separator(); ImGui::PopID();
        }
        if (erase) engine.eraseProfile(*erase);
    }

    void drawGroupedBindings(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer)
    {
        std::vector<RuntimeBinding*> order; for (auto& binding : engine.bindings()) order.push_back(&binding); runtimeSortUiNodes(order); std::set<std::string> groups; for (auto* binding : order) if (binding->Group[0]) groups.insert(binding->Group);
        std::optional<std::size_t> erase;
        auto drawOne = [&](RuntimeBinding& binding)
        {
            bool shouldErase = false;
            ImGui::PushID(static_cast<int>(binding.Id & 0x7fffffffULL));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
            if (ImGui::BeginChild("##BindingCard", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) drawRuntimeBinding(engine, shaderFramebuffer, binding, shouldErase);
            ImGui::EndChild(); ImGui::PopStyleVar(2); ImGui::PopID(); ImGui::Dummy(ImVec2(0.0f, 8.0f));
            if (shouldErase) erase = static_cast<std::size_t>(&binding - engine.bindings().data());
        };
        for (auto* binding : order) if (!binding->Group[0]) drawOne(*binding);
        for (const auto& group : groups) if (ImGui::CollapsingHeader((group + "###BindingGroup" + group).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) { ImGui::Indent(8.0f); for (auto* binding : order) if (group == binding->Group) drawOne(*binding); ImGui::Unindent(8.0f); }
        if (erase) engine.erase(*erase);
    }

    void drawGroupedControls(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer)
    {
        std::vector<RuntimeControlRule*> order; for (auto& control : engine.controls()) order.push_back(&control); runtimeSortUiNodes(order); std::set<std::string> groups; for (auto* control : order) if (control->Group[0]) groups.insert(control->Group);
        std::optional<std::size_t> erase;
        auto drawOne = [&](RuntimeControlRule& control)
        {
            bool shouldErase = false;
            ImGui::PushID(static_cast<int>(control.Id & 0x7fffffffULL));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
            if (ImGui::BeginChild("##ControlCard", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) drawRuntimeControlRule(engine, shaderFramebuffer, control, shouldErase);
            ImGui::EndChild(); ImGui::PopStyleVar(2); ImGui::PopID(); ImGui::Dummy(ImVec2(0.0f, 8.0f));
            if (shouldErase) erase = static_cast<std::size_t>(&control - engine.controls().data());
        };
        for (auto* control : order) if (!control->Group[0]) drawOne(*control);
        for (const auto& group : groups) if (ImGui::CollapsingHeader((group + "###ControlGroup" + group).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) { ImGui::Indent(8.0f); for (auto* control : order) if (group == control->Group) drawOne(*control); ImGui::Unindent(8.0f); }
        if (erase) engine.eraseControl(*erase);
    }

    void drawRuntimeBindingsPage(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer)
    {
        ImGui::TextWrapped("Runtime bindings are priority-ordered dataflow nodes. Related runtime tools now live in their own pages instead of a nested mega-tab.");
        drawGroupedBindings(engine, shaderFramebuffer);
    }


    void drawQRPCInspectorPage(RuntimeTelemetry& telemetry)
    {
        ImGui::SeparatorText("QRPC packet inspector");
        static int selectedNewest = 0;
        if (ImGui::Button("Clear packets")) { telemetry.clearPackets(); selectedNewest = 0; }
        ImGui::SameLine();
        ImGui::TextDisabled("Assembled QRPC packets, not raw libusb transfer boundaries. Click a packet for a captured hex view.");
        const auto packets = telemetry.packets();
        if (!packets.empty()) selectedNewest = std::clamp(selectedNewest, 0, static_cast<int>(packets.size()) - 1);
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
                    selectedNewest = newestIndex;
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
        ImGui::Text("Buffered packets: %zu / 512", packets.size());
        if (!packets.empty())
        {
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
                        if (offset + i < packet.CapturedBytes)
                            written += std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written), "%02X ", std::to_integer<unsigned>(packet.Data[offset + i]));
                        else
                            written += std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written), "   ");
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

    void drawUSBProfilerPage(RawUSB& usb, const RuntimeBindingEngine& bindings)
    {
        const auto stats = usb.stats();
        const auto& rates = bindings.usbRates();
        ImGui::SeparatorText("USB transport");
        ImGui::Text("Device: %04X:%04X   interface %d   OUT 0x%02X   IN 0x%02X", VendorId, ProductId, RPCInterfaceNumber, RPCOutEndpoint, RPCInEndpoint);
        ImGui::Text("State: %s   product: %s", usb.isConnected() ? "connected" : "disconnected", usb.deviceName().empty() ? "Quartz K552X" : usb.deviceName().c_str());
        ImGui::Text("TX %.2f KiB/s  %.1f transfers/s   RX %.2f KiB/s  %.1f transfers/s", rates.TxKiB, rates.TxTransfers, rates.RxKiB, rates.RxTransfers);
        ImGui::Text("TX total %.2f MiB / %llu transfers   RX total %.2f MiB / %llu transfers", stats.TxBytes / (1024.0 * 1024.0), static_cast<unsigned long long>(stats.TxTransfers), stats.RxBytes / (1024.0 * 1024.0), static_cast<unsigned long long>(stats.RxTransfers));
        ImGui::Text("Transfer latency: TX %.3f ms   RX %.3f ms", stats.LastTxMilliseconds, stats.LastRxMilliseconds);
        ImGui::Text("Errors: TX %llu   RX %llu   connects %llu   disconnects %llu", static_cast<unsigned long long>(stats.TxErrors), static_cast<unsigned long long>(stats.RxErrors), static_cast<unsigned long long>(stats.Connects), static_cast<unsigned long long>(stats.Disconnects));
        if (usb.lastError() != LIBUSB_SUCCESS) ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.48f, 1.0f), "Last libusb status: %s", libusb_error_name(usb.lastError()));
    }

    void drawInputAnalyzerPage(const EvdevKeyboard& keyboard, const ReactiveKeyState& keys, const RuntimeInputAnalytics& analytics)
    {
        const float held = std::accumulate(keys.Down.begin(), keys.Down.end(), 0.0f);
        ImGui::SeparatorText("Global keyboard input");
        ImGui::Text("evdev: %s", keyboard.connected() ? "connected" : "disconnected");
        ImGui::TextWrapped("%s", keyboard.status().c_str());
        ImGui::Text("Held mapped keys: %.0f   total presses: %llu   longest press: %.3f s   Caps %s   Scroll %s", held, static_cast<unsigned long long>(analytics.TotalPresses), analytics.LongestPress, keys.CapsLockActive ? "ON" : "off", keys.ScrollLockActive ? "ON" : "off");

        std::array<Color32, MatrixSize> stateFramebuffer{};
        for (std::size_t i = 0; i < MatrixSize; ++i)
            if (keys.Down[i] > 0.5f) stateFramebuffer[i] = {220, 220, 220};
        ImGui::TextUnformatted("Live state");
        drawFramebufferPreview(stateFramebuffer, 0.55f, 532.0f, 0.12f);

        const auto maxPressIt = std::max_element(analytics.PressCount.begin(), analytics.PressCount.end());
        const std::uint64_t maxPresses = maxPressIt != analytics.PressCount.end() ? *maxPressIt : 0;
        std::array<Color32, MatrixSize> heatmap{};
        if (maxPresses != 0)
        {
            for (std::size_t i = 0; i < MatrixSize; ++i)
            {
                const float amount = std::sqrt(static_cast<float>(analytics.PressCount[i]) / static_cast<float>(maxPresses));
                const auto value = static_cast<std::uint8_t>(std::lround(amount * 255.0f));
                heatmap[i] = {value, value, value};
            }
        }
        ImGui::Text("Press-count heatmap   max/key %llu", static_cast<unsigned long long>(maxPresses));
        drawFramebufferPreview(heatmap, 0.55f, 532.0f, 0.08f);

        ImGui::SeparatorText("Recent mapped key events");
        if (ImGui::BeginTable("RecentKeyEvents", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 230.0f)))
        {
            ImGui::TableSetupColumn("Slot");
            ImGui::TableSetupColumn("Row");
            ImGui::TableSetupColumn("Column");
            ImGui::TableSetupColumn("Age");
            ImGui::TableSetupColumn("Presses");
            ImGui::TableSetupColumn("Held / last");
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < keys.Events.size(); ++i)
            {
                const auto& event = keys.Events[i];
                if (event.Valid < 0.5f) continue;
                const std::size_t row = static_cast<std::size_t>(std::clamp(event.Row, 0.0f, static_cast<float>(Rows - 1)));
                const std::size_t column = static_cast<std::size_t>(std::clamp(event.Column, 0.0f, static_cast<float>(Columns - 1)));
                const std::size_t index = row * Columns + column;
                const double heldFor = analytics.heldDuration(index, ImGui::GetTime());
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%zu", i);
                ImGui::TableNextColumn(); ImGui::Text("%zu", row);
                ImGui::TableNextColumn(); ImGui::Text("%zu", column);
                ImGui::TableNextColumn(); ImGui::Text("%.3f s", std::max(static_cast<float>(ImGui::GetTime()) - event.Time, 0.0f));
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(analytics.PressCount[index]));
                ImGui::TableNextColumn(); ImGui::Text("%.3f s", heldFor > 0.0 ? heldFor : analytics.LastDuration[index]);
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Ctrl+Alt+Shift+Q remains the global recovery shortcut when the Quartz window is hidden.");
    }

    void drawRGBProfilerPage(const std::array<Color32, MatrixSize>& framebuffer, const VisualizerSettings& settings, const RuntimeRGBAnalytics& analytics)
    {
        std::array<float, ActiveProbeRows> rowLuma{};
        float r = 0.0f, g = 0.0f, b = 0.0f, lit = 0.0f, peak = 0.0f;
        for (std::size_t row = 0; row < ActiveProbeRows; ++row)
        {
            for (std::size_t column = 0; column < Columns; ++column)
            {
                const auto& color = framebuffer[row * Columns + column];
                const float rf = color.R / 255.0f, gf = color.G / 255.0f, bf = color.B / 255.0f;
                const float luma = rf * 0.2126f + gf * 0.7152f + bf * 0.0722f;
                r += rf; g += gf; b += bf; rowLuma[row] += luma; peak = std::max(peak, std::max({rf, gf, bf}));
                if (color.R || color.G || color.B) lit += 1.0f;
            }
            rowLuma[row] /= static_cast<float>(Columns);
        }
        constexpr float Count = static_cast<float>(ActiveProbeRows * Columns);
        r /= Count; g /= Count; b /= Count;
        ImGui::SeparatorText("Framebuffer / RGB profiler");
        ImGui::Text("Output %.1f Hz   changed this frame %zu / %.0f   EMA changed %.1f cells/frame", analytics.FrameRate, analytics.ChangedCells, Count, analytics.AverageChangedCells);
        ImGui::Text("Active cells: %.0f / %.0f (%.1f%%)   peak channel %.1f%%", lit, Count, lit / Count * 100.0f, peak * 100.0f);
        ImGui::Text("Average RGB: %.1f%% / %.1f%% / %.1f%%   luma %.1f%%", r * 100.0f, g * 100.0f, b * 100.0f, (r * 0.2126f + g * 0.7152f + b * 0.0722f) * 100.0f);
        ImGui::Text("Frames profiled %llu   total cell changes %llu   Global brightness %.0f%%   preview interpolation %.0f%%", static_cast<unsigned long long>(analytics.Frames), static_cast<unsigned long long>(analytics.ChangedCellsTotal), settings.GlobalBrightness * 100.0f, settings.LiveOutputInterpolation * 100.0f);
        ImGui::PlotHistogram("Luma distribution", analytics.LumaHistogram.data(), static_cast<int>(analytics.LumaHistogram.size()), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 90.0f));
        ImGui::PlotHistogram("Per-row luma", rowLuma.data(), static_cast<int>(rowLuma.size()), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 100.0f));
        drawFramebufferPreview(framebuffer, 0.55f, 532.0f, settings.LiveOutputInterpolation);
    }

    void drawAudioLabPage(AudioSpectrum& audio, const AudioLevelSnapshot& level, const AutoGainState& autoGain, VisualizerSettings& settings, const std::array<float, FFTSize>& analysisBands, const std::array<float, Columns>& mappedBands, const std::array<float, Columns>& smoothedBands)
    {
        static std::array<float, 300> rmsHistory{};
        static std::array<float, 300> peakHistory{};
        static double nextHistory = 0.0;
        if (ImGui::GetTime() >= nextHistory)
        {
            std::move(rmsHistory.begin() + 1, rmsHistory.end(), rmsHistory.begin());
            std::move(peakHistory.begin() + 1, peakHistory.end(), peakHistory.begin());
            rmsHistory.back() = level.Rms;
            peakHistory.back() = level.Peak;
            nextHistory = ImGui::GetTime() + 1.0 / 30.0;
        }

        ImGui::SeparatorText("Audio level / normalization");
        ImGui::Text("Capture: %s", audio.source().c_str());
        ImGui::Text("RMS %.5f   peak %.5f", level.Rms, level.Peak);
        ImGui::PlotLines("RMS history", rmsHistory.data(), static_cast<int>(rmsHistory.size()), 0, nullptr, 0.0f, std::max(0.25f, *std::max_element(peakHistory.begin(), peakHistory.end())), ImVec2(-1.0f, 100.0f));
        ImGui::PlotLines("Peak history", peakHistory.data(), static_cast<int>(peakHistory.size()), 0, nullptr, 0.0f, std::max(0.25f, *std::max_element(peakHistory.begin(), peakHistory.end())), ImVec2(-1.0f, 70.0f));

        ImGui::Checkbox("Enable automatic overall gain", &settings.AutomaticOverallGain);
        if (settings.AutomaticOverallGain)
        {
            ImGui::SliderFloat("Baseline gain", &settings.AutoGainBaseline, 0.10f, 6.0f, "%.2fx");
            ImGui::SliderFloat("Target long-term RMS", &settings.AutoGainTargetRms, 0.01f, 0.50f, "%.3f");
            ImGui::SliderFloat("Adaptation speed", &settings.AutoGainAdaptation, 0.02f, 2.0f, "%.2f /s", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Minimum correction", &settings.AutoGainMinCorrection, 0.10f, 1.0f, "%.2fx");
            ImGui::SliderFloat("Maximum correction", &settings.AutoGainMaxCorrection, 1.0f, 8.0f, "%.2fx");
            ImGui::SliderFloat("Silence gate", &settings.AutoGainSilenceGate, 0.0f, 0.05f, "%.4f");
            ImGui::Text("Learned RMS %.5f   correction %.3fx   effective gain %.3fx", autoGain.LongTermRms, autoGain.Correction, autoGain.EffectiveGain);
            ImGui::TextDisabled("The baseline is the normal gain. Long-term media loudness only applies a slow bounded correction around it; silence is ignored.");
        }
        else
        {
            ImGui::SliderFloat("Manual overall gain", &settings.OverallGain, 0.10f, 4.0f, "%.2fx");
            ImGui::Text("Effective gain %.3fx", autoGain.EffectiveGain);
        }

        ImGui::SeparatorText("Spectrum pipeline");
        ImGui::PlotLines("FFT / log analysis", analysisBands.data(), settings.AnalysisBandCount, 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 120.0f));
        ImGui::PlotHistogram("Mapped", mappedBands.data(), static_cast<int>(Columns), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 90.0f));
        ImGui::PlotHistogram("Attack / release output", smoothedBands.data(), static_cast<int>(Columns), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 90.0f));
    }

    void drawTimelinePage(RuntimeTelemetry& telemetry)
    {
        if (ImGui::Button("Clear timeline")) telemetry.clearEvents();
        ImGui::SameLine();
        ImGui::TextDisabled("USB/media/input/runtime events");
        const auto events = telemetry.events();
        if (ImGui::BeginTable("RuntimeTimeline", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 470.0f)))
        {
            ImGui::TableSetupColumn("t", ImGuiTableColumnFlags_WidthFixed, 85.0f);
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (auto it = events.rbegin(); it != events.rend(); ++it)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%.3f", it->Time);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->Category.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->Text.c_str());
            }
            ImGui::EndTable();
        }
    }

    void drawFirmwarePage(const PerformanceSnapshot& performance, const bool hasPerformance, const MatrixTimingProbeResult<ActiveProbeRows>& timingProbe, const bool hasTimingProbe)
    {
        ImGui::SeparatorText("Firmware identity");
        ImGui::Text("Quartz K552X firmware %s", FirmwareVersion);
        ImGui::Text("QRPC protocol v%u   VID:PID %04X:%04X", static_cast<unsigned>(ProtocolVersion), VendorId, ProductId);
        ImGui::Text("RPC interface %d   bulk OUT 0x%02X   bulk IN 0x%02X", RPCInterfaceNumber, RPCOutEndpoint, RPCInEndpoint);
        ImGui::Text("Framebuffer %zux%zu logical / %zux%zu active RGB   Color32 %zu B", Columns, Rows, Columns, ActiveProbeRows, sizeof(Color32));
        ImGui::Text("PacketHeader %zu B   framebuffer payload %zu B", sizeof(PacketHeader), sizeof(FramebufferSetPayload<MatrixSize>));
        ImGui::SeparatorText("Live firmware telemetry");
        if (hasPerformance) drawPerformance(performance); else ImGui::TextDisabled("Waiting for PerformanceResponse...");
        ImGui::SeparatorText("Matrix settle probe");
        if (hasTimingProbe) drawTimingProbe(timingProbe); else ImGui::TextDisabled("No matrix timing probe result received yet.");
    }

    bool defaultAudioSourceButton(const char* id, VisualizerSettings& settings, const VisualizerSettings& defaults, AudioSpectrum& audio)
    {
        const bool isDefault = std::strcmp(settings.AudioSource, defaults.AudioSource) == 0;
        ImGui::SameLine();
        if (isDefault) ImGui::BeginDisabled();
        char label[96];
        std::snprintf(label, sizeof(label), "Default##%s", id);
        const bool pressed = ImGui::SmallButton(label);
        if (isDefault) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Restore default");
        if (pressed)
        {
            std::snprintf(settings.AudioSource, sizeof(settings.AudioSource), "%s", defaults.AudioSource);
            audio.start(settings.AudioSource);
        }
        return pressed;
    }

    void applyDarkTheme()
    {
        ImGui::StyleColorsDark();
        auto& style = ImGui::GetStyle();
        for (auto& color : style.Colors)
        {
            const float gray = color.x * 0.2126f + color.y * 0.7152f + color.z * 0.0722f;
            color.x = gray;
            color.y = gray;
            color.z = gray;
        }
    }
#if IMGUI_VERSION_NUM >= 19200
    ImFont* ShaderEditorFont = nullptr;
#else
    std::array<ImFont*, ShaderEditorZoomLevels.size()> ShaderEditorFonts{};
#endif

    float shaderEditorPixelSize(const float zoom) noexcept { return std::max(8.0f, std::round(ShaderEditorBaseSize * zoom)); }

    void initializeShaderEditorFonts()
    {
        ImGuiIO& io = ImGui::GetIO();
#if IMGUI_VERSION_NUM >= 19200
        // ImGui 1.92+ can rasterize the scalable embedded font at the requested size on demand.
        // This avoids stretching one cached bitmap when the editor is zoomed.
        ShaderEditorFont = io.Fonts->AddFontDefaultVector();
#else
        // Older ImGui has fixed-size fonts, so bake one font per zoom level at an integer pixel size.
        for (std::size_t i = 0; i < ShaderEditorZoomLevels.size(); ++i)
        {
            ImFontConfig config{};
            config.SizePixels = shaderEditorPixelSize(ShaderEditorZoomLevels[i]);
            config.OversampleH = 2;
            config.OversampleV = 2;
            config.PixelSnapH = true;
            ShaderEditorFonts[i] = io.Fonts->AddFontDefault(&config);
        }
#endif
    }

    std::size_t shaderEditorZoomIndex(float& zoom) noexcept
    {
        std::size_t best = 0;
        float bestDistance = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < ShaderEditorZoomLevels.size(); ++i)
        {
            const float distance = std::abs(ShaderEditorZoomLevels[i] - zoom);
            if (distance < bestDistance) { bestDistance = distance; best = i; }
        }
        zoom = ShaderEditorZoomLevels[best];
        return best;
    }

    void updateShaderEditorZoomShortcuts(ShaderEditorState& shaderEditor, VisualizerSettings& settings, const bool editorHovered)
    {
        GLFWwindow* window = glfwGetCurrentContext();
        if (!window) return;
        const bool ctrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
        const bool zoomInDown = ctrl && (glfwGetKey(window, GLFW_KEY_EQUAL) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_KP_ADD) == GLFW_PRESS);
        const bool zoomOutDown = ctrl && (glfwGetKey(window, GLFW_KEY_MINUS) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS);
        const bool zoomResetDown = ctrl && glfwGetKey(window, GLFW_KEY_0) == GLFW_PRESS;
        if (zoomInDown && !shaderEditor.ZoomInWasDown) settings.ShaderEditorZoom = std::min(settings.ShaderEditorZoom + 0.10f, 2.50f);
        if (zoomOutDown && !shaderEditor.ZoomOutWasDown) settings.ShaderEditorZoom = std::max(settings.ShaderEditorZoom - 0.10f, 0.60f);
        if (zoomResetDown && !shaderEditor.ZoomResetWasDown) settings.ShaderEditorZoom = 1.0f;
        shaderEditor.ZoomInWasDown = zoomInDown;
        shaderEditor.ZoomOutWasDown = zoomOutDown;
        shaderEditor.ZoomResetWasDown = zoomResetDown;
        if (editorHovered && ctrl && ImGui::GetIO().MouseWheel != 0.0f) settings.ShaderEditorZoom = std::clamp(settings.ShaderEditorZoom + ImGui::GetIO().MouseWheel * 0.10f, 0.60f, 2.50f);
        settings.ShaderEditorZoom = std::round(settings.ShaderEditorZoom * 10.0f) / 10.0f;
    }

    bool renderShaderTextEditor(TextEditor& editor, const char* id, const ImVec2 size, ShaderEditorState& shaderEditor, VisualizerSettings& settings)
    {
        updateShaderEditorZoomShortcuts(shaderEditor, settings, false);
        const std::size_t zoomIndex = shaderEditorZoomIndex(settings.ShaderEditorZoom);
#if IMGUI_VERSION_NUM >= 19200
        ImGui::PushFont(ShaderEditorFont, shaderEditorPixelSize(settings.ShaderEditorZoom));
#else
        ImGui::PushFont(ShaderEditorFonts[zoomIndex] ? ShaderEditorFonts[zoomIndex] : ImGui::GetFont());
#endif
        const bool changed = editor.Render(id, size);
        ImGui::PopFont();
        const bool hovered = ImGui::IsItemHovered();
        updateShaderEditorZoomShortcuts(shaderEditor, settings, hovered);
        return changed;
    }

}
