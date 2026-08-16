#pragma once
#include <TextEditor.h>
#include <string>

namespace quartz::client::ui
{
    const TextEditor::Language* quartzTypeScriptLanguage();
    TextEditor::Palette quartzTextEditorPalette();
    void applyQuartzTextEditorPalette(TextEditor& editor);
    void configureQuartzTypeScriptEditor(TextEditor& editor, bool readOnly, bool miniMap = true);
    void configureQuartzReadOnlyTextEditor(TextEditor& editor, bool lineNumbers = false);
    std::string quartzDottedTokenAt(const TextEditor& editor, TextEditor::DocPos pos);
    void installQuartzScriptHover(TextEditor& editor);
}
