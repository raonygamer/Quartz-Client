from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"{label}: anchor not found")
    return text.replace(old, new, 1)


path = Path("src/ui/I18n.cpp")
text = path.read_text()
text = replace_once(text,
    '            {"appearance.theme", "Theme", "Tema"},\n',
    '            {"appearance.theme", "Theme", "Tema"},\n            {"theme.quartzCyan", "Quartz Cyan", "Quartz Ciano"},\n            {"theme.graphite", "Graphite", "Grafite"},\n            {"theme.oceanBlue", "Ocean Blue", "Azul Oceano"},\n            {"theme.emerald", "Emerald", "Esmeralda"},\n            {"theme.devilukePink", "Deviluke Pink", "Deviluke Rosa"},\n            {"theme.kurosakiPink", "Kurosaki Pink", "Kurosaki Rosa"},\n            {"theme.yamiGolden", "Yami Golden", "Yami Dourado"},\n            {"theme.kirisakiPurple", "Kirisaki Purple", "Kirisaki Roxo"},\n',
    "theme translations")
text = replace_once(text,
    '            {"appearance.suspiciousTooltip", "Unlocks Deviluke Pink, Kurosaki Pink, Yami Golden and Kirisaki Purple.", "Libera Deviluke Pink, Kurosaki Pink, Yami Golden e Kirisaki Purple."},\n',
    '            {"appearance.suspiciousTooltip", "Unlocks Deviluke Pink, Kurosaki Pink, Yami Golden and Kirisaki Purple.", "Libera Deviluke Rosa, Kurosaki Rosa, Yami Dourado e Kirisaki Roxo."},\n',
    "suspicious tooltip localization")
path.write_text(text)


path = Path("src/ui/Theme.cpp")
text = path.read_text()
old = '''    const char* themeName(const Theme theme) noexcept
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
'''
new = '''    const char* themeName(const Theme theme) noexcept
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
'''
text = replace_once(text, old, new, "localized theme names")
path.write_text(text)


path = Path("src/ui/UI.cpp")
text = path.read_text()
old = '''        if (showKeyboardPreview)
        {
            std::string title = std::string(ui::i18n::tr("keyboardPreview.title")) + "###QuartzKeyboardPreview";
            ImGui::SetNextWindowSize(ImVec2(560.0f, 270.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin(title.c_str(), &showKeyboardPreview, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
                drawFramebufferPreview(framebuffer, 1.0f, 520.0f, settings.LiveOutputInterpolation);
            ImGui::End();
        }
'''
new = '''        if (showKeyboardPreview)
        {
            std::string title = std::string(ui::i18n::tr("keyboardPreview.title")) + "###QuartzKeyboardPreview";
            ImGui::SetNextWindowSize(ImVec2(560.0f, 270.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f, 150.0f), ImVec2(FLT_MAX, FLT_MAX));
            if (ImGui::Begin(title.c_str(), &showKeyboardPreview, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            {
                constexpr float PreviewAspect = 19.0f / 6.35f;
                const ImVec2 available = ImGui::GetContentRegionAvail();
                const float widthFromHeight = available.y > 1.0f ? available.y * PreviewAspect : available.x;
                const float previewWidth = std::max(140.0f, std::min(available.x, widthFromHeight));
                const float offset = std::max(0.0f, (available.x - previewWidth) * 0.5f);
                if (offset > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
                drawFramebufferPreview(framebuffer, 1.0f, previewWidth, settings.LiveOutputInterpolation);
            }
            ImGui::End();
        }
'''
text = replace_once(text, old, new, "responsive keyboard preview")
path.write_text(text)


path = Path("src/ui/RuntimeUI.cpp")
text = path.read_text()
text = replace_once(text,
    '        const float unit = std::clamp((previewWidth - 2.0f) / LayoutWidth, 7.0f, 28.0f);\n',
    '        const float unit = std::clamp((previewWidth - 2.0f) / LayoutWidth, 7.0f, 64.0f);\n',
    "preview scaling ceiling")
path.write_text(text)

print("Theme and preview follow-up applied")
