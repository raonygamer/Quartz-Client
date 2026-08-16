from pathlib import Path
import re

path = Path("src/ui/UI.cpp")
text = path.read_text()

include_anchor = '#include "quartz/client/ui/PageContext.hpp"\n'
include_block = '#include "quartz/client/ui/PageContext.hpp"\n#include "quartz/client/ui/I18n.hpp"\n#include "quartz/client/ui/Theme.hpp"\n'
if '#include "quartz/client/ui/I18n.hpp"' not in text:
    if include_anchor not in text:
        raise RuntimeError("UI include anchor not found")
    text = text.replace(include_anchor, include_block, 1)

new_header = r'''    void drawPermanentHeader(VisualizerSettings& settings, const std::array<Color32, MatrixSize>& framebuffer)
    {
        static bool showKeyboardPreview = false;
        static const VisualizerSettings defaults{};
        const char* keyboardLabel = ui::i18n::tr("header.keyboard");
        const char* appearanceLabel = ui::i18n::tr("header.appearance");
        const char* terminateLabel = ui::i18n::tr("header.terminate");
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float buttonsWidth = ImGui::CalcTextSize(keyboardLabel).x + ImGui::CalcTextSize(appearanceLabel).x + ImGui::CalcTextSize(terminateLabel).x + ImGui::GetStyle().FramePadding.x * 6.0f + spacing * 2.0f;
        const float right = ImGui::GetWindowContentRegionMax().x;

        ImGui::TextUnformatted("Quartz K552X");
        const float bylineWidth = ImGui::CalcTextSize(ui::i18n::tr("header.byline")).x;
        if (ImGui::GetCursorPosX() + bylineWidth + buttonsWidth + 48.0f < right)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("|  %s", ui::i18n::tr("header.byline"));
        }
        const float buttonStart = right - buttonsWidth;
        if (ImGui::GetCursorPosX() < buttonStart) ImGui::SameLine(buttonStart);
        if (ImGui::SmallButton(keyboardLabel)) showKeyboardPreview = !showKeyboardPreview;
        ImGui::SameLine();
        if (ImGui::SmallButton(appearanceLabel)) ImGui::OpenPopup("##QuartzAppearancePopup");
        ImGui::SameLine();
        if (ImGui::SmallButton(terminateLabel)) glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ui::i18n::tr("header.terminateTooltip"));

        if (ImGui::BeginPopup("##QuartzAppearancePopup"))
        {
            ImGui::SeparatorText(ui::i18n::tr("header.appearance"));
            ui::drawThemeSelector(settings);
            ui::i18n::drawLanguageSelector();
            ImGui::Separator();
            ImGui::SetNextItemWidth(220.0f);
            ImGui::SliderFloat(ui::i18n::tr("appearance.globalBrightness"), &settings.GlobalBrightness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetNextItemWidth(220.0f);
            ImGui::SliderFloat(ui::i18n::tr("appearance.previewInterpolation"), &settings.LiveOutputInterpolation, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ui::i18n::tr("appearance.previewInterpolationTooltip"));
            if (ImGui::Button(ui::i18n::tr("appearance.reset")))
            {
                settings.UiTheme = defaults.UiTheme;
                settings.SuspiciousColorThemes = defaults.SuspiciousColorThemes;
                settings.GlobalBrightness = defaults.GlobalBrightness;
                settings.LiveOutputInterpolation = defaults.LiveOutputInterpolation;
                ui::applyTheme(settings.UiTheme);
                ui::saveThemePreferences(settings);
            }
            ImGui::TextDisabled("%s", settingsPath().string().c_str());
            ImGui::EndPopup();
        }
        ImGui::Separator();

        if (showKeyboardPreview)
        {
            std::string title = std::string(ui::i18n::tr("keyboardPreview.title")) + "###QuartzKeyboardPreview";
            ImGui::SetNextWindowSize(ImVec2(560.0f, 270.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin(title.c_str(), &showKeyboardPreview, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
                drawFramebufferPreview(framebuffer, 1.0f, 520.0f, settings.LiveOutputInterpolation);
            ImGui::End();
        }
    }

    void drawUi'''

header_pattern = re.compile(r'    void drawPermanentHeader\(RawUSB& usb\)\n    \{.*?\n    \}\n\n    void drawUi', re.S)
text, count = header_pattern.subn(new_header, text, count=1)
if count != 1:
    raise RuntimeError(f"Expected one permanent header block, replaced {count}")

if '        drawPermanentHeader(usb);' not in text:
    raise RuntimeError("drawPermanentHeader call not found")
text = text.replace('        drawPermanentHeader(usb);', '        drawPermanentHeader(settings, framebuffer);', 1)

draw_ui = text.find('    void drawUi')
if draw_ui < 0:
    raise RuntimeError("drawUi not found after header migration")
prefix, body = text[:draw_ui], text[draw_ui:]
body_pattern = re.compile(r'        static const VisualizerSettings defaults\{\};.*?        pageManager\.render\(context\);', re.S)
body, count = body_pattern.subn('        pageManager.render(context);', body, count=1)
if count != 1:
    raise RuntimeError(f"Expected one legacy global-control block inside drawUi, replaced {count}")
text = prefix + body

path.write_text(text)
print("Updated", path)
