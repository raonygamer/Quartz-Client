#pragma once
#include "Forward.hpp"

namespace quartz::client
{
    extern const std::string_view DefaultVertexShaderSource;
    extern const std::string_view LegacyDefaultFragmentShaderSource;
    extern const std::string_view DefaultFragmentShaderSource;
    extern const std::string_view PlasmaFragmentShaderSource;
    extern const std::string_view NeonRingsFragmentShaderSource;
    extern const std::string_view RotatingBoxesFragmentShaderSource;
    extern const std::string_view CheckerWarpFragmentShaderSource;
    extern const std::string_view DiamondTunnelFragmentShaderSource;
    extern const std::string_view WaveInterferenceFragmentShaderSource;
    extern const std::string_view RadialPulseFragmentShaderSource;
    extern const std::string_view SpectrumFireFragmentShaderSource;
    extern const std::string_view KaleidoscopeFragmentShaderSource;
    extern const std::string_view GeometricGridFragmentShaderSource;
    extern const std::string_view RotatingCubeFragmentShaderSource;
    extern const std::string_view ReactiveKeyGlowFragmentShaderSource;
    extern const std::string_view ReactiveRippleFragmentShaderSource;
    extern const std::string_view ReactiveRainbowRippleFragmentShaderSource;
    extern const std::string_view ReactiveHeatFragmentShaderSource;
    extern const std::string_view ReactiveCrossBlastFragmentShaderSource;
    extern const std::string_view ReactiveSparksFragmentShaderSource;
    extern const std::string_view ReactiveManhattanFragmentShaderSource;
    extern const std::string_view ReactiveVortexFragmentShaderSource;
    extern const std::string_view ReactiveStarburstFragmentShaderSource;
    extern const std::string_view ReactiveScannerFragmentShaderSource;
    extern std::vector<ShaderPreset> ShaderPresets;
    extern std::string g_SettingsStatus;
    extern std::unordered_map<std::string, std::string> g_ShaderMaterialValues;
#if IMGUI_VERSION_NUM >= 19200
    extern ImFont* ShaderEditorFont;
#else
    extern std::array<ImFont*, 20> ShaderEditorFonts;
#endif
}
