from pathlib import Path
import re


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"{label}: anchor not found")
    return text.replace(old, new, 1)


def remove_between(text: str, start: str, end: str, label: str, include_end: bool = True) -> str:
    begin = text.find(start)
    if begin < 0:
        raise RuntimeError(f"{label}: start anchor not found")
    finish = text.find(end, begin)
    if finish < 0:
        raise RuntimeError(f"{label}: end anchor not found")
    if include_end:
        finish += len(end)
    return text[:begin] + text[finish:]


# Remove the old q.re compatibility alias and move declarations out of QuickJSApi.cpp.
path = Path("src/runtime/QuickJSApi.cpp")
text = path.read_text()
text = remove_between(text,
    "        // Compatibility alias for scripts written before the namespace split.\n",
    '        JS_SetPropertyStr(ctx, api, "re", re);\n',
    "q.re compatibility alias")
start = text.find("    std::string_view runtimeQuickJSTypeDeclarations() noexcept\n")
if start < 0:
    raise RuntimeError("QuickJS declarations: start anchor not found")
if not text.rstrip().endswith("}"):
    raise RuntimeError("QuickJSApi.cpp: unexpected file ending")
text = text[:start].rstrip() + "\n}\n"
path.write_text(text)


# The first-class script model no longer has a legacy bridge toggle.
path = Path("include/quartz/client/runtime/RuntimeTypes.hpp")
text = path.read_text()
text = replace_once(text, "        bool LegacyBridge = false;\n", "", "RuntimeScript LegacyBridge")
path.write_text(text)


path = Path("src/runtime/QuickJSInternal.hpp")
text = path.read_text()
text = replace_once(text, "        bool AllowGraphMutation = false;\n        bool LegacyBridge = false;\n", "", "QuickJS legacy context flags")
text = replace_once(text, "    void runtimeInstallQuickJSGraphApi(JSContext* ctx, JSValueConst api);\n", "", "QuickJS graph API declaration")
path.write_text(text)


# Strip the bridge implementation from the workspace runtime itself.
path = Path("src/runtime/QuickJSWorkspace.cpp")
text = path.read_text()
text = replace_once(text, '#include "quartz/client/runtime/RuntimeBindingEngine.hpp"\n', "", "workspace binding engine include")
text = replace_once(text, "        constexpr double MaximumSafeInteger = 9007199254740991.0;\n", "", "workspace legacy safe integer")
text = remove_between(text,
    "        RuntimeBinding* resolveBinding(JSContext* ctx, JSValueConst value)\n",
    "        void appendLog(RuntimeScript& script, const double time, const RuntimeScriptLogLevel level, std::string text)\n",
    "workspace legacy accessors",
    include_end=False)
text = remove_between(text,
    "        void installLegacyBridge(Instance& instance)\n",
    "        Instance* createInstance(JavaScriptRuntime& javascript, RuntimeBindingEngine& legacy, RuntimeScript& script, const RuntimeSignalContext& signal, ShaderFramebuffer& shader, RuntimeControlOutput& output, const EvdevKeyboard& keyboard)\n",
    "workspace legacy installer",
    include_end=False)
text = replace_once(text,
    "        Instance* createInstance(JavaScriptRuntime& javascript, RuntimeBindingEngine& legacy, RuntimeScript& script, const RuntimeSignalContext& signal, ShaderFramebuffer& shader, RuntimeControlOutput& output, const EvdevKeyboard& keyboard)\n",
    "        Instance* createInstance(JavaScriptRuntime& javascript, RuntimeScript& script, const RuntimeSignalContext& signal, ShaderFramebuffer& shader, RuntimeControlOutput& output, const EvdevKeyboard& keyboard)\n",
    "workspace createInstance signature")
for old in (
    "            instance->Engine = script.LegacyBridge ? &legacy : nullptr;\n",
    "            instance->AllowGraphMutation = script.LegacyBridge;\n",
    "            instance->LegacyBridge = script.LegacyBridge;\n",
    "            runtimeInstallQuickJSGraphApi(instance->Context, instance->Api);\n",
    "            if (script.LegacyBridge) installLegacyBridge(*instance);\n            else removeGraphWhenLegacyDisabled(instance->Context, instance->Api);\n",
):
    if old not in text:
        raise RuntimeError(f"workspace cleanup: anchor not found: {old.strip()}")
    text = text.replace(old, "", 1)
text = replace_once(text,
    "        bool evaluate(JavaScriptRuntime& javascript, RuntimeBindingEngine& legacy, RuntimeScript& script, const RuntimeSignalContext& signal, ShaderFramebuffer& shader, RuntimeControlOutput& output, const EvdevKeyboard& keyboard)\n",
    "        bool evaluate(JavaScriptRuntime& javascript, RuntimeScript& script, const RuntimeSignalContext& signal, ShaderFramebuffer& shader, RuntimeControlOutput& output, const EvdevKeyboard& keyboard)\n",
    "workspace evaluate signature")
text = replace_once(text,
    "            Instance* instance = createInstance(javascript, legacy, script, signal, shader, output, keyboard);\n",
    "            Instance* instance = createInstance(javascript, script, signal, shader, output, keyboard);\n",
    "workspace createInstance call")
for old in (
    "            instance->Engine = script.LegacyBridge ? &legacy : nullptr;\n",
    "            instance->AllowGraphMutation = script.LegacyBridge;\n",
    "            instance->LegacyBridge = script.LegacyBridge;\n",
):
    if old not in text:
        raise RuntimeError(f"workspace evaluation cleanup: anchor not found: {old.strip()}")
    text = text.replace(old, "", 1)
text = replace_once(text,
    '            script.Status = script.LegacyBridge ? "running (legacy graph bridge enabled)" : "running";\n',
    '            script.Status = "running";\n',
    "workspace running status")
text = replace_once(text,
    "    const RuntimeControlOutput& runtimeEvaluateWorkspaceScripts(JavaScriptRuntime& javascript, RuntimeBindingEngine& legacy, const RuntimeSignalContext& context, ShaderFramebuffer& shader, const EvdevKeyboard& keyboard)\n",
    "    const RuntimeControlOutput& runtimeEvaluateWorkspaceScripts(JavaScriptRuntime& javascript, const RuntimeSignalContext& context, ShaderFramebuffer& shader, const EvdevKeyboard& keyboard)\n",
    "workspace public evaluator signature")
text = text.replace("evaluate(javascript, legacy, *script, context, shader, javascript.outputFor(script->Id), keyboard)", "evaluate(javascript, *script, context, shader, javascript.outputFor(script->Id), keyboard)")
if any(token in text for token in ("LegacyBridge", "installLegacyBridge", "runtimeInstallQuickJSGraphApi", "removeGraphWhenLegacyDisabled")):
    raise RuntimeError("workspace still contains legacy bridge symbols")
path.write_text(text)


path = Path("include/quartz/client/runtime/QuickJS.hpp")
text = path.read_text()
text = replace_once(text,
    "    const RuntimeControlOutput& runtimeEvaluateWorkspaceScripts(JavaScriptRuntime& javascript, RuntimeBindingEngine& legacy, const RuntimeSignalContext& context, ShaderFramebuffer& shader, const EvdevKeyboard& keyboard);\n",
    "    const RuntimeControlOutput& runtimeEvaluateWorkspaceScripts(JavaScriptRuntime& javascript, const RuntimeSignalContext& context, ShaderFramebuffer& shader, const EvdevKeyboard& keyboard);\n",
    "QuickJS workspace evaluator declaration")
path.write_text(text)


# Keep profile integration, but completely remove migration/serialization of the bridge.
path = Path("include/quartz/client/runtime/JavaScriptRuntime.hpp")
text = path.read_text()
text = replace_once(text, "        explicit JavaScriptRuntime(RuntimeBindingEngine& legacy);\n", "        JavaScriptRuntime();\n", "JavaScriptRuntime constructor")
text = replace_once(text, "        void migrateLegacy(RuntimeBindingEngine& legacy, bool hadOwnFile);\n", "", "legacy migration declaration")
path.write_text(text)


path = Path("src/runtime/JavaScriptRuntime.cpp")
text = path.read_text()
old_constructor = '''    JavaScriptRuntime::JavaScriptRuntime(RuntimeBindingEngine& legacy)
    {
        _path = settingsPath().parent_path() / "visualizer.javascript.ini";
        const bool hadOwnFile = std::filesystem::exists(_path);
        load();
        migrateLegacy(legacy, hadOwnFile);
    }
'''
new_constructor = '''    JavaScriptRuntime::JavaScriptRuntime()
    {
        _path = settingsPath().parent_path() / "visualizer.javascript.ini";
        load();
    }
'''
text = replace_once(text, old_constructor, new_constructor, "JavaScriptRuntime constructor body")
text = replace_once(text, '        file << "# Quartz JavaScript runtime v1\\n";\n', '        file << "# Quartz script runtime v2\\n";\n', "script runtime version")
text = replace_once(text,
    "            file << \"S\\t\" << script.Enabled << '\\t' << script.Id << '\\t' << runtimeEscape(script.Name) << '\\t' << script.External << '\\t' << runtimeEscape(script.Path) << '\\t' << script.HotReload << '\\t' << script.UpdateHz << '\\t' << script.TimeoutMs << '\\t' << script.Order << '\\t' << runtimeEscape(script.Group) << '\\t' << script.LegacyBridge << '\\t' << runtimeEscape(script.PersistentStateJson) << '\\t' << runtimeEscape(script.Source) << '\\n';\n",
    "            file << \"S\\t\" << script.Enabled << '\\t' << script.Id << '\\t' << runtimeEscape(script.Name) << '\\t' << script.External << '\\t' << runtimeEscape(script.Path) << '\\t' << script.HotReload << '\\t' << script.UpdateHz << '\\t' << script.TimeoutMs << '\\t' << script.Order << '\\t' << runtimeEscape(script.Group) << '\\t' << runtimeEscape(script.PersistentStateJson) << '\\t' << runtimeEscape(script.Source) << '\\n';\n",
    "script serialization")
old_load = '''                if (fields.size() > 10) parseBool(fields[10], script.LegacyBridge);
                if (fields.size() > 11) script.PersistentStateJson = runtimeUnescape(fields[11]);
                if (fields.size() > 12) script.Source = runtimeUnescape(fields[12]);
'''
new_load = '''                const bool oldLayout = fields.size() > 12;
                const std::size_t storageIndex = oldLayout ? 11 : 10;
                const std::size_t sourceIndex = oldLayout ? 12 : 11;
                if (fields.size() > storageIndex) script.PersistentStateJson = runtimeUnescape(fields[storageIndex]);
                if (fields.size() > sourceIndex) script.Source = runtimeUnescape(fields[sourceIndex]);
'''
text = replace_once(text, old_load, new_load, "script layout migration")
text = remove_between(text,
    "    void JavaScriptRuntime::migrateLegacy(RuntimeBindingEngine& legacy, const bool hadOwnFile)\n",
    "    void JavaScriptRuntime::syncProfile(RuntimeBindingEngine& legacy)\n",
    "legacy script migration",
    include_end=False)
text = text.replace("JavaScriptRuntime::erase(const std::size_t index, RuntimeBindingEngine& legacy)", "JavaScriptRuntime::erase(const std::size_t index, RuntimeBindingEngine& runtime)")
text = text.replace("for (auto& profile : legacy.profiles())", "for (auto& profile : runtime.profiles())")
text = text.replace("legacy.markChanged();", "runtime.markChanged();")
text = text.replace("JavaScriptRuntime::syncProfile(RuntimeBindingEngine& legacy)", "JavaScriptRuntime::syncProfile(RuntimeBindingEngine& runtime)")
text = text.replace("legacy.activeProfileId()", "runtime.activeProfileId()")
text = text.replace("legacy.revision()", "runtime.revision()")
text = text.replace("legacy.findProfile(activeId)", "runtime.findProfile(activeId)")
if "LegacyBridge" in text or "migrateLegacy" in text:
    raise RuntimeError("JavaScriptRuntime still contains legacy bridge state")
path.write_text(text)


path = Path("include/quartz/client/runtime/JavaScriptRuntime.hpp")
text = path.read_text().replace("void erase(std::size_t index, RuntimeBindingEngine& legacy);", "void erase(std::size_t index, RuntimeBindingEngine& runtime);")
text = text.replace("void syncProfile(RuntimeBindingEngine& legacy);", "void syncProfile(RuntimeBindingEngine& runtime);")
path.write_text(text)


path = Path("src/Application.cpp")
text = path.read_text()
text = replace_once(text, "    JavaScriptRuntime javascript(runtimeBindings);\n", "    JavaScriptRuntime javascript;\n", "Application script runtime construction")
text = replace_once(text,
    "        const RuntimeControlOutput& mainScriptOutput = runtimeEvaluateWorkspaceScripts(javascript, runtimeBindings, javascriptContext, shaderFramebuffer, keyboardInput);\n",
    "        const RuntimeControlOutput& mainScriptOutput = runtimeEvaluateWorkspaceScripts(javascript, javascriptContext, shaderFramebuffer, keyboardInput);\n",
    "Application workspace evaluation")
path.write_text(text)


# TypeScript-oriented source viewer and zero legacy controls in the Scripts UI.
path = Path("src/ui/pages/JavaScriptPage.cpp")
text = path.read_text()
text = text.replace("javascriptLanguage", "typescriptLanguage").replace("javascriptPalette", "typescriptPalette")
text = replace_once(text, '                value.name = "JavaScript";\n', '                value.name = "TypeScript";\n', "TypeScript editor language name")
text = replace_once(text,
    '                value.keywords = {"async","await","break","case","catch","class","const","continue","debugger","default","delete","do","else","export","extends","finally","for","from","function","get","if","import","in","instanceof","let","new","of","return","set","static","super","switch","this","throw","try","typeof","var","void","while","with","yield"};\n',
    '                value.keywords = {"abstract","any","as","asserts","async","await","bigint","boolean","break","case","catch","class","const","constructor","continue","debugger","declare","default","delete","do","else","export","extends","finally","for","from","function","get","if","implements","import","in","infer","instanceof","interface","is","keyof","let","module","namespace","never","new","number","object","of","override","private","protected","public","readonly","require","return","satisfies","set","static","string","super","switch","symbol","this","throw","try","type","typeof","unknown","var","void","while","with","yield"};\n',
    "TypeScript keywords")
text = replace_once(text,
    '                value.declarations = {"true","false","null","undefined","legacy","binding","raw","text","address","bank","control","triggered","graph","re"};\n',
    '                value.declarations = {"true","false","null","undefined"};\n',
    "script declarations")
text = replace_once(text,
    '                value.identifiers = {"q","Math","JSON","BigInt","Number","String","Boolean","Array","Object","Map","Set","WeakMap","WeakSet","Date","RegExp","Promise","Error","TypeError","NaN","Infinity","process","memory","signature","disassembly","breakpoint","input","events","runtime","state","storage","list","find","alive","modules","regions","read","write","readBytes","writeBytes","decode","arm","cancel","running","hit","keyDown","shortcut","capsLock","scrollLock","subscribe","unsubscribe","emit","scan","status","console","debug","info","warn","error","shader","shaderPreset","brightness","sendFramebuffer","baseColorMode","material","currentShader","previousShader","clear","loop","log"};\n',
    '                value.identifiers = {"Math","JSON","BigInt","Number","String","Boolean","Array","Object","Map","Set","WeakMap","WeakSet","Date","RegExp","Promise","Error","TypeError","NaN","Infinity","Process","Signature","Breakpoint","Struct","Field","Pointer","Property","Runtime","Script","System","Disassembly","Memory","Keyboard","Events","console"};\n',
    "SDK identifiers")
text = replace_once(text,
    "        struct EditorState { TextEditor Editor; std::string Synced; bool Initialized = false; };\n",
    "        struct EditorState { TextEditor Editor; std::string Synced; std::filesystem::path Path; std::filesystem::file_time_type Time{}; std::string Error; bool Initialized = false; };\n",
    "editor state")
text = replace_once(text,
    "                state.Editor.SetLanguage(typescriptLanguage()); state.Editor.SetPalette(typescriptPalette()); state.Editor.SetTabSize(4); state.Editor.SetInsertSpacesOnTabs(true); state.Editor.SetAutoIndentEnabled(true); state.Editor.SetShowLineNumbersEnabled(true); state.Editor.SetShowMatchingBrackets(true); state.Editor.SetShowMiniMapEnabled(true); state.Editor.SetText(script.Source); state.Synced = script.Source; state.Initialized = true;\n",
    "                state.Editor.SetLanguage(typescriptLanguage()); state.Editor.SetPalette(typescriptPalette()); state.Editor.SetTabSize(4); state.Editor.SetInsertSpacesOnTabs(true); state.Editor.SetAutoIndentEnabled(true); state.Editor.SetShowLineNumbersEnabled(true); state.Editor.SetShowMatchingBrackets(true); state.Editor.SetShowMiniMapEnabled(true); state.Editor.SetReadOnlyEnabled(false); state.Editor.SetText(script.Source); state.Synced = script.Source; state.Initialized = true;\n",
    "editable script editor setup")
editor_end = '''            return state;
        }

        bool materializeExternal(RuntimeScript& script, std::string& error)
'''
external_helpers = '''            return state;
        }

        bool readExternalSource(const std::filesystem::path& path, std::string& source, std::string& error) noexcept
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec)) { error = ec ? ec.message() : "not a regular file"; return false; }
            try
            {
                std::ifstream file(path, std::ios::binary); if (!file) { error = "could not open file"; return false; }
                source.assign(std::istreambuf_iterator<char>(file), {});
                if (!file && !file.eof()) { error = "could not read file"; return false; }
                error.clear(); return true;
            }
            catch (const std::exception& exception) { error = exception.what(); return false; }
            catch (...) { error = "unknown filesystem error"; return false; }
        }

        EditorState& externalEditor(RuntimeScript& script, const std::filesystem::path& path)
        {
            static std::unordered_map<std::uint64_t, std::unique_ptr<EditorState>> editors;
            auto [it, inserted] = editors.try_emplace(script.Id); if (inserted) it->second = std::make_unique<EditorState>(); auto& state = *it->second;
            std::error_code ec; const auto time = std::filesystem::last_write_time(path, ec); const bool changed = !state.Initialized || state.Path != path || (!ec && state.Time != time);
            if (!state.Initialized)
            {
                state.Editor.SetLanguage(typescriptLanguage()); state.Editor.SetPalette(typescriptPalette()); state.Editor.SetTabSize(4); state.Editor.SetInsertSpacesOnTabs(true); state.Editor.SetAutoIndentEnabled(true); state.Editor.SetShowLineNumbersEnabled(true); state.Editor.SetShowMatchingBrackets(true); state.Editor.SetShowMiniMapEnabled(true); state.Editor.SetReadOnlyEnabled(true); state.Initialized = true;
            }
            if (changed)
            {
                std::string source;
                if (readExternalSource(path, source, state.Error)) { state.Editor.SetText(source); state.Synced = std::move(source); state.Path = path; state.Time = time; }
                else if (state.Path != path) { state.Editor.ClearText(); state.Synced.clear(); state.Path = path; state.Time = {}; }
            }
            return state;
        }

        bool materializeExternal(RuntimeScript& script, std::string& error)
'''
text = replace_once(text, editor_end, external_helpers, "external script viewer helpers")
text = text.replace("    void JavaScriptPage::render(PageContext& context, PageManager& manager)\n", "    void JavaScriptPage::render(PageContext& context, PageManager&)\n", 1)
text = replace_once(text,
    '        ImGui::TextWrapped("JavaScript is Quartz\'s first-class automation runtime. Scripts have their own lifecycle, persistent q.storage, events, process/memory/signature/disassembly APIs and runtime output. They do not require bindings, controls or the value bank.");\n        ImGui::TextDisabled("Bindings / Controls / Value Bank remain available only as an explicitly enabled deprecated bridge for old setups.");\n',
    '        ImGui::TextWrapped("Quartz scripts are moving to a TypeScript-first SDK. External TypeScript and JavaScript sources stay visible here alongside runtime state, diagnostics and console output.");\n',
    "Scripts page introduction")
text = replace_once(text,
    '        if (ImGui::Button("+ Inline script")) { auto& script = javascript.add(); script.Source = "// Quartz runtime script\\n// q.process / q.memory / q.signature / q.disassembly / q.breakpoint / q.input / q.events / q.runtime\\n"; }\n',
    '        if (ImGui::Button("+ Inline script")) { auto& script = javascript.add(); script.Source = "// Quartz script\\n"; }\n',
    "new inline script source")
text = text.replace('status = "all JavaScript contexts reloaded"', 'status = "all script contexts reloaded"')
text = text.replace('ImGui::Button("Save .d.ts")', 'ImGui::Button("Save @quartz/client types")')
text = text.replace('ImGui::TextDisabled("watches root scripts and q.import() dependencies")', 'ImGui::TextDisabled("watches root scripts and imported dependencies")')
legacy_ui = '''                const bool oldLegacyBridge = script.LegacyBridge; ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.67f, 0.28f, 1.0f)); localChanged |= ImGui::Checkbox("Legacy bindings/controls/value-bank bridge (deprecated)", &script.LegacyBridge); ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off by default. When enabled, q.legacy.* plus deprecated q.binding/q.bank/q.control/q.graph compatibility aliases are installed for this script.");
                if (oldLegacyBridge != script.LegacyBridge) { runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); ++script.ReloadCount; }
'''
text = replace_once(text, legacy_ui, "", "legacy bridge UI")
old_external = '''                            std::filesystem::path resolved = script.Path; if (resolved.is_relative()) resolved = runtimeQuickJSScriptDirectory() / resolved; std::error_code ec; const bool regular = std::filesystem::is_regular_file(resolved, ec);
                            if (!regular) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f,0.35f,0.32f,1.0f)); ImGui::TextWrapped("Invalid external script: %s", ec ? ec.message().c_str() : resolved.string().c_str()); ImGui::PopStyleColor(); }
                            ImGui::TextDisabled("Relative paths resolve under %s", runtimeQuickJSScriptDirectory().string().c_str());
'''
new_external = '''                            std::filesystem::path resolved = script.Path; if (resolved.is_relative()) resolved = runtimeQuickJSScriptDirectory() / resolved; std::error_code ec; const bool regular = std::filesystem::is_regular_file(resolved, ec);
                            if (!regular) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f,0.35f,0.32f,1.0f)); ImGui::TextWrapped("Invalid external script: %s", ec ? ec.message().c_str() : resolved.string().c_str()); ImGui::PopStyleColor(); }
                            ImGui::TextDisabled("%s", resolved.string().c_str());
                            if (regular)
                            {
                                auto& external = externalEditor(script, resolved);
                                ImGui::SameLine(); ImGui::TextDisabled("read-only live view");
                                if (!external.Error.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f,0.35f,0.32f,1.0f)); ImGui::TextWrapped("Could not refresh source view: %s", external.Error.c_str()); ImGui::PopStyleColor(); }
                                external.Editor.Render("##ExternalScriptView", ImVec2(-1.0f, 360.0f));
                            }
                            ImGui::TextDisabled("Relative paths resolve under %s", runtimeQuickJSScriptDirectory().string().c_str());
'''
text = replace_once(text, old_external, new_external, "external source view")
text = text.replace('ImGui::SeparatorText("q.state")', 'ImGui::SeparatorText("Script.state")')
text = text.replace('ImGui::SeparatorText("q.storage")', 'ImGui::SeparatorText("Script.storage")')
compat_start = text.find('        ImGui::SeparatorText("Deprecated compatibility");\n')
if compat_start < 0:
    raise RuntimeError("deprecated compatibility UI: start anchor not found")
compat_end = text.find("        javascript.saveIfChanged();\n", compat_start)
if compat_end < 0:
    raise RuntimeError("deprecated compatibility UI: end anchor not found")
text = text[:compat_start] + text[compat_end:]
for token in ("LegacyBridge", "q.legacy", "q.graph", "Bindings (deprecated)", "Controls (deprecated)", "Value Bank (deprecated)"):
    if token in text:
        raise RuntimeError(f"Scripts UI still contains legacy token: {token}")
path.write_text(text)


# The graph bridge is no longer part of the product/runtime.
graph = Path("src/runtime/QuickJSGraphApi.cpp")
if not graph.exists():
    raise RuntimeError("QuickJSGraphApi.cpp already missing")
graph.unlink()

print("TypeScript SDK declarations, legacy bridge cleanup and external source viewer applied")
