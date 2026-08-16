#include "quartz/client/ui/pages/JavaScriptPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/runtime/JavaScriptRuntime.hpp"
#include "quartz/client/runtime/QuickJS.hpp"
#include <TextEditor.h>
#include <cmath>
#include <fstream>
#include <memory>
#include <unordered_map>

namespace quartz::client::ui
{
    namespace
    {
        const TextEditor::Language* typescriptLanguage()
        {
            static const TextEditor::Language language = []
            {
                TextEditor::Language value;
                value.name = "TypeScript";
                value.singleLineComment = "//";
                value.commentStart = "/*";
                value.commentEnd = "*/";
                value.hasSingleQuotedStrings = true;
                value.hasDoubleQuotedStrings = true;
                value.otherStringStart = "`";
                value.otherStringEnd = "`";
                value.stringEscape = '\\';
                value.keywords = {"abstract","any","as","asserts","async","await","bigint","boolean","break","case","catch","class","const","constructor","continue","debugger","declare","default","delete","do","else","export","extends","finally","for","from","function","get","if","implements","import","in","infer","instanceof","interface","is","keyof","let","module","namespace","never","new","number","object","of","override","private","protected","public","readonly","require","return","satisfies","set","static","string","super","switch","symbol","this","throw","try","type","typeof","unknown","var","void","while","with","yield"};
                value.declarations = {"true","false","null","undefined"};
                value.identifiers = {"Math","JSON","BigInt","Number","String","Boolean","Array","Object","Map","Set","WeakMap","WeakSet","Date","RegExp","Promise","Error","TypeError","NaN","Infinity","Process","Signature","Breakpoint","Struct","Field","Pointer","Property","Runtime","Script","System","Disassembly","Memory","Keyboard","Events","console"};
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

        TextEditor::Palette typescriptPalette()
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

        struct EditorState { TextEditor Editor; std::string Synced; std::filesystem::path Path; std::filesystem::file_time_type Time{}; std::string Error; bool Initialized = false; };
        EditorState& editor(RuntimeScript& script)
        {
            static std::unordered_map<std::uint64_t, std::unique_ptr<EditorState>> editors;
            auto [it, inserted] = editors.try_emplace(script.Id); if (inserted) it->second = std::make_unique<EditorState>(); auto& state = *it->second;
            if (!state.Initialized)
            {
                state.Editor.SetLanguage(typescriptLanguage()); state.Editor.SetPalette(typescriptPalette()); state.Editor.SetTabSize(4); state.Editor.SetInsertSpacesOnTabs(true); state.Editor.SetAutoIndentEnabled(true); state.Editor.SetShowLineNumbersEnabled(true); state.Editor.SetShowMatchingBrackets(true); state.Editor.SetShowMiniMapEnabled(true); state.Editor.SetReadOnlyEnabled(false); state.Editor.SetText(script.Source); state.Synced = script.Source; state.Initialized = true;
            }
            else if (state.Synced != script.Source) { state.Editor.SetText(script.Source); state.Synced = script.Source; }
            return state;
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
        {
            std::filesystem::path path = script.Path;
            std::error_code ec;
            if (path.empty() || std::filesystem::is_directory(path, ec)) path = runtimeQuickJSScriptDirectory() / ("script-" + std::to_string(script.Id) + ".js");
            if (path.is_relative()) path = runtimeQuickJSScriptDirectory() / path;
            std::filesystem::create_directories(path.parent_path(), ec); ec.clear();
            if (!std::filesystem::exists(path, ec))
            {
                try { std::ofstream file(path, std::ios::binary | std::ios::trunc); if (!file) { error = "could not create " + path.string(); return false; } file.write(script.Source.data(), static_cast<std::streamsize>(script.Source.size())); if (!file) { error = "could not write " + path.string(); return false; } }
                catch (const std::exception& exception) { error = "could not create external script: " + std::string(exception.what()); return false; }
            }
            if (!std::filesystem::is_regular_file(path, ec)) { error = "external script path is not a regular file: " + path.string(); return false; }
            script.Path = path.string(); error.clear(); return true;
        }

        void drawRuntimeIndicator(const RuntimeScript& script)
        {
            const float pulse = 0.62f + 0.38f * static_cast<float>((std::sin(ImGui::GetTime() * 3.6) + 1.0) * 0.5);
            const bool running = script.Enabled && script.Status.starts_with("running"); const bool failed = script.Enabled && !script.Status.empty() && !running && script.Status != "disabled";
            const ImVec4 color = running ? ImVec4(0.18f, 0.86f, 0.95f, pulse) : failed ? ImVec4(0.95f, 0.30f, 0.28f, 0.92f) : ImVec4(0.42f, 0.45f, 0.50f, 0.75f);
            const ImVec2 position = ImGui::GetCursorScreenPos(); const float side = ImGui::GetTextLineHeight() * 0.72f; ImGui::Dummy(ImVec2(side, side)); ImGui::GetWindowDrawList()->AddRectFilled(position, ImVec2(position.x + side, position.y + side), ImGui::ColorConvertFloat4ToU32(color), 2.0f);
            ImGui::SameLine(); ImGui::TextUnformatted(running ? "running" : failed ? "error / waiting" : script.Enabled ? "waiting" : "disabled");
        }

        const ImVec4& consoleColor(const RuntimeScriptLogLevel level)
        {
            static const ImVec4 Debug{0.55f,0.58f,0.64f,1.0f}, Info{0.82f,0.84f,0.88f,1.0f}, Warning{0.95f,0.70f,0.28f,1.0f}, Error{0.96f,0.35f,0.32f,1.0f};
            switch (level) { case RuntimeScriptLogLevel::Debug: return Debug; case RuntimeScriptLogLevel::Warning: return Warning; case RuntimeScriptLogLevel::Error: return Error; default: return Info; }
        }

        struct KeyOption { const char* Name; int Key; };
        static constexpr KeyOption Keys[] = {{"None",0},{"F1",GLFW_KEY_F1},{"F2",GLFW_KEY_F2},{"F3",GLFW_KEY_F3},{"F4",GLFW_KEY_F4},{"F5",GLFW_KEY_F5},{"F6",GLFW_KEY_F6},{"F7",GLFW_KEY_F7},{"F8",GLFW_KEY_F8},{"F9",GLFW_KEY_F9},{"F10",GLFW_KEY_F10},{"F11",GLFW_KEY_F11},{"F12",GLFW_KEY_F12},{"A",GLFW_KEY_A},{"B",GLFW_KEY_B},{"C",GLFW_KEY_C},{"D",GLFW_KEY_D},{"E",GLFW_KEY_E},{"F",GLFW_KEY_F},{"G",GLFW_KEY_G},{"H",GLFW_KEY_H},{"I",GLFW_KEY_I},{"J",GLFW_KEY_J},{"K",GLFW_KEY_K},{"L",GLFW_KEY_L},{"M",GLFW_KEY_M},{"N",GLFW_KEY_N},{"O",GLFW_KEY_O},{"P",GLFW_KEY_P},{"Q",GLFW_KEY_Q},{"R",GLFW_KEY_R},{"S",GLFW_KEY_S},{"T",GLFW_KEY_T},{"U",GLFW_KEY_U},{"V",GLFW_KEY_V},{"W",GLFW_KEY_W},{"X",GLFW_KEY_X},{"Y",GLFW_KEY_Y},{"Z",GLFW_KEY_Z},{"0",GLFW_KEY_0},{"1",GLFW_KEY_1},{"2",GLFW_KEY_2},{"3",GLFW_KEY_3},{"4",GLFW_KEY_4},{"5",GLFW_KEY_5},{"6",GLFW_KEY_6},{"7",GLFW_KEY_7},{"8",GLFW_KEY_8},{"9",GLFW_KEY_9}};
    }

    void JavaScriptPage::render(PageContext& context, PageManager&)
    {
        auto& javascript = context.javascript; auto& settings = javascript.settings(); static std::string status;
        ImGui::TextWrapped("Quartz scripts are moving to a TypeScript-first SDK. External TypeScript and JavaScript sources stay visible here alongside runtime state, diagnostics and console output.");

        if (ImGui::Button("+ Inline script")) { auto& script = javascript.add(); script.Source = "// Quartz script\n"; }
        ImGui::SameLine(); if (ImGui::Button("+ External script")) { auto& script = javascript.add(); script.External = true; script.Path = (runtimeQuickJSScriptDirectory() / "script.js").string(); }
        ImGui::SameLine(); if (ImGui::Button("Reload all")) { runtimeReloadAllWorkspaceScripts(); javascript.clearOutputs(); for (auto& script : javascript.scripts()) ++script.ReloadCount; status = "all script contexts reloaded"; }
        ImGui::SameLine(); if (ImGui::Button("Save @quartz/client types")) { std::string error; status = runtimeSaveQuickJSTypeDeclarations(error) ? "saved " + runtimeQuickJSTypeDeclarationsPath().string() : error; }
        ImGui::SameLine(); if (ImGui::Button("Save runtime")) status = javascript.save() ? "saved " + javascript.path().string() : "could not save JavaScript runtime";
        if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());
        ImGui::TextDisabled("Runtime: %s", javascript.path().string().c_str());
        ImGui::TextDisabled("External scripts: %s", runtimeQuickJSScriptDirectory().string().c_str());

        bool changed = false; changed |= ImGui::Checkbox("External script hot reload", &settings.ExternalHotReload); ImGui::SameLine(); ImGui::TextDisabled("watches root scripts and imported dependencies");
        ImGui::SeparatorText("Global reload hotkey");
        changed |= ImGui::Checkbox("Ctrl##jsReload", &settings.ReloadHotkeyCtrl); ImGui::SameLine(); changed |= ImGui::Checkbox("Alt##jsReload", &settings.ReloadHotkeyAlt); ImGui::SameLine(); changed |= ImGui::Checkbox("Shift##jsReload", &settings.ReloadHotkeyShift); ImGui::SameLine();
        const char* preview = "None"; for (const auto& key : Keys) if (key.Key == settings.ReloadHotkeyKey) { preview = key.Name; break; }
        ImGui::SetNextItemWidth(100.0f); if (ImGui::BeginCombo("Key##jsReload", preview)) { for (const auto& key : Keys) { const bool selected = key.Key == settings.ReloadHotkeyKey; if (ImGui::Selectable(key.Name, selected)) { settings.ReloadHotkeyKey = key.Key; changed = true; } if (selected) ImGui::SetItemDefaultFocus(); } ImGui::EndCombo(); }
        ImGui::SameLine(); ImGui::TextDisabled("evdev globally; GLFW fallback"); if (changed) javascript.markChanged();

        ImGui::SeparatorText("Scripts"); std::optional<std::size_t> erase;
        for (std::size_t i = 0; i < javascript.scripts().size(); ++i)
        {
            auto& script = javascript.scripts()[i]; ImGui::PushID(static_cast<int>(script.Id & 0x7fffffffULL)); const std::string header = std::string(script.Name) + (script.Enabled ? "" : "  DISABLED") + "###RuntimeScript" + std::to_string(script.Id);
            if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                bool localChanged = false; localChanged |= ImGui::Checkbox("Enabled", &script.Enabled); ImGui::SameLine(); ImGui::SetNextItemWidth(240.0f); localChanged |= ImGui::InputText("Name", script.Name, sizeof(script.Name)); ImGui::SameLine();
                const bool wasExternal = script.External;
                if (ImGui::Checkbox("External", &script.External))
                {
                    localChanged = true;
                    if (!wasExternal && script.External)
                    {
                        std::string conversionError;
                        if (!materializeExternal(script, conversionError)) { script.External = false; status = conversionError; }
                        else { runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); ++script.ReloadCount; status = "external script: " + script.Path; }
                    }
                    else if (wasExternal && !script.External) { runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); ++script.ReloadCount; }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Reload")) { runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); ++script.ReloadCount; }
                ImGui::SameLine(); if (ImGui::SmallButton("Reset persistent storage")) { script.PersistentStateJson = "{}"; runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); ++script.ReloadCount; localChanged = true; }
                ImGui::SameLine(); if (ImGui::SmallButton("Remove")) erase = i;
                ImGui::SetNextItemWidth(160.0f); localChanged |= ImGui::DragFloat("Update Hz", &script.UpdateHz, 0.5f, 0.5f, 500.0f, "%.1f"); ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f); localChanged |= ImGui::DragFloat("Timeout", &script.TimeoutMs, 0.1f, 0.1f, 100.0f, "%.1f ms"); ImGui::SameLine(); localChanged |= ImGui::Checkbox("Hot reload##script", &script.HotReload);
                ImGui::SetNextItemWidth(180.0f); localChanged |= ImGui::InputText("Group", script.Group, sizeof(script.Group)); ImGui::SameLine(); ImGui::SetNextItemWidth(80.0f); localChanged |= ImGui::InputInt("Order", &script.Order);
                if (ImGui::BeginTabBar("##JavaScriptRuntimeTabs"))
                {
                    if (ImGui::BeginTabItem("Editor"))
                    {
                        if (script.External)
                        {
                            char path[1024]{}; std::snprintf(path, sizeof(path), "%s", script.Path.c_str()); ImGui::SetNextItemWidth(-1.0f); if (ImGui::InputText("Path", path, sizeof(path))) { script.Path = path; runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); localChanged = true; }
                            std::filesystem::path resolved = script.Path; if (resolved.is_relative()) resolved = runtimeQuickJSScriptDirectory() / resolved; std::error_code ec; const bool regular = std::filesystem::is_regular_file(resolved, ec);
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
                        }
                        else
                        {
                            auto& editorState = editor(script); editorState.Editor.Render("##JavaScriptEditor", ImVec2(-1.0f, 360.0f)); const std::string edited = editorState.Editor.GetText();
                            if (edited != editorState.Synced) { script.Source = edited; editorState.Synced = script.Source; runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); localChanged = true; }
                        }
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("State"))
                    {
                        drawRuntimeIndicator(script);
                        ImGui::Text("Runs %llu  compiles %llu  reloads %llu  timeouts %llu  last %.3f ms", static_cast<unsigned long long>(script.RunCount), static_cast<unsigned long long>(script.CompileCount), static_cast<unsigned long long>(script.ReloadCount), static_cast<unsigned long long>(script.TimeoutCount), script.LastMilliseconds);
                        if (!script.Status.empty()) ImGui::TextWrapped("Status: %s", script.Status.c_str());
                        if (!script.SignatureScans.empty())
                        {
                            ImGui::SeparatorText("Async signature scans");
                            for (const auto& scan : script.SignatureScans)
                            {
                                const bool running = !scan.Finished; const float pulse = running ? 0.55f + 0.45f * static_cast<float>((std::sin(ImGui::GetTime() * 4.0) + 1.0) * 0.5) : 1.0f;
                                ImGui::PushStyleColor(ImGuiCol_Text, running ? ImVec4(0.18f,0.86f,0.95f,pulse) : scan.Found ? ImVec4(0.45f,0.90f,0.62f,1.0f) : scan.Status == "error" ? ImVec4(0.96f,0.35f,0.32f,1.0f) : ImVec4(0.72f,0.74f,0.78f,1.0f));
                                ImGui::Text("#%llu  PID %d  %s", static_cast<unsigned long long>(scan.Id), scan.Pid, scan.Status.c_str()); ImGui::PopStyleColor();
                                if (running) ImGui::ProgressBar(scan.Progress, ImVec2(-1.0f, 0.0f));
                                ImGui::TextDisabled("%.1f MiB/s | %llu / %llu bytes%s", scan.AverageMiBs, static_cast<unsigned long long>(scan.ScannedBytes), static_cast<unsigned long long>(scan.TotalBytes), scan.Found ? (" | 0x" + [] (std::uintptr_t value) { char buffer[32]; std::snprintf(buffer, sizeof(buffer), "%llX", static_cast<unsigned long long>(value)); return std::string(buffer); }(scan.MatchAddress)).c_str() : "");
                                if (!scan.Error.empty()) ImGui::TextWrapped("%s", scan.Error.c_str());
                            }
                        }
                        ImGui::SeparatorText("Script.state"); ImGui::BeginChild("##jsState", ImVec2(0.0f, 150.0f), true, ImGuiWindowFlags_HorizontalScrollbar); ImGui::TextUnformatted(script.StateSnapshot.empty() ? "{}" : script.StateSnapshot.c_str()); ImGui::EndChild();
                        ImGui::SeparatorText("Script.storage"); ImGui::BeginChild("##jsStorage", ImVec2(0.0f, 120.0f), true, ImGuiWindowFlags_HorizontalScrollbar); ImGui::TextUnformatted(script.PersistentStateJson.empty() ? "{}" : script.PersistentStateJson.c_str()); ImGui::EndChild();
                        if (!script.Dependencies.empty() && ImGui::TreeNode("Imported dependencies")) { for (const auto& dependency : script.Dependencies) ImGui::BulletText("%s", dependency.c_str()); ImGui::TreePop(); }
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Console"))
                    {
                        static std::unordered_map<std::uint64_t, bool> autoScroll; auto [followIt, inserted] = autoScroll.try_emplace(script.Id, true); bool& follow = followIt->second;
                        if (ImGui::SmallButton("Clear")) script.Console.clear(); ImGui::SameLine(); ImGui::Checkbox("Auto-scroll", &follow); ImGui::SameLine(); ImGui::TextDisabled("%zu / 512", script.Console.size());
                        ImGui::BeginChild("##jsConsole", ImVec2(0.0f, 260.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
                        for (const auto& entry : script.Console) { ImGui::TextColored(consoleColor(entry.Level), "[%8.3f]", entry.Time); ImGui::SameLine(); ImGui::TextUnformatted(entry.Text.c_str()); }
                        if (follow && ImGui::GetScrollMaxY() > 0.0f) ImGui::SetScrollY(ImGui::GetScrollMaxY());
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                if (localChanged) javascript.markChanged();
            }
            ImGui::Separator(); ImGui::PopID();
        }
        if (erase) javascript.erase(*erase, context.runtimeBindings);

        javascript.saveIfChanged();
    }
}
