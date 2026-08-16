#include "quartz/client/ui/pages/JavaScriptPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/TextEditorSupport.hpp"
#include "quartz/client/runtime/JavaScriptRuntime.hpp"
#include "quartz/client/runtime/QuickJS.hpp"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <unordered_map>

namespace quartz::client::ui
{
    namespace
    {
        struct EditorState
        {
            TextEditor Editor;
            std::string Synced;
            std::filesystem::path Path;
            std::filesystem::file_time_type Time{};
            std::string Error;
            bool Initialized = false;
        };

        struct ReadOnlyViewState
        {
            TextEditor Editor;
            std::string Synced;
            bool Initialized = false;
        };

        EditorState& editor(RuntimeScript& script)
        {
            static std::unordered_map<std::uint64_t, std::unique_ptr<EditorState>> editors; auto [it, inserted] = editors.try_emplace(script.Id); if (inserted) it->second = std::make_unique<EditorState>(); auto& state = *it->second;
            if (!state.Initialized) { configureQuartzTypeScriptEditor(state.Editor, false, true); state.Editor.SetText(script.Source); state.Synced = script.Source; state.Initialized = true; }
            else if (state.Synced != script.Source) { state.Editor.SetText(script.Source); state.Synced = script.Source; }
            applyQuartzTextEditorPalette(state.Editor); return state;
        }

        bool readExternalSource(const std::filesystem::path& path, std::string& source, std::string& error) noexcept
        {
            std::error_code ec; if (!std::filesystem::is_regular_file(path, ec)) { error = ec ? ec.message() : "not a regular file"; return false; }
            try { std::ifstream file(path, std::ios::binary); if (!file) { error = "could not open file"; return false; } source.assign(std::istreambuf_iterator<char>(file), {}); if (!file && !file.eof()) { error = "could not read file"; return false; } error.clear(); return true; }
            catch (const std::exception& exception) { error = exception.what(); return false; } catch (...) { error = "unknown filesystem error"; return false; }
        }

        EditorState& externalEditor(RuntimeScript& script, const std::filesystem::path& path)
        {
            static std::unordered_map<std::uint64_t, std::unique_ptr<EditorState>> editors; auto [it, inserted] = editors.try_emplace(script.Id); if (inserted) it->second = std::make_unique<EditorState>(); auto& state = *it->second;
            std::error_code ec; const auto time = std::filesystem::last_write_time(path, ec); const bool changed = !state.Initialized || state.Path != path || (!ec && state.Time != time);
            if (!state.Initialized) { configureQuartzTypeScriptEditor(state.Editor, true, true); state.Initialized = true; }
            applyQuartzTextEditorPalette(state.Editor);
            if (changed)
            {
                std::string source; if (readExternalSource(path, source, state.Error)) { state.Editor.SetText(source); state.Synced = std::move(source); state.Path = path; state.Time = time; }
                else if (state.Path != path) { state.Editor.ClearText(); state.Synced.clear(); state.Path = path; state.Time = {}; }
            }
            return state;
        }

        ReadOnlyViewState& readOnlyView(const std::uint64_t scriptId, const std::uint32_t kind, const std::string_view text, const bool lineNumbers = false)
        {
            static std::map<std::pair<std::uint64_t,std::uint32_t>, std::unique_ptr<ReadOnlyViewState>> views; auto [it, inserted] = views.try_emplace({scriptId,kind}); if (inserted) it->second = std::make_unique<ReadOnlyViewState>(); auto& state = *it->second;
            if (!state.Initialized) { configureQuartzReadOnlyTextEditor(state.Editor, lineNumbers); state.Initialized = true; } applyQuartzTextEditorPalette(state.Editor);
            if (state.Synced != text) { state.Editor.SetText(text); state.Synced = text; } return state;
        }

        bool materializeExternal(RuntimeScript& script, std::string& error)
        {
            std::filesystem::path path = script.Path; std::error_code ec; if (path.empty() || std::filesystem::is_directory(path, ec)) path = runtimeQuickJSScriptDirectory() / ("script-" + std::to_string(script.Id) + ".ts"); if (path.is_relative()) path = runtimeQuickJSScriptDirectory() / path; std::filesystem::create_directories(path.parent_path(), ec);
            if (!std::filesystem::exists(path, ec))
            {
                try { std::ofstream file(path, std::ios::binary | std::ios::trunc); if (!file) { error = "could not create " + path.string(); return false; } file.write(script.Source.data(), static_cast<std::streamsize>(script.Source.size())); if (!file) { error = "could not write " + path.string(); return false; } }
                catch (const std::exception& exception) { error = "could not create external script: " + std::string(exception.what()); return false; }
            }
            if (!std::filesystem::is_regular_file(path, ec)) { error = "external script path is not a regular file: " + path.string(); return false; } script.Path = path.string(); error.clear(); return true;
        }

        bool drawScriptProperty(RuntimeScriptProperty& property)
        {
            const char* label = property.Label.empty() ? property.Id.c_str() : property.Label.c_str(); bool changed = false; ImGui::PushID(property.Id.c_str());
            switch (property.Type)
            {
            case RuntimeScriptPropertyType::Boolean: changed = ImGui::Checkbox(label, &property.BoolValue); break;
            case RuntimeScriptPropertyType::Int32: case RuntimeScriptPropertyType::UInt32: case RuntimeScriptPropertyType::Float32: case RuntimeScriptPropertyType::Float64:
            {
                double value = property.NumberValue; const double* min = property.HasMin ? &property.Min : nullptr; const double* max = property.HasMax ? &property.Max : nullptr; const char* format = property.Type == RuntimeScriptPropertyType::Int32 || property.Type == RuntimeScriptPropertyType::UInt32 ? "%.0f" : "%.3f";
                ImGui::SetNextItemWidth(260.0f); if (ImGui::DragScalar(label, ImGuiDataType_Double, &value, static_cast<float>(property.Step), min, max, format)) { if (property.Type == RuntimeScriptPropertyType::Int32 || property.Type == RuntimeScriptPropertyType::UInt32) value = std::round(value); if (property.Type == RuntimeScriptPropertyType::UInt32) value = std::max(value,0.0); property.NumberValue = value; changed = true; } break;
            }
            case RuntimeScriptPropertyType::Enum: if (ImGui::BeginCombo(label, property.StringValue.c_str())) { for (const auto& value : property.EnumValues) { const bool selected = property.StringValue == value; if (ImGui::Selectable(value.c_str(), selected)) { property.StringValue = value; changed = true; } if (selected) ImGui::SetItemDefaultFocus(); } ImGui::EndCombo(); } break;
            default:
            {
                char value[1024]{}; std::snprintf(value,sizeof(value),"%s",property.StringValue.c_str()); const float available = ImGui::GetContentRegionAvail().x; const float reserve = ImGui::CalcTextSize(label).x + ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x * 3.0f + 36.0f; ImGui::SetNextItemWidth(std::clamp(available - reserve,220.0f,760.0f)); if (ImGui::InputText(label,value,sizeof(value))) { property.StringValue = value; property.KeyIsNumber = false; changed = true; } break;
            }
            }
            if (!property.Description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s",property.Description.c_str()); ImGui::SameLine(); if (ImGui::SmallButton("Reset")) { property.StringValue = property.DefaultString; property.NumberValue = property.DefaultNumber; property.BoolValue = property.DefaultBool; property.KeyIsNumber = property.DefaultKeyIsNumber; changed = true; } if (changed) ++property.Revision; ImGui::PopID(); return changed;
        }

        bool drawScriptProperties(RuntimeScript& script)
        {
            if (script.Properties.empty()) return false; bool changed = false; std::string group; ImGui::SeparatorText("Properties"); for (auto& property : script.Properties) { if (property.Group != group) { group = property.Group; if (!group.empty()) ImGui::TextDisabled("%s",group.c_str()); } changed |= drawScriptProperty(property); } return changed;
        }

        void drawRuntimeIndicator(const RuntimeScript& script)
        {
            const float pulse = 0.62f + 0.38f * static_cast<float>((std::sin(ImGui::GetTime() * 3.6) + 1.0) * 0.5); const bool running = script.Enabled && script.Status.starts_with("running"), failed = script.Enabled && !script.Status.empty() && !running && script.Status != "disabled"; const ImVec4 color = running ? ImVec4(0.18f,0.86f,0.95f,pulse) : failed ? ImVec4(0.95f,0.30f,0.28f,0.92f) : ImVec4(0.42f,0.45f,0.50f,0.75f); const float rowHeight = ImGui::GetTextLineHeight(), side = rowHeight * 0.72f; ImGui::Dummy(ImVec2(side,rowHeight)); const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax(); const float y = min.y + (max.y - min.y - side) * 0.5f; ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x,y),ImVec2(min.x+side,y+side),ImGui::ColorConvertFloat4ToU32(color),2.0f); ImGui::SameLine(); ImGui::TextUnformatted(running ? "running" : failed ? "error / waiting" : script.Enabled ? "waiting" : "disabled");
        }

        std::string consoleText(const RuntimeScript& script)
        {
            std::ostringstream out; for (const auto& entry : script.Console) { out << '[' << std::fixed << std::setprecision(3) << std::setw(8) << entry.Time << "] "; switch (entry.Level) { case RuntimeScriptLogLevel::Debug: out << "DEBUG "; break; case RuntimeScriptLogLevel::Warning: out << "WARN  "; break; case RuntimeScriptLogLevel::Error: out << "ERROR "; break; default: out << "INFO  "; break; } out << entry.Text << '\n'; } return out.str();
        }

        struct KeyOption { const char* Name; int Key; };
        static constexpr KeyOption Keys[] = {{"None",0},{"F1",GLFW_KEY_F1},{"F2",GLFW_KEY_F2},{"F3",GLFW_KEY_F3},{"F4",GLFW_KEY_F4},{"F5",GLFW_KEY_F5},{"F6",GLFW_KEY_F6},{"F7",GLFW_KEY_F7},{"F8",GLFW_KEY_F8},{"F9",GLFW_KEY_F9},{"F10",GLFW_KEY_F10},{"F11",GLFW_KEY_F11},{"F12",GLFW_KEY_F12},{"A",GLFW_KEY_A},{"B",GLFW_KEY_B},{"C",GLFW_KEY_C},{"D",GLFW_KEY_D},{"E",GLFW_KEY_E},{"F",GLFW_KEY_F},{"G",GLFW_KEY_G},{"H",GLFW_KEY_H},{"I",GLFW_KEY_I},{"J",GLFW_KEY_J},{"K",GLFW_KEY_K},{"L",GLFW_KEY_L},{"M",GLFW_KEY_M},{"N",GLFW_KEY_N},{"O",GLFW_KEY_O},{"P",GLFW_KEY_P},{"Q",GLFW_KEY_Q},{"R",GLFW_KEY_R},{"S",GLFW_KEY_S},{"T",GLFW_KEY_T},{"U",GLFW_KEY_U},{"V",GLFW_KEY_V},{"W",GLFW_KEY_W},{"X",GLFW_KEY_X},{"Y",GLFW_KEY_Y},{"Z",GLFW_KEY_Z},{"0",GLFW_KEY_0},{"1",GLFW_KEY_1},{"2",GLFW_KEY_2},{"3",GLFW_KEY_3},{"4",GLFW_KEY_4},{"5",GLFW_KEY_5},{"6",GLFW_KEY_6},{"7",GLFW_KEY_7},{"8",GLFW_KEY_8},{"9",GLFW_KEY_9}};
    }

    void JavaScriptPage::render(PageContext& context, PageManager&)
    {
        auto& javascript = context.javascript; auto& settings = javascript.settings(); static std::string status; ImGui::TextWrapped("Quartz scripts are moving to a TypeScript-first SDK. External TypeScript and JavaScript sources stay visible here alongside runtime state, diagnostics and console output.");
        if (ImGui::Button("+ Inline script")) { auto& script = javascript.add(); script.Source = "// Quartz script\n"; } ImGui::SameLine(); if (ImGui::Button("+ External script")) { auto& script = javascript.add(); script.External = true; script.Path = (runtimeQuickJSScriptDirectory() / "script.ts").string(); } ImGui::SameLine(); if (ImGui::Button("Reload all")) { runtimeReloadAllWorkspaceScripts(); javascript.clearOutputs(); for (auto& script : javascript.scripts()) ++script.ReloadCount; status = "all script contexts reloaded"; } ImGui::SameLine(); if (ImGui::Button("Save @quartz/client types")) { std::string error; status = runtimeSaveQuickJSTypeDeclarations(error) ? "saved " + runtimeQuickJSTypeDeclarationsPath().string() : error; } ImGui::SameLine(); if (ImGui::Button("Save runtime")) status = javascript.save() ? "saved " + javascript.path().string() : "could not save JavaScript runtime";
        if (!status.empty()) ImGui::TextDisabled("%s",status.c_str()); ImGui::TextDisabled("Runtime: %s",javascript.path().string().c_str()); ImGui::TextDisabled("External scripts: %s",runtimeQuickJSScriptDirectory().string().c_str());
        bool changed = false; changed |= ImGui::Checkbox("External script hot reload",&settings.ExternalHotReload); ImGui::SameLine(); ImGui::TextDisabled("watches root scripts and imported dependencies"); ImGui::SeparatorText("Global reload hotkey"); changed |= ImGui::Checkbox("Ctrl##jsReload",&settings.ReloadHotkeyCtrl); ImGui::SameLine(); changed |= ImGui::Checkbox("Alt##jsReload",&settings.ReloadHotkeyAlt); ImGui::SameLine(); changed |= ImGui::Checkbox("Shift##jsReload",&settings.ReloadHotkeyShift); ImGui::SameLine(); const char* preview = "None"; for (const auto& key : Keys) if (key.Key == settings.ReloadHotkeyKey) { preview = key.Name; break; } ImGui::SetNextItemWidth(100.0f); if (ImGui::BeginCombo("Key##jsReload",preview)) { for (const auto& key : Keys) { const bool selected = key.Key == settings.ReloadHotkeyKey; if (ImGui::Selectable(key.Name,selected)) { settings.ReloadHotkeyKey = key.Key; changed = true; } if (selected) ImGui::SetItemDefaultFocus(); } ImGui::EndCombo(); } ImGui::SameLine(); ImGui::TextDisabled("evdev globally; GLFW fallback"); if (changed) javascript.markChanged();

        ImGui::SeparatorText("Scripts"); std::optional<std::size_t> erase;
        for (std::size_t i=0;i<javascript.scripts().size();++i)
        {
            auto& script=javascript.scripts()[i]; ImGui::PushID(static_cast<int>(script.Id & 0x7fffffffULL)); const std::string header=std::string(script.Name)+(script.Enabled?"":"  DISABLED")+"###RuntimeScript"+std::to_string(script.Id);
            if (ImGui::CollapsingHeader(header.c_str(),ImGuiTreeNodeFlags_DefaultOpen))
            {
                bool localChanged=false; localChanged|=ImGui::Checkbox("Enabled",&script.Enabled); ImGui::SameLine(); ImGui::SetNextItemWidth(240.0f); localChanged|=ImGui::InputText("Name",script.Name,sizeof(script.Name)); ImGui::SameLine(); const bool wasExternal=script.External;
                if (ImGui::Checkbox("External",&script.External)) { localChanged=true; if (!wasExternal&&script.External) { std::string conversionError; if (!materializeExternal(script,conversionError)) { script.External=false; status=conversionError; } else { runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); ++script.ReloadCount; status="external script: "+script.Path; } } else if (wasExternal&&!script.External) { runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); ++script.ReloadCount; } }
                ImGui::SameLine(); if (ImGui::SmallButton("Reload")) { runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); ++script.ReloadCount; } ImGui::SameLine(); if (ImGui::SmallButton("Reset persistent storage")) { script.PersistentStateJson="{}"; script.Properties.clear(); runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); ++script.ReloadCount; localChanged=true; } ImGui::SameLine(); if (ImGui::SmallButton("Remove")) erase=i;
                ImGui::SetNextItemWidth(160.0f); localChanged|=ImGui::DragFloat("Update Hz",&script.UpdateHz,0.5f,0.5f,500.0f,"%.1f"); ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f); localChanged|=ImGui::DragFloat("Timeout",&script.TimeoutMs,0.1f,0.1f,100.0f,"%.1f ms"); ImGui::SameLine(); localChanged|=ImGui::Checkbox("Hot reload##script",&script.HotReload); ImGui::SetNextItemWidth(180.0f); localChanged|=ImGui::InputText("Group",script.Group,sizeof(script.Group)); ImGui::SameLine(); ImGui::SetNextItemWidth(80.0f); localChanged|=ImGui::InputInt("Order",&script.Order); localChanged|=drawScriptProperties(script);
                if (ImGui::BeginTabBar("##JavaScriptRuntimeTabs"))
                {
                    if (ImGui::BeginTabItem("Editor"))
                    {
                        if (script.External)
                        {
                            char path[1024]{}; std::snprintf(path,sizeof(path),"%s",script.Path.c_str()); ImGui::SetNextItemWidth(-1.0f); if (ImGui::InputText("Path",path,sizeof(path))) { script.Path=path; runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); localChanged=true; } std::filesystem::path resolved=script.Path; if (resolved.is_relative()) resolved=runtimeQuickJSScriptDirectory()/resolved; std::error_code ec; const bool regular=std::filesystem::is_regular_file(resolved,ec); if (!regular) { ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0.96f,0.35f,0.32f,1.0f)); ImGui::TextWrapped("Invalid external script: %s",ec?ec.message().c_str():resolved.string().c_str()); ImGui::PopStyleColor(); } ImGui::TextDisabled("%s",resolved.string().c_str());
                            if (regular) { auto& external=externalEditor(script,resolved); ImGui::SameLine(); ImGui::TextDisabled("read-only live view — hover SDK symbols; selection/copy is enabled"); if (!external.Error.empty()) { ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0.96f,0.35f,0.32f,1.0f)); ImGui::TextWrapped("Could not refresh source view: %s",external.Error.c_str()); ImGui::PopStyleColor(); } external.Editor.Render("##ExternalScriptView",ImVec2(-1.0f,360.0f)); } ImGui::TextDisabled("Relative paths resolve under %s",runtimeQuickJSScriptDirectory().string().c_str());
                        }
                        else { auto& editorState=editor(script); editorState.Editor.Render("##JavaScriptEditor",ImVec2(-1.0f,360.0f)); const std::string edited=editorState.Editor.GetText(); if (edited!=editorState.Synced) { script.Source=edited; editorState.Synced=script.Source; runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); localChanged=true; } }
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("State"))
                    {
                        drawRuntimeIndicator(script); ImGui::Text("Runs %llu  compiles %llu  reloads %llu  timeouts %llu  last %.3f ms",static_cast<unsigned long long>(script.RunCount),static_cast<unsigned long long>(script.CompileCount),static_cast<unsigned long long>(script.ReloadCount),static_cast<unsigned long long>(script.TimeoutCount),script.LastMilliseconds); if (!script.Status.empty()) ImGui::TextWrapped("Status: %s",script.Status.c_str());
                        if (!script.SignatureScans.empty()) { ImGui::SeparatorText("Async signature scans"); for (const auto& scan:script.SignatureScans) { const bool running=!scan.Finished; const float pulse=running?0.55f+0.45f*static_cast<float>((std::sin(ImGui::GetTime()*4.0)+1.0)*0.5):1.0f; ImGui::PushStyleColor(ImGuiCol_Text,running?ImVec4(0.18f,0.86f,0.95f,pulse):scan.Found?ImVec4(0.45f,0.90f,0.62f,1.0f):scan.Status=="error"?ImVec4(0.96f,0.35f,0.32f,1.0f):ImVec4(0.72f,0.74f,0.78f,1.0f)); ImGui::Text("#%llu  PID %d  %s",static_cast<unsigned long long>(scan.Id),scan.Pid,scan.Status.c_str()); ImGui::PopStyleColor(); if (running) ImGui::ProgressBar(scan.Progress,ImVec2(-1.0f,0.0f)); ImGui::TextDisabled("%.1f MiB/s | %llu / %llu bytes",scan.AverageMiBs,static_cast<unsigned long long>(scan.ScannedBytes),static_cast<unsigned long long>(scan.TotalBytes)); if (!scan.Error.empty()) ImGui::TextWrapped("%s",scan.Error.c_str()); } }
                        ImGui::SeparatorText("Script.state"); auto& stateView=readOnlyView(script.Id,1,script.StateSnapshot.empty()?std::string_view("{}"):std::string_view(script.StateSnapshot),true); stateView.Editor.Render("##jsState",ImVec2(0.0f,150.0f),ImGuiChildFlags_Borders); ImGui::SeparatorText("Script.storage"); auto& storageView=readOnlyView(script.Id,2,script.PersistentStateJson.empty()?std::string_view("{}"):std::string_view(script.PersistentStateJson),true); storageView.Editor.Render("##jsStorage",ImVec2(0.0f,120.0f),ImGuiChildFlags_Borders); if (!script.Dependencies.empty()&&ImGui::TreeNode("Imported dependencies")) { for (const auto& dependency:script.Dependencies) ImGui::BulletText("%s",dependency.c_str()); ImGui::TreePop(); } ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Console"))
                    {
                        static std::unordered_map<std::uint64_t,bool> autoScroll; auto [followIt,inserted]=autoScroll.try_emplace(script.Id,true); bool& follow=followIt->second; if (ImGui::SmallButton("Clear")) script.Console.clear(); ImGui::SameLine(); ImGui::Checkbox("Auto-scroll",&follow); ImGui::SameLine(); if (ImGui::SmallButton("Copy all")) { const std::string text=consoleText(script); ImGui::SetClipboardText(text.c_str()); } ImGui::SameLine(); ImGui::TextDisabled("%zu / 512 — selection/copy is enabled",script.Console.size()); const std::string text=consoleText(script); auto& consoleView=readOnlyView(script.Id,3,text,false); if (follow&&consoleView.Editor.GetLineCount()) consoleView.Editor.ScrollToLine(consoleView.Editor.GetLineCount()-1,TextEditor::Scroll::alignBottom); consoleView.Editor.Render("##jsConsole",ImVec2(0.0f,260.0f),ImGuiChildFlags_Borders); ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                if (localChanged) javascript.markChanged();
            }
            ImGui::Separator(); ImGui::PopID();
        }
        if (erase) javascript.erase(*erase,context.runtimeBindings); javascript.saveIfChanged();
    }
}
