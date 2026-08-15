#pragma once
#include "Functions.hpp"
#include "quartz/client/shader/ShaderPreset.hpp"
#include "quartz/client/device/DeviceState.hpp"
#include "quartz/client/input/Input.hpp"
#include "quartz/client/platform/AppCpuMeter.hpp"
#include "quartz/client/settings/VisualizerSettings.hpp"
#include "quartz/client/shader/ShaderEditor.hpp"
#include "quartz/client/shader/ShaderMaterial.hpp"
#include "quartz/client/shader/ShaderFramebuffer.hpp"
#include "quartz/client/shader/ShaderTransition.hpp"
#include "quartz/client/usb/USB.hpp"
#include "quartz/client/audio/Audio.hpp"
#include "quartz/client/media/Media.hpp"
#include "quartz/client/native/NativeTypes.hpp"
#include "quartz/client/runtime/Profile.hpp"
#include "quartz/client/runtime/RuntimeTypes.hpp"
#include "quartz/client/runtime/RuntimeBindingEngine.hpp"
#include "quartz/client/runtime/RuntimeTelemetry.hpp"
#include "quartz/client/ui/UIState.hpp"

namespace quartz::client
{
    static_assert(sizeof(Color32) == 3);
    static_assert(sizeof(FramebufferSetPayload<MatrixSize>) == 340);
    static_assert(sizeof(PerformancePayload) == 36);
    static_assert(sizeof(MatrixTimingProbeResult<ActiveProbeRows>) == 52);

    inline constexpr auto ReactiveKeyBindings = std::to_array<ReactiveKeyBinding>({
        {KEY_ESC, 0, 0}, {KEY_F1, 0, 1}, {KEY_F2, 0, 2}, {KEY_F3, 0, 3}, {KEY_F4, 0, 4}, {KEY_F5, 0, 5}, {KEY_F6, 0, 6}, {KEY_F7, 0, 7},
        {KEY_F8, 0, 8}, {KEY_F9, 0, 9}, {KEY_F10, 0, 10}, {KEY_F11, 0, 11}, {KEY_F12, 0, 12}, {KEY_SYSRQ, 0, 13}, {KEY_SCROLLLOCK, 0, 14}, {KEY_PAUSE, 0, 15},
        {KEY_GRAVE, 1, 0}, {KEY_1, 1, 1}, {KEY_2, 1, 2}, {KEY_3, 1, 3}, {KEY_4, 1, 4}, {KEY_5, 1, 5}, {KEY_6, 1, 6}, {KEY_7, 1, 7},
        {KEY_8, 1, 8}, {KEY_9, 1, 9}, {KEY_0, 1, 10}, {KEY_MINUS, 1, 11}, {KEY_EQUAL, 1, 12}, {KEY_BACKSPACE, 1, 12}, {KEY_INSERT, 1, 13}, {KEY_HOME, 1, 14}, {KEY_PAGEUP, 1, 15},
        {KEY_TAB, 2, 0}, {KEY_Q, 2, 1}, {KEY_W, 2, 2}, {KEY_E, 2, 3}, {KEY_R, 2, 4}, {KEY_T, 2, 5}, {KEY_Y, 2, 6}, {KEY_U, 2, 7},
        {KEY_I, 2, 8}, {KEY_O, 2, 9}, {KEY_P, 2, 10}, {KEY_LEFTBRACE, 2, 11}, {KEY_RIGHTBRACE, 2, 12}, {KEY_DELETE, 2, 13}, {KEY_END, 2, 14}, {KEY_PAGEDOWN, 2, 15},
        {KEY_CAPSLOCK, 3, 0}, {KEY_A, 3, 1}, {KEY_S, 3, 2}, {KEY_D, 3, 3}, {KEY_F, 3, 4}, {KEY_G, 3, 5}, {KEY_H, 3, 6}, {KEY_J, 3, 7},
        {KEY_K, 3, 8}, {KEY_L, 3, 9}, {KEY_SEMICOLON, 3, 10}, {KEY_APOSTROPHE, 3, 11}, {KEY_BACKSLASH, 3, 12}, {KEY_ENTER, 3, 15},
        {KEY_LEFTSHIFT, 4, 0}, {KEY_102ND, 4, 1}, {KEY_Z, 4, 2}, {KEY_X, 4, 3}, {KEY_C, 4, 4}, {KEY_V, 4, 5}, {KEY_B, 4, 6}, {KEY_N, 4, 7}, {KEY_M, 4, 8},
        {KEY_COMMA, 4, 9}, {KEY_DOT, 4, 10}, {KEY_SLASH, 4, 11}, {KEY_RO, 4, 12}, {KEY_UP, 4, 14}, {KEY_RIGHTSHIFT, 4, 15},
        {KEY_LEFTCTRL, 5, 0}, {KEY_LEFTMETA, 5, 1}, {KEY_LEFTALT, 5, 2}, {KEY_SPACE, 5, 6}, {KEY_RIGHTALT, 5, 9}, {KEY_MENU, 5, 11}, {KEY_RIGHTCTRL, 5, 12},
        {KEY_LEFT, 5, 13}, {KEY_DOWN, 5, 14}, {KEY_RIGHT, 5, 15}
    });

    inline constexpr float ShaderEditorBaseSize = 14.0f;
    inline constexpr std::array<float, 20> ShaderEditorZoomLevels{0.60f, 0.70f, 0.80f, 0.90f, 1.00f, 1.10f, 1.20f, 1.30f, 1.40f, 1.50f, 1.60f, 1.70f, 1.80f, 1.90f, 2.00f, 2.10f, 2.20f, 2.30f, 2.40f, 2.50f};

    template<typename T>
    bool parseNumber(const std::string_view value, T& result)
    {
        if (value.empty()) return false;
        if constexpr (std::is_same_v<T, bool>)
        {
            if (value == "1" || value == "true" || value == "True" || value == "TRUE") { result = true; return true; }
            if (value == "0" || value == "false" || value == "False" || value == "FALSE") { result = false; return true; }
            return false;
        }
        else if constexpr (std::is_integral_v<T>)
        {
            std::string_view text = value; bool negative = false;
            if (text.front() == '+' || text.front() == '-') { negative = text.front() == '-'; text.remove_prefix(1); }
            if (text.empty() || (negative && !std::is_signed_v<T>)) return false;
            int base = 10; if (text.starts_with("0x") || text.starts_with("0X")) { base = 16; text.remove_prefix(2); }
            if (text.empty()) return false;
            using U = std::make_unsigned_t<T>; U magnitude{}; const auto parsedResult = std::from_chars(text.data(), text.data() + text.size(), magnitude, base);
            if (parsedResult.ec != std::errc{} || parsedResult.ptr != text.data() + text.size()) return false;
            if constexpr (std::is_signed_v<T>)
            {
                const U maximum = static_cast<U>(std::numeric_limits<T>::max());
                if (negative)
                {
                    const U minimumMagnitude = maximum + U{1}; if (magnitude > minimumMagnitude) return false;
                    result = magnitude == minimumMagnitude ? std::numeric_limits<T>::min() : static_cast<T>(-static_cast<T>(magnitude));
                }
                else { if (magnitude > maximum) return false; result = static_cast<T>(magnitude); }
            }
            else result = static_cast<T>(magnitude);
            return true;
        }
        else
        {
            T parsed{}; const char* begin = value.data(); const char* end = value.data() + value.size(); const auto parsedResult = std::from_chars(begin, end, parsed, std::chars_format::general);
            if (parsedResult.ec != std::errc{} || parsedResult.ptr != end || !std::isfinite(parsed)) return false; result = parsed; return true;
        }
    }

    template<std::size_t N>
    bool parseFloatArray(const std::string_view value, std::array<float, N>& result)
    {
        std::array<float, N> parsed{};
        std::size_t start = 0;
        for (std::size_t i = 0; i < N; ++i)
        {
            const std::size_t end = value.find(',', start);
            const std::string_view part = trimSettingValue(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start));
            if (!parseNumber(part, parsed[i]))
                return false;
            if (i + 1 < N)
            {
                if (end == std::string_view::npos)
                    return false;
                start = end + 1;
            }
            else if (end != std::string_view::npos)
                return false;
        }
        result = parsed;
        return true;
    }

    template<typename T>
    PacketBuffer makePacket(const PacketType type, const T& payload)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        PacketBuffer packet{};
        packet.Size = sizeof(PacketHeader) + sizeof(T);
        auto* header = std::construct_at(reinterpret_cast<PacketHeader*>(packet.Data.data()), 1, type, PacketDirection::HostToDevice, 0, sizeof(T));
        std::memcpy(packet.Data.data() + sizeof(PacketHeader), &payload, sizeof(T));
        return packet;
    }

    template<typename T>
    bool readProcessMemoryValue(pid_t pid, std::uintptr_t address, T& value, std::string& error);

    template<typename T>
    bool readProcessMemoryValue(const pid_t pid, const std::uintptr_t address, T& value, std::string& error)
    {
        iovec local{&value, sizeof(T)};
        iovec remote{reinterpret_cast<void*>(address), sizeof(T)};
        errno = 0;
        const ssize_t count = ::process_vm_readv(pid, &local, 1, &remote, 1, 0);
        if (count == static_cast<ssize_t>(sizeof(T))) return true;
        error = count < 0 ? std::string(std::strerror(errno)) : "short read (" + std::to_string(count) + "/" + std::to_string(sizeof(T)) + ")";
        return false;
    }

    template<typename T>
    void runtimeSortUiNodes(std::vector<T*>& nodes)
    {
        std::ranges::sort(nodes, [](const T* a, const T* b) { if (a->Order != b->Order) return a->Order < b->Order; return a->Id < b->Id; });
    }

    template<typename T>
    bool defaultButton(const char* id, T& value, const T& defaultValue)
    {
        const bool isDefault = value == defaultValue;
        ImGui::SameLine();
        if (isDefault) ImGui::BeginDisabled();
        char label[96];
        std::snprintf(label, sizeof(label), "Default##%s", id);
        const bool pressed = ImGui::SmallButton(label);
        if (isDefault) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Restore default");
        if (pressed) value = defaultValue;
        return pressed;
    }

}
