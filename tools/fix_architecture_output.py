from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
inc = root / "include/quartz/client"

functions = (inc / "Functions.hpp").read_text()
# A global with .size() in its type looked function-like to the first-pass splitter.
functions = re.sub(r"^\s*std::array<ImFont\*, ShaderEditorZoomLevels\.size\(\)> ShaderEditorFonts;\n", "", functions, flags=re.M)
needle = "namespace quartz::client\n{\n"
templates = '''    template<typename T>\n    bool parseNumber(std::string_view value, T& result);\n\n    template<typename T>\n    bool readProcessMemoryValue(pid_t pid, std::uintptr_t address, T& value, std::string& error);\n\n'''
if "template<typename T>\n    bool parseNumber" not in functions:
    functions = functions.replace(needle, needle + templates, 1)
(inc / "Functions.hpp").write_text(functions)

model = (inc / "Model.hpp").read_text()
# The original source conditionally declares one of two font caches. The generic block splitter separated the
# preprocessor directive from the declarations; keep the constexpr zoom table in the model and move the cache itself
# to Globals.hpp/RuntimeUI.cpp.
model = model.replace("    inline constexpr std::array<float, 20> ShaderEditorZoomLevels{0.60f, 0.70f, 0.80f, 0.90f, 1.00f, 1.10f, 1.20f, 1.30f, 1.40f, 1.50f, 1.60f, 1.70f, 1.80f, 1.90f, 2.00f, 2.10f, 2.20f, 2.30f, 2.40f, 2.50f};\n#if IMGUI_VERSION_NUM >= 19200\n", "    inline constexpr std::array<float, 20> ShaderEditorZoomLevels{0.60f, 0.70f, 0.80f, 0.90f, 1.00f, 1.10f, 1.20f, 1.30f, 1.40f, 1.50f, 1.60f, 1.70f, 1.80f, 1.90f, 2.00f, 2.10f, 2.20f, 2.30f, 2.40f, 2.50f};\n")
(inc / "Model.hpp").write_text(model)

globals_h = (inc / "Globals.hpp").read_text()
globals_h = re.sub(r"^\s*extern ImFont\* ShaderEditorFont;\n", "", globals_h, flags=re.M)
font_decl = '''#if IMGUI_VERSION_NUM >= 19200\n    extern ImFont* ShaderEditorFont;\n#else\n    extern std::array<ImFont*, 20> ShaderEditorFonts;\n#endif\n'''
globals_h = globals_h.replace("}\n", font_decl + "}\n", 1)
(inc / "Globals.hpp").write_text(globals_h)

runtime_ui = root / "src/ui/RuntimeUI.cpp"
text = runtime_ui.read_text()
# Reassemble the original conditional global pair after the first-pass splitter externalized them.
pattern = re.compile(r"\s*ImFont\* ShaderEditorFont = nullptr;\s*#else\s*std::array<ImFont\*, ShaderEditorZoomLevels\.size\(\)> ShaderEditorFonts\{\};\s*#endif", re.S)
replacement = '''\n#if IMGUI_VERSION_NUM >= 19200\n    ImFont* ShaderEditorFont = nullptr;\n#else\n    std::array<ImFont*, ShaderEditorZoomLevels.size()> ShaderEditorFonts{};\n#endif'''
text, count = pattern.subn(replacement, text, count=1)
if count != 1:
    raise SystemExit("failed to reconstruct shader editor font conditional")
runtime_ui.write_text(text)

# The monolith had a file-order-only static forward declaration immediately before readNativeBinding(). Functions.hpp
# now provides the real cross-TU declaration and Runtime.cpp owns the definition, so keeping this static declaration
# would give the same function conflicting linkage inside RuntimeNative.cpp.
native = root / "src/runtime/RuntimeNative.cpp"
text = native.read_text()
text = text.replace("    static std::string runtimeHexAddress(std::uintptr_t value);\n", "")
native.write_text(text)

print("fixed generated template declarations, conditional globals, and native forward declarations")
