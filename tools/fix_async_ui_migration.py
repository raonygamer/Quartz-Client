from pathlib import Path

path = Path("tools/apply_async_ui_migration.py")
text = path.read_text()
old = '''def page_header(class_name: str, page_id: str, title: str, section: str = "Runtime") -> str:\n    return f\'''#pragma once\\n#include "quartz/client/ui/Page.hpp"\\n\\nnamespace quartz::client::ui\\n{{\\n    class {class_name} final : public Page\\n    {{\\n    public:\\n        [[nodiscard]] std::string_view id() const noexcept override {{ return "{page_id}"; }}\\n        [[nodiscard]] std::string_view title() const noexcept override {{ return "{title}"; }}\\n        [[nodiscard]] PageSection section() const noexcept override {{ return PageSection::{section}; }}\\n        void render(PageContext& context, PageManager& manager) override;\\n    }};\\n}}\\n\'''\n'''
new = '''def page_header(class_name: str, page_id: str, title: str, section: str = "Runtime") -> str:\n    return f\'''#pragma once\n#include "quartz/client/ui/Page.hpp"\n\nnamespace quartz::client::ui\n{{\n    class {class_name} final : public Page\n    {{\n    public:\n        [[nodiscard]] std::string_view id() const noexcept override {{ return "{page_id}"; }}\n        [[nodiscard]] std::string_view title() const noexcept override {{ return "{title}"; }}\n        [[nodiscard]] PageSection section() const noexcept override {{ return PageSection::{section}; }}\n        void render(PageContext& context, PageManager& manager) override;\n    }};\n}}\n\'''\n'''
if old not in text:
    raise RuntimeError("page_header generator target not found")
path.write_text(text.replace(old, new, 1))
print("fixed page header generator")
