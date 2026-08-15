
    class RuntimeTelemetry
    {
    public:
        void event(const double time, std::string category, std::string text)
        {
            std::lock_guard lock(_mutex);
            _events.push_back({time, std::move(category), std::move(text)});
            while (_events.size() > 512) _events.pop_front();
        }

        void packet(const double time, const bool tx, const PacketHeader& header, const std::span<const std::byte> bytes)
        {
            std::lock_guard lock(_mutex);
            RuntimePacketRecord record;
            record.Time = time;
            record.Tx = tx;
            record.Type = static_cast<std::uint16_t>(header.Type);
            record.Version = header.Version;
            record.PacketId = header.PacketId;
            record.ResponseFor = header.ResponseFor;
            record.PayloadLength = header.PayloadLength;
            record.Bytes = bytes.size();
            record.CapturedBytes = std::min(bytes.size(), record.Data.size());
            std::copy_n(bytes.begin(), record.CapturedBytes, record.Data.begin());
            _packets.push_back(record);
            while (_packets.size() > 512) _packets.pop_front();
        }

        std::deque<RuntimeTimelineEvent> events() const { std::lock_guard lock(_mutex); return _events; }
        std::deque<RuntimePacketRecord> packets() const { std::lock_guard lock(_mutex); return _packets; }
        void clearEvents() { std::lock_guard lock(_mutex); _events.clear(); }
        void clearPackets() { std::lock_guard lock(_mutex); _packets.clear(); }

    private:
        mutable std::mutex _mutex;
        std::deque<RuntimeTimelineEvent> _events;
        std::deque<RuntimePacketRecord> _packets;
    };

    static void mapSpectrumToColumns(const std::span<const float> analysisBands, std::array<float, Columns>& bands, const VisualizerSettings& settings, const float overallGain)
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

    static void smoothBands(const std::array<float, Columns>& bands, std::array<float, Columns>& smoothedBands, const VisualizerSettings& settings, const float dt)
    {
        for (std::size_t i = 0; i < Columns; ++i)
        {
            const float target = bands[i];
            const float speed = target > smoothedBands[i] ? settings.AttackSpeed : settings.ReleaseSpeed;
            const float alpha = 1.0f - std::exp(-speed * dt);
            smoothedBands[i] += (target - smoothedBands[i]) * alpha;
        }
    }

    static void renderAudioRGB(std::array<Color32, MatrixSize>& framebuffer, const std::array<float, Columns>& bands, const VisualizerSettings& settings, const std::optional<Color32> visualizerColor, const float mediaColorAmount, const float wavePhase)
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

    static bool sendFramebuffer(RawUSB& usb, const std::array<Color32, MatrixSize>& framebuffer)
    {
        FramebufferSetPayload<MatrixSize> payload{};
        payload.Framebuffer = framebuffer;
        return usb.send(makePacket(PacketType::FramebufferSet, payload));
    }

    static void applyGlobalBrightness(std::array<Color32, MatrixSize>& framebuffer, const float brightness) noexcept
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

    struct PreviewRect
    {
        std::uint8_t Row;
        std::uint8_t Column;
        float X;
        float Y;
        float Width;
        float Height;
    };

    static Color32 interpolatePreviewColor(const std::array<Color32, MatrixSize>& framebuffer, const std::uint8_t row, const std::uint8_t column, const float interpolation) noexcept
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

    static void drawFramebufferPreview(const std::array<Color32, MatrixSize>& framebuffer, const float widthFraction = 0.55f, const float maxWidth = 532.0f, const float interpolation = 0.0f)
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

    static void drawShaderLivePanel(RawUSB& usb, SharedDeviceState& deviceState, const EvdevKeyboard& keyboardInput, const std::array<Color32, MatrixSize>& framebuffer, const float appCpuUsage, const bool scrollLockActive, const bool capsLockActive, VisualizerSettings& settings)
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

    static void drawPerformance(const PerformanceSnapshot& stats)
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

    static void drawTimingProbe(const MatrixTimingProbeResult<ActiveProbeRows>& probe)
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


    static const char* packetTypeLabel(const std::uint16_t type)
    {
        if (type == static_cast<std::uint16_t>(PacketType::FramebufferSet)) return "FramebufferSet";
        if (type == static_cast<std::uint16_t>(PacketType::PerformanceRequest)) return "PerformanceRequest";
        if (type == static_cast<std::uint16_t>(PacketType::PerformanceResponse)) return "PerformanceResponse";
        if (type == static_cast<std::uint16_t>(PacketType::MatrixTimingProbeResult)) return "MatrixTimingProbeResult";
        return "Other";
    }

    static void selectRuntimeProcess(RuntimeBinding& binding, const RuntimeProcessInfo& process)
    {
        binding.ProcessId = static_cast<int>(process.Pid);
        std::snprintf(binding.ProcessName, sizeof(binding.ProcessName), "%s", process.Name.c_str());
        binding.NextProcessSearch = 0.0;
        captureRuntimeRebindPattern(binding, process);
    }

    static bool drawRuntimeTargetSelector(RuntimeBinding& binding, ShaderFramebuffer& shaderFramebuffer)
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

    static bool drawRuntimeProcessSelector(RuntimeBinding& binding, std::vector<RuntimeProcessInfo>& processes)
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

    static const char* runtimeParameterSlotName(const RuntimeParameterSlot slot) noexcept
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

    static bool drawRuntimeParameterLink(RuntimeBindingEngine& engine, RuntimeBinding& owner, const RuntimeParameterSlot slot)
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

    static bool drawRuntimeLinkedFloat(RuntimeBindingEngine& engine, RuntimeBinding& binding, const RuntimeParameterSlot slot, const char* label, float& value, const float speed = 0.01f, const char* format = "%.3f")
    {
        bool changed = false;
        ImGui::SetNextItemWidth(180.0f);
        changed |= ImGui::DragFloat(label, &value, speed, 0.0f, 0.0f, format);
        ImGui::SameLine();
        changed |= drawRuntimeParameterLink(engine, binding, slot);
        return changed;
    }

    static bool drawRuntimeLinkedBool(RuntimeBindingEngine& engine, RuntimeBinding& binding, const RuntimeParameterSlot slot, const char* label, bool& value)
    {
        bool changed = ImGui::Checkbox(label, &value);
        ImGui::SameLine();
        changed |= drawRuntimeParameterLink(engine, binding, slot);
        return changed;
    }

    static bool drawRuntimeBindingReferenceCombo(RuntimeBindingEngine& engine, const char* label, std::uint64_t& id, const std::uint64_t excludeId = 0, const bool unboundOnly = false)
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

    static bool drawRuntimeControlReferenceCombo(RuntimeBindingEngine& engine, const char* label, std::uint64_t& id)
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

    static bool drawRuntimeBankReferenceCombo(RuntimeBindingEngine& engine, const char* label, std::uint64_t& id)
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

    static ImVec4 runtimeStateColor(const bool enabled, const bool good, const bool error = false, const bool waiting = false)
    {
        if (!enabled) return ImVec4(0.38f, 0.38f, 0.38f, 1.0f);
        if (error) return ImVec4(0.92f, 0.18f, 0.18f, 1.0f);
        if (waiting) return ImVec4(0.95f, 0.65f, 0.14f, 1.0f);
        return good ? ImVec4(0.15f, 0.78f, 0.30f, 1.0f) : ImVec4(0.82f, 0.20f, 0.20f, 1.0f);
    }

    static bool runtimeBindingLooksBoolean(const RuntimeBinding& binding) noexcept
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
