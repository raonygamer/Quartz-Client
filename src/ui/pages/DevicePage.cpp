#include "quartz/client/ui/pages/DevicePage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void DevicePage::render(PageContext& context, PageManager& manager)
    {
        auto& usb = context.usb;
        const auto& keyboardInput = context.keyboardInput;
        auto& shaderFramebuffer = context.shaderFramebuffer;
        auto& settings = context.settings;
        const auto& framebuffer = context.framebuffer;
        auto& runtimeBindings = context.runtimeBindings;
        const auto sentFrames = context.sentFrames;
        const auto droppedFrames = context.droppedFrames;
        const auto appCpuUsage = context.appCpuUsage;
        const auto scrollLockActive = context.scrollLockActive;
        const auto capsLockActive = context.capsLockActive;
        const bool connected = usb.isConnected();
        auto& deviceState = context.deviceState;
        PerformanceSnapshot performance{};
        MatrixTimingProbeResult<ActiveProbeRows> timingProbe{};
        bool hasPerformance = false;
        bool hasTimingProbe = false;
        std::uint64_t receivedPackets = 0;
        {
            std::lock_guard lock(deviceState.Mutex);
            performance = deviceState.Performance;
            timingProbe = deviceState.TimingProbe;
            hasPerformance = deviceState.HasPerformance;
            hasTimingProbe = deviceState.HasTimingProbe;
            receivedPackets = deviceState.ReceivedPackets;
        }

        (void)manager;
        const std::size_t framebufferPayloadBytes = sizeof(FramebufferSetPayload<MatrixSize>);
        const std::size_t framebufferPacketBytes = sizeof(PacketHeader) + framebufferPayloadBytes;
        const double configuredTxKiB = settings.Enabled && settings.SendFramebuffer ? framebufferPacketBytes * static_cast<double>(settings.FrameRate) / 1024.0 : 0.0;
        const char* outputMode = settings.BaseColorMode == 0 ? "RGB wave" : settings.BaseColorMode == 1 ? "Solid" : "Shader framebuffer";
        const char* shaderName = settings.ShaderPresetIndex == 0 ? "Custom / current" : ShaderPresets[static_cast<std::size_t>(settings.ShaderPresetIndex - 1)].Name.c_str();

        ImGui::SeparatorText("Identity");
        ImGui::Text("Product: %s", connected ? (usb.deviceName().empty() ? "Quartz K552X" : usb.deviceName().c_str()) : "Disconnected");
        ImGui::Text("Firmware: %s", FirmwareVersion);
        ImGui::Text("VID:PID: %04X:%04X", VendorId, ProductId);
        ImGui::Text("QRPC protocol: v%u   interface %d   OUT 0x%02X   IN 0x%02X", static_cast<unsigned>(ProtocolVersion), RPCInterfaceNumber, static_cast<unsigned>(RPCOutEndpoint), static_cast<unsigned>(RPCInEndpoint));
        ImGui::Text("Packet header: %zu B   framebuffer payload: %zu B   full frame packet: %zu B", sizeof(PacketHeader), framebufferPayloadBytes, framebufferPacketBytes);
        ImGui::Text("Logical framebuffer: %zux%zu = %zu cells   active RGB area: %zux%zu", Columns, Rows, MatrixSize, Columns, ActiveProbeRows);

        ImGui::SeparatorText("Session / output");
        ImGui::Text("Client uptime: %.1f s   App CPU: %.2f%%", ImGui::GetTime(), appCpuUsage);
        ImGui::Text("TX frames: %llu   dropped/busy: %llu   RX packets: %llu", static_cast<unsigned long long>(sentFrames), static_cast<unsigned long long>(droppedFrames), static_cast<unsigned long long>(receivedPackets));
        ImGui::Text("Output mode: %s   target: %d Hz   estimated framebuffer TX: %.1f KiB/s", outputMode, settings.FrameRate, configuredTxKiB);
        if (settings.BaseColorMode == 2) ImGui::Text("Shader: %s   render target: %dx%d   downsample mode: %d   material params: %zu", shaderName, settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight, settings.ShaderDownsampleMode, shaderFramebuffer.materialParameters().size());
        ImGui::Text("Brightness: %.0f%%   live-preview interpolation: %.0f%%", settings.GlobalBrightness * 100.0f, settings.LiveOutputInterpolation * 100.0f);

        ImGui::SeparatorText("Host input / window");
        ImGui::Text("evdev: %s", keyboardInput.connected() ? "connected" : "disconnected");
        if (!keyboardInput.deviceName().empty()) ImGui::Text("Input device: %s", keyboardInput.deviceName().c_str());
        ImGui::TextWrapped("%s", keyboardInput.status().c_str());
        ImGui::Text("Caps Lock: %s   Scroll Lock: %s", capsLockActive ? "on" : "off", scrollLockActive ? "on" : "off");
        ImGui::TextDisabled("Closing the GLFW window hides it. Ctrl + Alt + Shift + Q restores it globally. Use Terminate in the permanent header to actually exit.");

        ImGui::SeparatorText("Runtime files");
        const std::string configFile = settingsPath().string();
        const std::string vertexFile = vertexShaderPath().string();
        const std::string fragmentFile = fragmentShaderPath().string();
        const std::string materialFile = shaderMaterialPath().string();
        const std::string bindingsFile = runtimeBindings.path().string();
        ImGui::TextWrapped("Settings: %s", configFile.c_str());
        ImGui::TextWrapped("Vertex shader: %s", vertexFile.c_str());
        ImGui::TextWrapped("Fragment shader: %s", fragmentFile.c_str());
        ImGui::TextWrapped("Material parameters: %s", materialFile.c_str());
        ImGui::TextWrapped("Runtime bindings: %s", bindingsFile.c_str());
    }
}
