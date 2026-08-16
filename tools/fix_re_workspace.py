from pathlib import Path

path = Path("src/ui/MemoryInspector.cpp")
text = path.read_text()
old = "                    requestSignatureMaker(state.Pid, ui.DisassemblyLines[firstLine], static_cast<int>(lastLine - firstLine + 1));\n"
new = "                    ui::requestSignatureMaker(state.Pid, ui.DisassemblyLines[firstLine], static_cast<int>(lastLine - firstLine + 1));\n"
if old not in text:
    raise RuntimeError("signature maker namespace fix anchor not found")
path.write_text(text.replace(old, new, 1))
print("Applied reverse engineering follow-up fixes")
