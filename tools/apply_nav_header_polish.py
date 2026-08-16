from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"{label}: anchor not found")
    return text.replace(old, new, 1)


path = Path("src/ui/PageManager.cpp")
text = path.read_text()
text = replace_once(text,
    '        if (ImGui::BeginChild("PageNavigation", ImVec2(NavigationWidth, available.y), ImGuiChildFlags_Borders))\n        {\n            ImGui::Dummy({0.0f, 3.0f});\n',
    '        if (ImGui::BeginChild("PageNavigation", ImVec2(NavigationWidth, available.y), ImGuiChildFlags_Borders))\n        {\n            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));\n            ImGui::Dummy({0.0f, 3.0f});\n',
    "navigation text alignment")
text = replace_once(text, 'ImVec2(selectableWidth, 27.0f)', 'ImVec2(selectableWidth, 25.0f)', "navigation row height")
text = replace_once(text,
    '                        ImGui::GetWindowDrawList()->AddRectFilled({min.x, min.y + 4.0f}, {min.x + 3.0f, max.y - 4.0f}, ImGui::GetColorU32(ImGuiCol_CheckMark), 2.0f);\n',
    '                        ImGui::GetWindowDrawList()->AddRectFilled({min.x, min.y + 3.0f}, {min.x + 3.0f, max.y - 3.0f}, ImGui::GetColorU32(ImGuiCol_CheckMark), 2.0f);\n',
    "navigation selection marker")
text = replace_once(text,
    '                ImGui::Spacing(); ImGui::Spacing();\n            }\n        }\n        ImGui::EndChild();\n',
    '                ImGui::Spacing(); ImGui::Spacing();\n            }\n            ImGui::PopStyleVar();\n        }\n        ImGui::EndChild();\n',
    "navigation style cleanup")
path.write_text(text)


path = Path("src/ui/UI.cpp")
text = path.read_text()
text = replace_once(text,
    '        const char* terminateLabel = ui::i18n::tr("header.terminate");\n        const float spacing = ImGui::GetStyle().ItemSpacing.x;\n        const float buttonsWidth = ImGui::CalcTextSize(keyboardLabel).x + ImGui::CalcTextSize(appearanceLabel).x + ImGui::CalcTextSize(terminateLabel).x + ImGui::GetStyle().FramePadding.x * 6.0f + spacing * 2.0f;\n        const float right = ImGui::GetWindowContentRegionMax().x;\n',
    '        const char* terminateLabel = ui::i18n::tr("header.terminate");\n        const std::string themeText = std::string(ui::i18n::tr("header.theme")) + ": " + ui::themeName(static_cast<ui::Theme>(std::clamp(settings.UiTheme, 0, static_cast<int>(ui::Theme::Count) - 1)));\n        const float spacing = ImGui::GetStyle().ItemSpacing.x;\n        const float themeWidth = ImGui::CalcTextSize(themeText.c_str()).x;\n        const float buttonsWidth = ImGui::CalcTextSize(keyboardLabel).x + ImGui::CalcTextSize(appearanceLabel).x + ImGui::CalcTextSize(terminateLabel).x + ImGui::GetStyle().FramePadding.x * 6.0f + spacing * 2.0f;\n        const float right = ImGui::GetWindowContentRegionMax().x;\n',
    "header theme metrics")
text = replace_once(text,
    '        if (ImGui::GetCursorPosX() + bylineWidth + buttonsWidth + 48.0f < right)\n',
    '        if (ImGui::GetCursorPosX() + bylineWidth + themeWidth + buttonsWidth + 72.0f < right)\n',
    "header byline fit")
text = replace_once(text,
    '        const float buttonStart = right - buttonsWidth;\n        if (ImGui::GetCursorPosX() < buttonStart) ImGui::SameLine(buttonStart);\n',
    '        const float buttonStart = right - buttonsWidth;\n        const float themeStart = buttonStart - themeWidth - spacing * 2.0f;\n        if (ImGui::GetCursorPosX() + themeWidth + spacing < buttonStart)\n        {\n            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), themeStart));\n            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", themeText.c_str());\n        }\n        if (ImGui::GetCursorPosX() < buttonStart) ImGui::SameLine(buttonStart);\n',
    "header theme label")
path.write_text(text)


path = Path("src/ui/I18n.cpp")
text = path.read_text()
text = replace_once(text,
    '            {"header.appearance", "Appearance", "Aparência"},\n',
    '            {"header.appearance", "Appearance", "Aparência"},\n            {"header.theme", "Theme", "Tema"},\n',
    "header theme translation")
path.write_text(text)

print("Navigation and header polish applied")
