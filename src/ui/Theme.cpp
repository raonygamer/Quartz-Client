#include "quartz/client/ui/Theme.hpp"
#include "quartz/client/settings/VisualizerSettings.hpp"
#include <algorithm>
#include <array>
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
            {{1.00f, 0.34f, 0.62f, 1.0f}, {1.00f, 0.48f, 0.72f, 1.0f}, {0.86f, 0.22f, 0.49f, 1.0f}, {1.00f, 0.68f, 0.83f, 1.0f}, {0.072f, 0.040f, 0.056f, 1.0f}, {0.100f, 0.052f, 0.075f, 1.0f}},
            {{0.91f, 0.25f, 0.51f, 1.0f}, {1.00f, 0.38f, 0.62f, 1.0f}, {0.73f, 0.15f, 0.39f, 1.0f}, {0.98f, 0.60f, 0.74f, 1.0f}, {0.061f, 0.035f, 0.047f, 1.0f}, {0.087f, 0.045f, 0.062f, 1.0f}},
            {{0.93f, 0.70f, 0.20f, 1.0f}, {1.00f, 0.82f, 0.34f, 1.0f}, {0.77f, 0.55f, 0.10f, 1.0f}, {1.00f, 0.90f, 0.55f, 1.0f}, {0.058f, 0.050f, 0.031f, 1.0f}, {0.082f, 0.069f, 0.040f, 1.0f}},
            {{0.64f, 0.35f, 0.91f, 1.0f}, {0.76f, 0.48f, 1.00f, 1.0f}, {0.50f, 0.24f, 0.76f, 1.0f}, {0.84f, 0.68f, 1.00f, 1.0f}, {0.051f, 0.039f, 0.066f, 1.0f}, {0.071f, 0.051f, 0.096f, 1.0f}}
        }};

        ImVec4 alpha(ImVec4 color, const float value) noexcept { color.w = value; return color; }
    }

    const char* themeName(const Theme theme) noexcept
    {
        switch (theme)
        {
        case Theme::QuartzCyan: return "Quartz Cyan";
        case Theme::Graphite: return "Graphite";
        case Theme::OceanBlue: return "Ocean Blue";
        case Theme::Emerald: return "Emerald";
        case Theme::DevilukePink: return "Deviluke Pink";
        case Theme::KurosakiPink: return "Kurosaki Pink";
        case Theme::YamiGolden: return "Yami Golden";
        case Theme::KirisakiPurple: return "Kirisaki Purple";
        case Theme::Count: break;
        }
        return "Quartz Cyan";
    }

    bool suspiciousTheme(const Theme theme) noexcept { return theme >= Theme::DevilukePink && theme < Theme::Count; }

    void applyTheme(const Theme theme) noexcept
    {
        const int index = std::clamp(static_cast<int>(theme), 0, static_cast<int>(Theme::Count) - 1);
        const Palette& p = Palettes[static_cast<std::size_t>(index)];
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowPadding = {12.0f, 10.0f};
        style.FramePadding = {8.0f, 5.0f};
        style.ItemSpacing = {8.0f, 7.0f};
        style.ItemInnerSpacing = {6.0f, 5.0f};
        style.ScrollbarSize = 13.0f;
        style.GrabMinSize = 10.0f;
        style.WindowRounding = 7.0f;
        style.ChildRounding = 6.0f;
        style.FrameRounding = 5.0f;
        style.PopupRounding = 6.0f;
        style.ScrollbarRounding = 8.0f;
        style.GrabRounding = 4.0f;
        style.TabRounding = 5.0f;
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
        c[ImGuiCol_SliderGrabActive] = p.Hover;
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
        if (ImGui::BeginCombo("Theme", themeName(current)))
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

        bool suspicious = settings.SuspiciousColorThemes;
        if (ImGui::Checkbox("Enable suspicious color themes", &suspicious))
        {
            if (suspicious && !settings.SuspiciousColorThemes) ImGui::OpenPopup("Suspicious color themes");
            else if (!suspicious)
            {
                settings.SuspiciousColorThemes = false;
                if (suspiciousTheme(static_cast<Theme>(settings.UiTheme))) settings.UiTheme = static_cast<int>(Theme::QuartzCyan);
                changed = true;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unlocks Deviluke Pink, Kurosaki Pink, Yami Golden and Kirisaki Purple.");

        if (ImGui::BeginPopupModal("Suspicious color themes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped("Are you sure you want to enable those colors? YOU WILL NEVER BE ABLE TO GO BACK LOL... not really tho");
            ImGui::Spacing();
            if (ImGui::Button("Enable suspicious color themes")) { settings.SuspiciousColorThemes = true; changed = true; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("I choose peace")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (changed) applyTheme(settings.UiTheme);
        return changed;
    }
}
