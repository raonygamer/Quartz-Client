from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"{label}: anchor not found")
    return text.replace(old, new, 1)


path = Path("src/ui/PageManager.cpp")
text = path.read_text()
text = replace_once(text,
    'ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));',
    'ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.035f, 0.5f));',
    "navigation label inset")
path.write_text(text)


path = Path("src/ui/pages/JavaScriptPage.cpp")
text = path.read_text()
start = text.find('        if (ImGui::CollapsingHeader("Runtime API", ImGuiTreeNodeFlags_DefaultOpen))\n')
if start < 0:
    raise RuntimeError("Runtime API section: start anchor not found")
end_anchor = '        ImGui::SeparatorText("Scripts");'
end = text.find(end_anchor, start)
if end < 0:
    raise RuntimeError("Runtime API section: end anchor not found")
text = text[:start] + text[end:]
path.write_text(text)

print("Navigation inset and Runtime API cleanup applied")
# Validation trigger.
