#pragma once

namespace quartz::client
{
    struct VisualizerSettings;
}

namespace quartz::client::ui
{
    enum class Theme : int
    {
        QuartzCyan,
        Graphite,
        OceanBlue,
        Emerald,
        DevilukePink,
        KurosakiPink,
        YamiGolden,
        KirisakiPurple,
        Count
    };

    [[nodiscard]] const char* themeName(Theme theme) noexcept;
    [[nodiscard]] bool suspiciousTheme(Theme theme) noexcept;
    void loadThemePreferences(VisualizerSettings& settings) noexcept;
    void saveThemePreferences(const VisualizerSettings& settings) noexcept;
    void applyTheme(Theme theme) noexcept;
    void applyTheme(int theme) noexcept;
    bool drawThemeSelector(VisualizerSettings& settings);
}
