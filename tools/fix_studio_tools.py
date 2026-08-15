from pathlib import Path


def replace_once(path: str, old: str, new: str):
    p = Path(path)
    text = p.read_text()
    if old not in text:
        raise RuntimeError(f"missing replacement in {path}: {old!r}")
    p.write_text(text.replace(old, new, 1))


replace_once("include/quartz/client/native/MemoryScanner.hpp", "        MemoryScanner() = default;\n", "        MemoryScanner();\n")
replace_once("src/native/MemoryScanner.cpp", "    MemoryScanner::~MemoryScanner() { cancel(); _job.reset(); }\n", "    MemoryScanner::MemoryScanner() = default;\n    MemoryScanner::~MemoryScanner() { cancel(); _job.reset(); }\n")

p = Path("src/ui/RuntimeUI.cpp")
text = p.read_text()
count = text.count("ImGuiStyleVar_ChildPadding")
if count != 2:
    raise RuntimeError(f"expected 2 ChildPadding occurrences, got {count}")
p.write_text(text.replace("ImGuiStyleVar_ChildPadding", "ImGuiStyleVar_WindowPadding"))

print("studio tools compile fixes applied")
