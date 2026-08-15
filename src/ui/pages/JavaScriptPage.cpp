#include "quartz/client/ui/pages/JavaScriptPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/runtime/JavaScriptRuntime.hpp"
#include "quartz/client/runtime/QuickJS.hpp"
#include <TextEditor.h>
#include <memory>
#include <unordered_map>

namespace quartz::client::ui
{
    namespace
    {
        const TextEditor::Language* javascriptLanguage()
        {
            static const TextEditor::Language language = []
            {
                TextEditor::Language value;
                value.name = "JavaScript";
                value.singleLineComment = "//";
                value.commentStart = "/*";
                value.commentEnd = "*/";
                value.hasSingleQuotedStrings = true;
                value.hasDoubleQuotedStrings = true;
                value.otherStringStart = "`";
                value.otherStringEnd = "`";
                value.stringEscape = '\\';
                value.keywords = {"async","await","break","case","catch","class","const","continue","debugger","default","delete","do","else","export","extends","finally","for","from","function","get","if","import","in","instanceof","let","new","of","return","set","static","super","switch","this","throw","try","typeof","var","void","while","with","yield"};
                value.declarations = {"true","false","null","undefined","legacy","binding","raw","text","address","bank","control","triggered","graph","re"};
                value.identifiers = {"q","Math","JSON","BigInt","Number","String","Boolean","Array","Object","Map","Set","WeakMap","WeakSet","Date","RegExp","Promise","Error","TypeError","NaN","Infinity","process","memory","signature","disassembly","breakpoint","input","events","runtime","state","storage","list","find","alive","modules","regions","read","write","readBytes","writeBytes","decode","arm","cancel","running","hit","keyDown","shortcut","capsLock","scrollLock","subscribe","unsubscribe","emit","shader","shaderPreset","brightness","sendFramebuffer","baseColorMode","material","currentShader","previousShader","clear","loop","log"};
                value.isPunctuation = [](const ImWchar c) { return std::string_view("[]{}().,;:+-*/%<>=!&|^~?").find(static_cast<char>(c)) != std::string_view::npos; };
                value.getIdentifier = [](TextEditor::Iterator start, const TextEditor::Iterator end)
                {
                    if (start == end || !(TextEditor::CodePoint::isXidStart(*start) || *start == '_' || *start == '$')) return start;
                    auto current = start; ++current; while (current != end && (TextEditor::CodePoint::isXidContinue(*current) || *current == '$')) ++current; return current;
                };
                value.getNumber = [](TextEditor::Iterator start, const TextEditor::Iterator end)
                {
                    if (start == end) return start; auto current = start; auto next = current; ++next;
                    const auto digit = [](const ImWchar c) { return c >= '0' && c <= '9'; };
                    const auto hex = [&](const ImWchar c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
                    if (*current == '.' && (next == end || !digit(*next))) return start;
                    if (*current == '0' && next != end && (*next == 'x' || *next == 'X' || *next == 'b' || *next == 'B' || *next == 'o' || *next == 'O'))
                    {
                        const ImWchar prefix = *next; current = next; ++current;
                        while (current != end && (*current == '_' || (prefix == 'x' || prefix == 'X' ? hex(*current) : prefix == 'b' || prefix == 'B' ? (*current == '0' || *current == '1') : (*current >= '0' && *current <= '7')))) ++current;
                        if (current != end && *current == 'n') ++current; return current;
                    }
                    bool dot = false; if (*current == '.') { dot = true; ++current; } while (current != end && (digit(*current) || *current == '_')) ++current;
                    if (!dot && current != end && *current == '.') { ++current; while (current != end && (digit(*current) || *current == '_')) ++current; }
                    if (current != end && (*current == 'e' || *current == 'E')) { auto exponent = current; ++exponent; if (exponent != end && (*exponent == '+' || *exponent == '-')) ++exponent; bool any = false; while (exponent != end && (digit(*exponent) || *exponent == '_')) { any |= digit(*exponent); ++exponent; } if (any) current = exponent; }
                    if (current != end && *current == 'n') ++current; return current;
                };
                return value;
            }();
            return &language;
        }

        TextEditor::Palette javascriptPalette()
        {
            auto palette = shaderEditorPalette();
            palette[static_cast<std::size_t>(TextEditor::Color::keyword)] = IM_COL32(198, 120, 221, 255);
            palette[static_cast<std::size_t>(TextEditor::Color::declaration)] = IM_COL32(224, 161, 83, 255);
            palette[static_cast<std::size_t>(TextEditor::Color::number)] = IM_COL32(181, 206, 168, 255);
            palette[static_cast<std::size_t>(TextEditor::Color::string)] = IM_COL32(152, 195, 121, 255);
            palette[static_cast<std::size_t>(TextEditor::Color::knownIdentifier)] = IM_COL32(86, 182, 194, 255);
            palette[static_cast<std::size_t>(TextEditor::Color::identifier)] = IM_COL32(220, 223, 228, 255);
            palette[static_cast<std::size_t>(TextEditor::Color::punctuation)] = IM_COL32(171, 178, 191, 255);
            palette[static_cast<std::size_t>(TextEditor::Color::comment)] = IM_COL32(92, 99, 112, 255);
            return palette;
        }

        struct EditorState { TextEditor Editor; std::string Synced; bool Initialized = false; };
        EditorState& editor(RuntimeScript& script)
        {
            static std::unordered_map<std::uint64_t, std::unique_ptr<EditorState>> editors;
            auto [it, inserted] = editors.try_emplace(script.Id); if (inserted) it->second = std::make_unique<EditorState>(); auto& state = *it->second;
            if (!state.Initialized)
            {
                state.Editor.SetLanguage(javascriptLanguage()); state.Editor.SetPalette(javascriptPalette()); state.Editor.SetTabSize(4); state.Editor.SetInsertSpacesOnTabs(true); state.Editor.SetAutoIndentEnabled(true); state.Editor.SetShowLineNumbersEnabled(true); state.Editor.SetShowMatchingBrackets(true); state.Editor.SetShowMiniMapEnabled(true); state.Editor.SetText(script.Source); state.Synced = script.Source; state.Initialized = true;
            }
            else if (state.Synced != script.Source) { state.Editor.SetText(script.Source); state.Synced = script.Source; }
            return state;
        }

        struct KeyOption { const char* Name; int Key; };
        static constexpr KeyOption Keys[] = {{"None",0},{"F1",GLFW_KEY_F1},{"F2",GLFW_KEY_F2},{"F3",GLFW_KEY_F3},{"F4",GLFW_KEY_F4},{"F5",GLFW_KEY_F5},{"F6",GLFW_KEY_F6},{"F7",GLFW_KEY_F7},{"F8",GLFW_KEY_F8},{"F9",GLFW_KEY_F9},{"F10",GLFW_KEY_F10},{"F11",GLFW_KEY_F11},{"F12",GLFW_KEY_F12},{"A",GLFW_KEY_A},{"B",GLFW_KEY_B},{"C",GLFW_KEY_C},{"D",GLFW_KEY_D},{"E",GLFW_KEY_E},{"F",GLFW_KEY_F},{"G",GLFW_KEY_G},{"H",GLFW_KEY_H},{"I",GLFW_KEY_I},{"J",GLFW_KEY_J},{"K",GLFW_KEY_K},{"L",GLFW_KEY_L},{"M",GLFW_KEY_M},{"N",GLFW_KEY_N},{"O",GLFW_KEY_O},{"P",GLFW_KEY_P},{"Q",GLFW_KEY_Q},{"R",GLFW_KEY_R},{"S",GLFW_KEY_S},{"T",GLFW_KEY_T},{"U",GLFW_KEY_U},{"V",GLFW_KEY_V},{"W",GLFW_KEY_W},{"X",GLFW_KEY_X},{"Y",GLFW_KEY_Y},{"Z",GLFW_KEY_Z},{"0",GLFW_KEY_0},{"1",GLFW_KEY_1},{"2",GLFW_KEY_2},{"3",GLFW_KEY_3},{"4",GLFW_KEY_4},{"5",GLFW_KEY_5},{"6",GLFW_KEY_6},{"7",GLFW_KEY_7},{"8",GLFW_KEY_8},{"9",GLFW_KEY_9}};
    }

    void JavaScriptPage::render(PageContext& context, PageManager& manager)
    {
        auto& javascript = context.javascript; auto& settings = javascript.settings(); static std::string status;
        ImGui::TextWrapped("JavaScript is Quartz's first-class automation runtime. Scripts have their own lifecycle, persistent q.storage, events, process/memory/signature/disassembly APIs and runtime output. They do not require bindings, controls or the value bank.");
        ImGui::TextDisabled("Bindings / Controls / Value Bank remain available only as an explicitly enabled deprecated bridge for old setups.");

        if (ImGui::Button("+ Inline script")) { auto& script = javascript.add(); script.Source = "// Quartz runtime script\n// q.process / q.memory / q.signature / q.disassembly / q.breakpoint / q.input / q.events / q.runtime\n"; }
        ImGui::SameLine(); if (ImGui::Button("+ External script")) { auto& script = javascript.add(); script.External = true; script.Path = (runtimeQuickJSScriptDirectory() / "script.js").string(); }
        ImGui::SameLine(); if (ImGui::Button("Reload all")) { runtimeReloadAllWorkspaceScripts(); for (auto& script : javascript.scripts()) ++script.ReloadCount; status = "all JavaScript contexts reloaded"; }
        ImGui::SameLine(); if (ImGui::Button("Save .d.ts")) { std::string error; status = runtimeSaveQuickJSTypeDeclarations(error) ? "saved " + runtimeQuickJSTypeDeclarationsPath().string() : error; }
        ImGui::SameLine(); if (ImGui::Button("Save runtime")) status = javascript.save() ? "saved " + javascript.path().string() : "could not save JavaScript runtime";
        if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());
        ImGui::TextDisabled("Runtime: %s", javascript.path().string().c_str());
        ImGui::TextDisabled("External scripts: %s", runtimeQuickJSScriptDirectory().string().c_str());

        bool changed = false; changed |= ImGui::Checkbox("External script hot reload", &settings.ExternalHotReload); ImGui::SameLine(); ImGui::TextDisabled("watches root scripts and q.import() dependencies");
        ImGui::SeparatorText("Global reload hotkey");
        changed |= ImGui::Checkbox("Ctrl##jsReload", &settings.ReloadHotkeyCtrl); ImGui::SameLine(); changed |= ImGui::Checkbox("Alt##jsReload", &settings.ReloadHotkeyAlt); ImGui::SameLine(); changed |= ImGui::Checkbox("Shift##jsReload", &settings.ReloadHotkeyShift); ImGui::SameLine();
        const char* preview = "None"; for (const auto& key : Keys) if (key.Key == settings.ReloadHotkeyKey) { preview = key.Name; break; }
        ImGui::SetNextItemWidth(100.0f); if (ImGui::BeginCombo("Key##jsReload", preview)) { for (const auto& key : Keys) { const bool selected = key.Key == settings.ReloadHotkeyKey; if (ImGui::Selectable(key.Name, selected)) { settings.ReloadHotkeyKey = key.Key; changed = true; } if (selected) ImGui::SetItemDefaultFocus(); } ImGui::EndCombo(); }
        ImGui::SameLine(); ImGui::TextDisabled("evdev globally; GLFW fallback"); if (changed) javascript.markChanged();

        if (ImGui::CollapsingHeader("Runtime API", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BulletText("q.process.list/find/alive/modules/regions");
            ImGui::BulletText("q.memory.read/write/readBytes/writeBytes");
            ImGui::BulletText("q.signature.find  |  q.disassembly.decode");
            ImGui::BulletText("q.breakpoint.arm/hit/running/cancel");
            ImGui::BulletText("q.input.keyDown/shortcut/capsLock/scrollLock");
            ImGui::BulletText("q.events.subscribe/unsubscribe/emit");
            ImGui::BulletText("q.runtime.*  |  q.state (context lifetime)  |  q.storage (persistent JSON)");
            ImGui::BulletText("q.import(path)  |  q.loop(count, callback)  |  q.log(...)");
            ImGui::TextDisabled("Built-in events: tick, shader.changed, key.down, key.up, key.changed, lock.changed, process.started, process.stopped, breakpoint.hit, script.loaded, script.reload.");
        }

        ImGui::SeparatorText("Scripts"); std::optional<std::size_t> erase;
        for (std::size_t i = 0; i < javascript.scripts().size(); ++i)
        {
            auto& script = javascript.scripts()[i]; ImGui::PushID(static_cast<int>(script.Id & 0x7fffffffULL)); const std::string header = std::string(script.Name) + (script.Enabled ? "" : "  DISABLED") + "###RuntimeScript" + std::to_string(script.Id);
            if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                bool localChanged = false; localChanged |= ImGui::Checkbox("Enabled", &script.Enabled); ImGui::SameLine(); ImGui::SetNextItemWidth(240.0f); localChanged |= ImGui::InputText("Name", script.Name, sizeof(script.Name)); ImGui::SameLine(); localChanged |= ImGui::Checkbox("External", &script.External); ImGui::SameLine();
                if (ImGui::SmallButton("Reload")) { runtimeResetWorkspaceScript(script.Id); ++script.ReloadCount; }
                ImGui::SameLine(); if (ImGui::SmallButton("Reset persistent storage")) { script.PersistentStateJson = "{}"; runtimeResetWorkspaceScript(script.Id); ++script.ReloadCount; localChanged = true; }
                ImGui::SameLine(); if (ImGui::SmallButton("Remove")) erase = i;
                ImGui::SetNextItemWidth(160.0f); localChanged |= ImGui::DragFloat("Update Hz", &script.UpdateHz, 0.5f, 0.5f, 500.0f, "%.1f"); ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f); localChanged |= ImGui::DragFloat("Timeout", &script.TimeoutMs, 0.1f, 0.1f, 100.0f, "%.1f ms"); ImGui::SameLine(); localChanged |= ImGui::Checkbox("Hot reload##script", &script.HotReload);
                ImGui::SetNextItemWidth(180.0f); localChanged |= ImGui::InputText("Group", script.Group, sizeof(script.Group)); ImGui::SameLine(); ImGui::SetNextItemWidth(80.0f); localChanged |= ImGui::InputInt("Order", &script.Order);
                const bool oldLegacyBridge = script.LegacyBridge; ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.67f, 0.28f, 1.0f)); localChanged |= ImGui::Checkbox("Legacy bindings/controls/value-bank bridge (deprecated)", &script.LegacyBridge); ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off by default. When enabled, q.legacy.* plus deprecated q.binding/q.bank/q.control/q.graph compatibility aliases are installed for this script.");
                if (oldLegacyBridge != script.LegacyBridge) { runtimeResetWorkspaceScript(script.Id); ++script.ReloadCount; }
                if (script.External)
                {
                    char path[1024]{}; std::snprintf(path, sizeof(path), "%s", script.Path.c_str()); ImGui::SetNextItemWidth(-1.0f); if (ImGui::InputText("Path", path, sizeof(path))) { script.Path = path; localChanged = true; }
                    ImGui::TextDisabled("Relative paths resolve under %s", runtimeQuickJSScriptDirectory().string().c_str());
                }
                else
                {
                    auto& editorState = editor(script); editorState.Editor.Render("##JavaScriptEditor", ImVec2(-1.0f, 360.0f)); const std::string edited = editorState.Editor.GetText();
                    if (edited != editorState.Synced) { script.Source = edited; editorState.Synced = script.Source; runtimeResetWorkspaceScript(script.Id); localChanged = true; }
                }
                ImGui::Text("Runs %llu  compiles %llu  reloads %llu  timeouts %llu  last %.3f ms", static_cast<unsigned long long>(script.RunCount), static_cast<unsigned long long>(script.CompileCount), static_cast<unsigned long long>(script.ReloadCount), static_cast<unsigned long long>(script.TimeoutCount), script.LastMilliseconds);
                if (!script.Status.empty()) ImGui::TextWrapped("Status: %s", script.Status.c_str()); if (!script.LastLog.empty()) ImGui::TextWrapped("Log: %s", script.LastLog.c_str());
                if (!script.Dependencies.empty() && ImGui::TreeNode("Imported dependencies")) { for (const auto& dependency : script.Dependencies) ImGui::BulletText("%s", dependency.c_str()); ImGui::TreePop(); }
                if (localChanged) javascript.markChanged();
            }
            ImGui::Separator(); ImGui::PopID();
        }
        if (erase) javascript.erase(*erase, context.runtimeBindings);

        ImGui::SeparatorText("Deprecated compatibility");
        ImGui::TextDisabled("The old runtime graph is intentionally no longer the JavaScript execution model. Enable the per-script legacy bridge only for old configs/scripts that still need it.");
        if (ImGui::Button("Bindings (deprecated)")) manager.open("bindings"); ImGui::SameLine(); if (ImGui::Button("Controls (deprecated)")) manager.open("controls"); ImGui::SameLine(); if (ImGui::Button("Value Bank (deprecated)")) manager.open("value-bank"); ImGui::SameLine(); if (ImGui::Button("Profiles")) manager.open("profiles");
        javascript.saveIfChanged();
    }
}
