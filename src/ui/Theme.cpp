#include "quartz/client/ui/Theme.hpp"
#include "quartz/client/ui/I18n.hpp"
#include "quartz/client/settings/VisualizerSettings.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <imgui.h>

namespace quartz::client::ui
{
    namespace
    {
        struct Palette
        {
            ImVec4 Accent;
            ImVec4 Hover;
            ImVec4 Active;
            ImVec4 Secondary;
            ImVec4 Window;
            ImVec4 Panel;
        };

        constexpr std::array<Palette, static_cast<std::size_t>(Theme::Count)> Palettes{{
            {{0.10f, 0.78f, 0.86f, 1.0f}, {0.18f, 0.88f, 0.96f, 1.0f}, {0.07f, 0.62f, 0.72f, 1.0f}, {0.34f, 0.92f, 0.96f, 1.0f}, {0.045f, 0.050f, 0.055f, 1.0f}, {0.065f, 0.072f, 0.078f, 1.0f}},
            {{0.55f, 0.58f, 0.62f, 1.0f}, {0.68f, 0.71f, 0.75f, 1.0f}, {0.42f, 0.45f, 0.49f, 1.0f}, {0.78f, 0.80f, 0.83f, 1.0f}, {0.043f, 0.043f, 0.046f, 1.0f}, {0.064f, 0.064f, 0.069f, 1.0f}},
            {{0.24f, 0.52f, 0.94f, 1.0f}, {0.34f, 0.64f, 1.00f, 1.0f}, {0.17f, 0.40f, 0.79f, 1.0f}, {0.45f, 0.72f, 1.00f, 1.0f}, {0.038f, 0.047f, 0.064f, 1.0f}, {0.054f, 0.067f, 0.090f, 1.0f}},
            {{0.22f, 0.72f, 0.48f, 1.0f}, {0.30f, 0.84f, 0.57f, 1.0f}, {0.15f, 0.57f, 0.37f, 1.0f}, {0.52f, 0.90f, 0.68f, 1.0f}, {0.038f, 0.055f, 0.047f, 1.0f}, {0.052f, 0.076f, 0.064f, 1.0f}},
            // Canonical suspicious accents: #cf6090, #852140, #eda83b, #684d7f.
            {{0.811765f, 0.376471f, 0.564706f, 1.0f}, {0.91f, 0.50f, 0.67f, 1.0f}, {0.68f, 0.26f, 0.45f, 1.0f}, {1.00f, 0.72f, 0.83f, 1.0f}, {0.071f, 0.040f, 0.055f, 1.0f}, {0.098f, 0.052f, 0.073f, 1.0f}},
            {{0.521569f, 0.129412f, 0.250980f, 1.0f}, {0.70f, 0.23f, 0.39f, 1.0f}, {0.43f, 0.07f, 0.19f, 1.0f}, {0.94f, 0.29f, 0.48f, 1.0f}, {0.057f, 0.031f, 0.040f, 1.0f}, {0.081f, 0.039f, 0.052f, 1.0f}},
            {{0.929412f, 0.658824f, 0.231373f, 1.0f}, {1.00f, 0.78f, 0.35f, 1.0f}, {0.76f, 0.51f, 0.13f, 1.0f}, {1.00f, 0.90f, 0.62f, 1.0f}, {0.057f, 0.049f, 0.030f, 1.0f}, {0.081f, 0.068f, 0.039f, 1.0f}},
            {{0.407843f, 0.301961f, 0.498039f, 1.0f}, {0.55f, 0.42f, 0.66f, 1.0f}, {0.31f, 0.21f, 0.40f, 1.0f}, {0.72f, 0.62f, 0.82f, 1.0f}, {0.048f, 0.038f, 0.058f, 1.0f}, {0.068f, 0.050f, 0.084f, 1.0f}}
        }};

        ImVec4 alpha(ImVec4 color, const float value) noexcept { color.w = value; return color; }
        std::filesystem::path themePath() { return settingsPath().parent_path() / "ui.theme.ini"; }
    }

    const char* themeName(const Theme theme) noexcept
    {
        switch (theme)
        {
        case Theme::QuartzCyan: return i18n::tr("theme.quartzCyan");
        case Theme::Graphite: return i18n::tr("theme.graphite");
        case Theme::OceanBlue: return i18n::tr("theme.oceanBlue");
        case Theme::Emerald: return i18n::tr("theme.emerald");
        case Theme::DevilukePink: return i18n::tr("theme.devilukePink");
        case Theme::KurosakiPink: return i18n::tr("theme.kurosakiPink");
        case Theme::YamiGolden: return i18n::tr("theme.yamiGolden");
        case Theme::KirisakiPurple: return i18n::tr("theme.kirisakiPurple");
        case Theme::Count: break;
        }
        return i18n::tr("theme.quartzCyan");
    }

    bool suspiciousTheme(const Theme theme) noexcept { return theme >= Theme::DevilukePink && theme < Theme::Count; }

    void loadThemePreferences(VisualizerSettings& settings) noexcept
    {
        try
        {
            std::ifstream file(themePath());
            if (file)
            {
                std::string line;
                while (std::getline(file, line))
                {
                    const std::size_t separator = line.find('=');
                    if (separator == std::string::npos) continue;
                    const std::string_view key(line.data(), separator);
                    const std::string_view value(line.data() + separator + 1, line.size() - separator - 1);
                    if (key == "Theme") parseNumber(value, settings.UiTheme);
                    else if (key == "Suspicious") parseBool(value, settings.SuspiciousColorThemes);
                    else if (key == "Rounding") parseNumber(value, settings.UiCornerRounding);
                    else if (key == "SilhouetteOpacity") parseNumber(value, settings.UiSilhouetteOpacity);
                }
            }
            settings.UiTheme = std::clamp(settings.UiTheme, 0, static_cast<int>(Theme::Count) - 1);
            settings.UiCornerRounding = std::clamp(settings.UiCornerRounding, 0.0f, 12.0f);
            settings.UiSilhouetteOpacity = std::clamp(settings.UiSilhouetteOpacity, 0.0f, 2.0f);
            if (suspiciousTheme(static_cast<Theme>(settings.UiTheme)) && !settings.SuspiciousColorThemes) settings.UiTheme = static_cast<int>(Theme::QuartzCyan);
            applyCornerRounding(settings.UiCornerRounding);
        }
        catch (...) { applyCornerRounding(settings.UiCornerRounding); }
    }

    void saveThemePreferences(const VisualizerSettings& settings) noexcept
    {
        try
        {
            std::error_code ec; std::filesystem::create_directories(themePath().parent_path(), ec);
            std::ofstream file(themePath(), std::ios::trunc);
            if (!file) return;
            file << "Theme=" << settings.UiTheme << '\n';
            file << "Suspicious=" << (settings.SuspiciousColorThemes ? "true" : "false") << '\n';
            file << "Rounding=" << settings.UiCornerRounding << '\n';
            file << "SilhouetteOpacity=" << settings.UiSilhouetteOpacity << '\n';
        }
        catch (...) {}
    }

    void applyCornerRounding(const float rounding) noexcept
    {
        const float r = std::clamp(rounding, 0.0f, 12.0f); ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = r * 1.4f; style.ChildRounding = r * 1.2f; style.FrameRounding = r; style.PopupRounding = r * 1.2f; style.ScrollbarRounding = r * 1.6f; style.GrabRounding = r * 0.8f; style.TabRounding = r;
    }

    void applyTheme(const Theme theme) noexcept
    {
        const int index = std::clamp(static_cast<int>(theme), 0, static_cast<int>(Theme::Count) - 1);
        const Palette& p = Palettes[static_cast<std::size_t>(index)];
        const ImGuiStyle previousStyle = ImGui::GetStyle();
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowPadding = {12.0f, 10.0f};
        style.FramePadding = {8.0f, 5.0f};
        style.ItemSpacing = {8.0f, 7.0f};
        style.ItemInnerSpacing = {6.0f, 5.0f};
        style.ScrollbarSize = 13.0f;
        style.GrabMinSize = 10.0f;
        style.WindowRounding = previousStyle.WindowRounding;
        style.ChildRounding = previousStyle.ChildRounding;
        style.FrameRounding = previousStyle.FrameRounding;
        style.PopupRounding = previousStyle.PopupRounding;
        style.ScrollbarRounding = previousStyle.ScrollbarRounding;
        style.GrabRounding = previousStyle.GrabRounding;
        style.TabRounding = previousStyle.TabRounding;
        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;

        auto& c = style.Colors;
        c[ImGuiCol_Text] = {0.93f, 0.94f, 0.96f, 1.0f};
        c[ImGuiCol_TextDisabled] = {0.49f, 0.52f, 0.57f, 1.0f};
        c[ImGuiCol_WindowBg] = p.Window;
        c[ImGuiCol_ChildBg] = p.Panel;
        c[ImGuiCol_PopupBg] = {p.Panel.x + 0.015f, p.Panel.y + 0.015f, p.Panel.z + 0.015f, 0.98f};
        c[ImGuiCol_Border] = alpha(p.Accent, 0.22f);
        c[ImGuiCol_BorderShadow] = {0.0f, 0.0f, 0.0f, 0.0f};
        c[ImGuiCol_FrameBg] = {p.Panel.x + 0.035f, p.Panel.y + 0.035f, p.Panel.z + 0.035f, 1.0f};
        c[ImGuiCol_FrameBgHovered] = alpha(p.Accent, 0.22f);
        c[ImGuiCol_FrameBgActive] = alpha(p.Accent, 0.30f);
        c[ImGuiCol_TitleBg] = p.Window;
        c[ImGuiCol_TitleBgActive] = alpha(p.Accent, 0.18f);
        c[ImGuiCol_MenuBarBg] = p.Panel;
        c[ImGuiCol_ScrollbarBg] = alpha(p.Window, 0.75f);
        c[ImGuiCol_ScrollbarGrab] = alpha(p.Accent, 0.34f);
        c[ImGuiCol_ScrollbarGrabHovered] = alpha(p.Hover, 0.52f);
        c[ImGuiCol_ScrollbarGrabActive] = alpha(p.Active, 0.72f);
        c[ImGuiCol_CheckMark] = p.Secondary;
        c[ImGuiCol_SliderGrab] = p.Accent;
        c[ImGuiCol_SliderGrabActive] = p.Secondary;
        c[ImGuiCol_Button] = alpha(p.Accent, 0.31f);
        c[ImGuiCol_ButtonHovered] = alpha(p.Hover, 0.58f);
        c[ImGuiCol_ButtonActive] = alpha(p.Active, 0.82f);
        c[ImGuiCol_Header] = alpha(p.Accent, 0.24f);
        c[ImGuiCol_HeaderHovered] = alpha(p.Hover, 0.42f);
        c[ImGuiCol_HeaderActive] = alpha(p.Active, 0.60f);
        c[ImGuiCol_Separator] = alpha(p.Accent, 0.24f);
        c[ImGuiCol_SeparatorHovered] = alpha(p.Hover, 0.58f);
        c[ImGuiCol_SeparatorActive] = p.Active;
        c[ImGuiCol_ResizeGrip] = alpha(p.Accent, 0.20f);
        c[ImGuiCol_ResizeGripHovered] = alpha(p.Hover, 0.55f);
        c[ImGuiCol_ResizeGripActive] = alpha(p.Active, 0.82f);
        c[ImGuiCol_Tab] = alpha(p.Accent, 0.18f);
        c[ImGuiCol_TabHovered] = alpha(p.Hover, 0.52f);
        c[ImGuiCol_TabSelected] = alpha(p.Accent, 0.46f);
        c[ImGuiCol_TabSelectedOverline] = p.Secondary;
        c[ImGuiCol_TabDimmed] = alpha(p.Accent, 0.10f);
        c[ImGuiCol_TabDimmedSelected] = alpha(p.Accent, 0.30f);
        c[ImGuiCol_TabDimmedSelectedOverline] = alpha(p.Secondary, 0.72f);
        c[ImGuiCol_PlotLines] = p.Secondary;
        c[ImGuiCol_PlotHistogram] = p.Accent;
        c[ImGuiCol_TableHeaderBg] = alpha(p.Accent, 0.18f);
        c[ImGuiCol_TableBorderStrong] = alpha(p.Accent, 0.25f);
        c[ImGuiCol_TableBorderLight] = alpha(p.Accent, 0.12f);
        c[ImGuiCol_TextSelectedBg] = alpha(p.Accent, 0.34f);
        c[ImGuiCol_NavCursor] = p.Secondary;
    }

    void applyTheme(const int theme) noexcept { applyTheme(static_cast<Theme>(std::clamp(theme, 0, static_cast<int>(Theme::Count) - 1))); }

    bool drawThemeSelector(VisualizerSettings& settings)
    {
        bool changed = false;
        settings.UiTheme = std::clamp(settings.UiTheme, 0, static_cast<int>(Theme::Count) - 1);
        Theme current = static_cast<Theme>(settings.UiTheme);
        if (suspiciousTheme(current) && !settings.SuspiciousColorThemes) { settings.UiTheme = 0; current = Theme::QuartzCyan; changed = true; }

        ImGui::SetNextItemWidth(190.0f);
        if (ImGui::BeginCombo(i18n::tr("appearance.theme"), themeName(current)))
        {
            for (int i = 0; i < static_cast<int>(Theme::Count); ++i)
            {
                const Theme candidate = static_cast<Theme>(i);
                if (suspiciousTheme(candidate) && !settings.SuspiciousColorThemes) continue;
                const bool selected = i == settings.UiTheme;
                if (ImGui::Selectable(themeName(candidate), selected)) { settings.UiTheme = i; current = candidate; changed = true; }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::SetNextItemWidth(190.0f);
        if (ImGui::SliderFloat(i18n::tr("appearance.cornerRounding"), &settings.UiCornerRounding, 0.0f, 12.0f, "%.1f px", ImGuiSliderFlags_AlwaysClamp)) { applyCornerRounding(settings.UiCornerRounding); saveThemePreferences(settings); }

        bool suspicious = settings.SuspiciousColorThemes;
        if (ImGui::Checkbox(i18n::tr("appearance.suspicious"), &suspicious))
        {
            if (suspicious && !settings.SuspiciousColorThemes) ImGui::OpenPopup(i18n::tr("appearance.suspiciousTitle"));
            else if (!suspicious)
            {
                settings.SuspiciousColorThemes = false;
                if (suspiciousTheme(static_cast<Theme>(settings.UiTheme))) settings.UiTheme = static_cast<int>(Theme::QuartzCyan);
                changed = true;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::tr("appearance.suspiciousTooltip"));

        if (ImGui::BeginPopupModal(i18n::tr("appearance.suspiciousTitle"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped("%s", i18n::tr("appearance.suspiciousWarning"));
            ImGui::Spacing();
            if (ImGui::Button(i18n::tr("appearance.suspiciousEnable"))) { settings.SuspiciousColorThemes = true; changed = true; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button(i18n::tr("appearance.suspiciousCancel"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (changed) { applyTheme(settings.UiTheme); applyCornerRounding(settings.UiCornerRounding); saveThemePreferences(settings); }
        return changed;
    }
}
