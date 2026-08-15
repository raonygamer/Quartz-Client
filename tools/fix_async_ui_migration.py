from pathlib import Path

path = Path("tools/apply_async_ui_migration.py")
text = path.read_text()
old = '''def page_header(class_name: str, page_id: str, title: str, section: str = "Runtime") -> str:\n    return f\'''#pragma once\\n#include "quartz/client/ui/Page.hpp"\\n\\nnamespace quartz::client::ui\\n{{\\n    class {class_name} final : public Page\\n    {{\\n    public:\\n        [[nodiscard]] std::string_view id() const noexcept override {{ return "{page_id}"; }}\\n        [[nodiscard]] std::string_view title() const noexcept override {{ return "{title}"; }}\\n        [[nodiscard]] PageSection section() const noexcept override {{ return PageSection::{section}; }}\\n        void render(PageContext& context, PageManager& manager) override;\\n    }};\\n}}\\n\'''\n'''
new = '''def page_header(class_name: str, page_id: str, title: str, section: str = "Runtime") -> str:\n    return f\'''#pragma once\n#include "quartz/client/ui/Page.hpp"\n\nnamespace quartz::client::ui\n{{\n    class {class_name} final : public Page\n    {{\n    public:\n        [[nodiscard]] std::string_view id() const noexcept override {{ return "{page_id}"; }}\n        [[nodiscard]] std::string_view title() const noexcept override {{ return "{title}"; }}\n        [[nodiscard]] PageSection section() const noexcept override {{ return PageSection::{section}; }}\n        void render(PageContext& context, PageManager& manager) override;\n    }};\n}}\n\'''\n'''
if old not in text:
    raise RuntimeError("page_header generator target not found")
text = text.replace(old, new, 1)

old_pointer = '''replace_once("src/ui/RuntimeUI.cpp", '        static RuntimeMemoryInspectorState inspector;\\n        ImGui::TextUnformatted("Pointer assignments")', '        ImGui::TextUnformatted("Pointer assignments")')'''
new_pointer = '''replace_once("src/ui/RuntimeUI.cpp", '        static RuntimeMemoryInspectorState inspector;\\n        ImGui::TextUnformatted("Pointer assignments")', '        auto& inspector = runtimeMemoryInspectorState();\\n        ImGui::TextUnformatted("Pointer assignments")')'''
if old_pointer not in text:
    raise RuntimeError("pointer inspector migration target not found")
text = text.replace(old_pointer, new_pointer, 1)

anchor = '''replace_once("src/ui/RuntimeUI.cpp", '        drawRuntimeMemoryInspector(inspector);\\n    }\\n\\n    void drawRuntimeObjectDescriptors', '    }\\n\\n    void drawRuntimeObjectDescriptors')'''
addition = anchor + '''\nreplace_once("include/quartz/client/Functions.hpp", "    void drawRuntimeMemoryInspector(RuntimeMemoryInspectorState& state);", "    RuntimeMemoryInspectorState& runtimeMemoryInspectorState();\\n    void drawRuntimeMemoryInspector(RuntimeMemoryInspectorState& state);")\nreplace_once("src/ui/RuntimeUI.cpp", "    void drawRuntimeMemoryInspector(RuntimeMemoryInspectorState& state)\\n    {", "    RuntimeMemoryInspectorState& runtimeMemoryInspectorState()\\n    {\\n        static RuntimeMemoryInspectorState state;\\n        return state;\\n    }\\n\\n    void drawRuntimeMemoryInspector(RuntimeMemoryInspectorState& state)\\n    {")'''
if anchor not in text:
    raise RuntimeError("memory inspector anchor not found")
text = text.replace(anchor, addition, 1)
text = text.replace('        static RuntimeMemoryInspectorState inspector;\\n        auto& engine = context.runtimeBindings;', '        auto& inspector = runtimeMemoryInspectorState();\\n        auto& engine = context.runtimeBindings;', 1)
path.write_text(text)
print("fixed page generator and shared native inspector state")
