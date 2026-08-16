#include "quartz/client/ui/ObjectExperiments.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/runtime/StructExperiment.hpp"
#include "quartz/client/ui/AddressInput.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/ProcessPicker.hpp"
#include "quartz/client/ui/ReverseEngineeringNavigation.hpp"
#include "quartz/client/ui/TextEditorSupport.hpp"
#include "quartz/client/ui/pages/MemoryScannerPage.hpp"
#include "quartz/client/ui/pages/MemoryWatchPage.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace quartz::client::ui
{
    namespace
    {
        struct ExperimentRow
        {
            std::string Name;
            std::string Type;
            std::uintptr_t Offset = 0;
            std::uintptr_t Address = 0;
            std::uintptr_t PointerValue = 0;
            std::size_t Width = 0;
            std::optional<MemoryScanValueType> WatchType;
            std::string Value;
            std::string Raw;
            bool Readable = false;
        };

        struct FieldHelp
        {
            std::string_view Name;
            std::string_view Signature;
            std::string_view Description;
        };

        constexpr auto FieldHelpEntries = std::to_array<FieldHelp>({
            {"Int8","Field.Int8(offset: number | bigint)","Signed 8-bit integer."},{"UInt8","Field.UInt8(offset: number | bigint)","Unsigned 8-bit integer."},
            {"Int16","Field.Int16(offset: number | bigint)","Signed 16-bit integer."},{"UInt16","Field.UInt16(offset: number | bigint)","Unsigned 16-bit integer."},
            {"Int32","Field.Int32(offset: number | bigint)","Signed 32-bit integer."},{"UInt32","Field.UInt32(offset: number | bigint)","Unsigned 32-bit integer."},
            {"Int64","Field.Int64(offset: number | bigint)","Signed 64-bit integer."},{"UInt64","Field.UInt64(offset: number | bigint)","Unsigned 64-bit integer."},
            {"Float32","Field.Float32(offset: number | bigint)","32-bit IEEE floating-point value."},{"Float64","Field.Float64(offset: number | bigint)","64-bit IEEE floating-point value."},
            {"Boolean","Field.Boolean(offset: number | bigint)","One-byte boolean value."},
            {"Pointer","Field.Pointer(offset: number | bigint, type?: Struct)","Native-width pointer. Passing a Struct type makes Quartz expand the pointed object."},
            {"CString","Field.CString(offset: number | bigint, maxLength = 256)","Native-width pointer to a NUL-terminated UTF-8 C string."},
            {"WString","Field.WString(offset: number | bigint, maxLength = 256)","Native-width pointer to a NUL-terminated UTF-16 string."},
            {"Struct","Field.Struct(offset: number | bigint, type: Struct)","Embedded struct at the supplied offset."},
            {"Array","Field.Array(offset: number | bigint, element: Field, count: number)","Fixed-count array of a fixed-width field type."}
        });

        struct ObjectExperimentState
        {
            std::vector<RuntimeProcessInfo> Processes;
            pid_t Pid = 0;
            std::array<char,256> ProcessSearch{};
            std::array<char,256> Address{"0x0"};
            std::uintptr_t Base = 0;
            float RefreshHz = 10.0f;
            double LastRefresh = 0.0;
            TextEditor Editor;
            TextEditor::AutoCompleteConfig AutoComplete;
            bool EditorInitialized = false;
            bool EditorValid = false;
            std::string EditorDiagnostic;
            bool HasDefinition = false;
            StructExperimentDefinition Definition;
            std::vector<ExperimentRow> Rows;
            std::string Status;
        };

        const FieldHelp* fieldHelp(const std::string_view token)
        {
            std::string_view name = token; if (name.starts_with("Field.")) name.remove_prefix(6); const auto it = std::ranges::find(FieldHelpEntries,name,&FieldHelp::Name); return it == FieldHelpEntries.end() ? nullptr : &*it;
        }

        std::optional<std::size_t> diagnosticLine(const std::string_view error, const std::size_t lineCount)
        {
            static constexpr std::string_view Marker = "<object-experiment.ts>:"; const auto marker = error.find(Marker);
            if (marker != std::string_view::npos)
            {
                const std::size_t begin = marker + Marker.size(); std::size_t end = begin; while (end < error.size() && error[end] >= '0' && error[end] <= '9') ++end; std::size_t raw = 0;
                if (end > begin) if (const auto [ptr, ec] = std::from_chars(error.data()+begin,error.data()+end,raw); ec == std::errc{} && ptr == error.data()+end)
                {
                    constexpr std::size_t WrapperLines = 18; const std::size_t userLine = raw > WrapperLines ? raw - WrapperLines : raw; if (userLine) return std::min(userLine - 1,lineCount ? lineCount - 1 : 0);
                }
            }
            return std::nullopt;
        }

        void validateEditor(ObjectExperimentState& state)
        {
            StructExperimentDefinition definition; std::string error; state.EditorValid = runtimeEvaluateStructExperiment(state.Editor.GetText(),definition,error); state.EditorDiagnostic = state.EditorValid ? "Definition is valid." : std::move(error); state.Editor.ClearSquiggles(1);
            if (!state.EditorValid && state.Editor.GetLineCount())
            {
                std::size_t line = diagnosticLine(state.EditorDiagnostic,state.Editor.GetLineCount()).value_or(std::min(state.Editor.GetCurrentCursorPosition().line,state.Editor.GetLineCount()-1)); const std::string text = state.Editor.GetLineText(line); const std::size_t end = std::max<std::size_t>(text.size(),1); state.Editor.AddSquiggle({line,0},{line,end},1,IM_COL32(245,78,74,255),state.EditorDiagnostic);
            }
        }

        void initializeEditor(ObjectExperimentState& state)
        {
            if (state.EditorInitialized) return;
            static constexpr std::string_view DefaultSource = R"TS(const Vec2 = Struct.define({
    x: Field.Float32(0x0),
    y: Field.Float32(0x4),
});

return Struct.define({
    health: Field.Int32(0x0),
    maxHealth: Field.Int32(0x4),
    name: Field.CString(0x8),
    position: Field.Struct(0x10, Vec2),
    owner: Field.Pointer(0x18),
});)TS";
            configureQuartzTypeScriptEditor(state.Editor,false,false); state.Editor.SetText(DefaultSource); state.AutoComplete = {}; state.AutoComplete.triggerDelay = std::chrono::milliseconds(70); state.AutoComplete.suggestionWidth = 36; state.AutoComplete.noSuggestionsLabel = "No matching Quartz fields";
            state.AutoComplete.callback = [&state](TextEditor::AutoCompleteState& completion)
            {
                completion.suggestions.clear(); const std::string line = state.Editor.GetLineText(completion.searchTermStart.line); const std::size_t start = std::min(completion.searchTermStart.index,line.size()); const std::string_view before(line.data(),start); const auto matches = [&](const std::string_view candidate) { return completion.searchTerm.empty() || candidate.starts_with(completion.searchTerm); };
                if (before.ends_with("Field.")) for (const auto& entry:FieldHelpEntries) if (matches(entry.Name)) completion.suggestions.emplace_back(entry.Name);
                else if (before.ends_with("Struct.")) { if (matches("define")) completion.suggestions.emplace_back("define"); }
                else { if (matches("Field")) completion.suggestions.emplace_back("Field"); if (matches("Struct")) completion.suggestions.emplace_back("Struct"); state.Editor.IterateIdentifiers([&](const std::string& identifier) { if (identifier != completion.searchTerm && matches(identifier) && std::ranges::find(completion.suggestions,identifier) == completion.suggestions.end()) completion.suggestions.push_back(identifier); }); }
                std::ranges::sort(completion.suggestions);
            };
            state.Editor.SetAutoCompleteConfig(&state.AutoComplete);
            state.Editor.SetTextHoverCallback([&state](TextEditor::PopupData& data)
            {
                const std::string token = quartzDottedTokenAt(state.Editor,data.pos); if (token.empty()) { ImGui::CloseCurrentPopup(); return; }
                if (const auto* help = fieldHelp(token)) { ImGui::TextUnformatted(help->Signature.data(),help->Signature.data()+help->Signature.size()); ImGui::Separator(); ImGui::TextWrapped("%.*s",static_cast<int>(help->Description.size()),help->Description.data()); return; }
                if (token == "Struct" || token == "Struct.define") { ImGui::TextUnformatted("Struct.define(fields: Record<string, Field>): Struct"); ImGui::Separator(); ImGui::TextWrapped("Creates a temporary object type. The returned definition can be nested in Field.Struct/Pointer and the top-level script must return one."); return; }
                if (token == "Field") { ImGui::TextUnformatted("Quartz Field API"); ImGui::Separator(); ImGui::TextWrapped("Type Field. and use autocomplete (or Ctrl+Space) to see the available memory field descriptors."); return; }
                ImGui::CloseCurrentPopup();
            });
            state.Editor.SetChangeCallback([&state] { validateEditor(state); },350); state.EditorInitialized = true; validateEditor(state);
        }

        std::size_t pointerWidth(const RuntimeX86Mode mode) noexcept { return mode == RuntimeX86Mode::X86 ? 4 : 8; }

        std::optional<MemoryScanValueType> watchType(const StructExperimentFieldKind kind) noexcept
        {
            switch (kind)
            {
            case StructExperimentFieldKind::I8:return MemoryScanValueType::I8; case StructExperimentFieldKind::U8:return MemoryScanValueType::U8; case StructExperimentFieldKind::I16:return MemoryScanValueType::I16; case StructExperimentFieldKind::U16:return MemoryScanValueType::U16; case StructExperimentFieldKind::I32:return MemoryScanValueType::I32; case StructExperimentFieldKind::U32:return MemoryScanValueType::U32; case StructExperimentFieldKind::I64:return MemoryScanValueType::I64; case StructExperimentFieldKind::U64:return MemoryScanValueType::U64; case StructExperimentFieldKind::F32:return MemoryScanValueType::Float; case StructExperimentFieldKind::F64:return MemoryScanValueType::Double; case StructExperimentFieldKind::Bool:return MemoryScanValueType::Bool; case StructExperimentFieldKind::Pointer: case StructExperimentFieldKind::CString: case StructExperimentFieldKind::WString:return MemoryScanValueType::Pointer; default:return std::nullopt;
            }
        }

        template<typename T> std::string scalarText(const std::span<const std::uint8_t> bytes)
        {
            if (bytes.size() < sizeof(T)) return "<short read>"; T value{}; std::memcpy(&value,bytes.data(),sizeof(T)); std::ostringstream out; if constexpr (std::is_floating_point_v<T>) out << std::setprecision(12) << value; else if constexpr (sizeof(T)==1) out << static_cast<int>(value); else out << value; return out.str();
        }

        std::string scalarText(const StructExperimentFieldKind kind, const std::span<const std::uint8_t> bytes)
        {
            switch (kind) { case StructExperimentFieldKind::I8:return scalarText<std::int8_t>(bytes); case StructExperimentFieldKind::U8:return scalarText<std::uint8_t>(bytes); case StructExperimentFieldKind::I16:return scalarText<std::int16_t>(bytes); case StructExperimentFieldKind::U16:return scalarText<std::uint16_t>(bytes); case StructExperimentFieldKind::I32:return scalarText<std::int32_t>(bytes); case StructExperimentFieldKind::U32:return scalarText<std::uint32_t>(bytes); case StructExperimentFieldKind::I64:return scalarText<std::int64_t>(bytes); case StructExperimentFieldKind::U64:return scalarText<std::uint64_t>(bytes); case StructExperimentFieldKind::F32:return scalarText<float>(bytes); case StructExperimentFieldKind::F64:return scalarText<double>(bytes); case StructExperimentFieldKind::Bool:return !bytes.empty()&&bytes[0]?"true":"false"; default:return {}; }
        }

        bool readPointer(const pid_t pid, const std::uintptr_t address, const RuntimeX86Mode mode, std::uintptr_t& value, std::vector<std::uint8_t>& bytes, std::string& error)
        {
            bytes.resize(pointerWidth(mode)); if (!readProcessMemoryBlock(pid,address,bytes,error)) return false; if (mode==RuntimeX86Mode::X86) { std::uint32_t raw=0; std::memcpy(&raw,bytes.data(),sizeof(raw)); value=raw; } else { std::uint64_t raw=0; std::memcpy(&raw,bytes.data(),sizeof(raw)); value=static_cast<std::uintptr_t>(raw); } return true;
        }

        void appendUtf8(std::string& output, const std::uint32_t codepoint)
        {
            if (codepoint<=0x7F) output.push_back(static_cast<char>(codepoint)); else if (codepoint<=0x7FF) { output.push_back(static_cast<char>(0xC0|(codepoint>>6))); output.push_back(static_cast<char>(0x80|(codepoint&0x3F))); } else if (codepoint<=0xFFFF) { output.push_back(static_cast<char>(0xE0|(codepoint>>12))); output.push_back(static_cast<char>(0x80|((codepoint>>6)&0x3F))); output.push_back(static_cast<char>(0x80|(codepoint&0x3F))); } else { output.push_back(static_cast<char>(0xF0|(codepoint>>18))); output.push_back(static_cast<char>(0x80|((codepoint>>12)&0x3F))); output.push_back(static_cast<char>(0x80|((codepoint>>6)&0x3F))); output.push_back(static_cast<char>(0x80|(codepoint&0x3F))); }
        }

        bool readRemoteString(const pid_t pid, const std::uintptr_t pointer, const StructExperimentFieldKind kind, const std::size_t maxLength, std::string& value, std::string& error)
        {
            if (!pointer) { value="<null>"; return true; } const std::size_t width=kind==StructExperimentFieldKind::WString?2:1; std::vector<std::uint8_t> data(std::max<std::size_t>(maxLength,1)*width); if (!readProcessMemoryBlock(pid,pointer,data,error)) return false; value.clear();
            if (kind==StructExperimentFieldKind::CString) { const auto end=std::ranges::find(data,std::uint8_t{}); value.assign(reinterpret_cast<const char*>(data.data()),static_cast<std::size_t>(end-data.begin())); if (end==data.end()) value+="…"; return true; }
            const std::size_t units=data.size()/2; bool terminated=false; for (std::size_t i=0;i<units;++i) { std::uint16_t unit=0; std::memcpy(&unit,data.data()+i*2,2); if (!unit) { terminated=true; break; } std::uint32_t codepoint=unit; if (unit>=0xD800&&unit<=0xDBFF&&i+1<units) { std::uint16_t low=0; std::memcpy(&low,data.data()+(i+1)*2,2); if (low>=0xDC00&&low<=0xDFFF) { codepoint=0x10000+((unit-0xD800)<<10)+(low-0xDC00); ++i; } } appendUtf8(value,codepoint); } if (!terminated) value+="…"; return true;
        }

        void appendFieldRows(const pid_t pid, const RuntimeX86Mode mode, const StructExperimentField& field, const std::uintptr_t base, const std::string& name, std::vector<ExperimentRow>& rows, const std::size_t depth)
        {
            if (depth>8||rows.size()>=2048) return; const std::uintptr_t address=base+field.Offset;
            if (field.Kind==StructExperimentFieldKind::Struct) { rows.push_back({name,"struct",field.Offset,address,0,0,std::nullopt,field.Nested?"{...}":"<missing type>",{},field.Nested!=nullptr}); if (field.Nested) for (const auto& nested:field.Nested->Fields) appendFieldRows(pid,mode,nested,address,name+"."+nested.Name,rows,depth+1); return; }
            if (field.Kind==StructExperimentFieldKind::Array) { rows.push_back({name,"array["+std::to_string(field.Count)+"]",field.Offset,address,0,0,std::nullopt,field.Element?"[...]":"<missing element>",{},field.Element!=nullptr}); if (!field.Element) return; const std::size_t stride=field.Element->Size?field.Element->Size:field.Element->Kind==StructExperimentFieldKind::Pointer?pointerWidth(mode):0; if (!stride) return; const std::size_t visible=std::min<std::size_t>(field.Count,128); for (std::size_t i=0;i<visible;++i) appendFieldRows(pid,mode,*field.Element,address+i*stride,name+"["+std::to_string(i)+"]",rows,depth+1); return; }
            ExperimentRow row; row.Name=name; row.Type=runtimeStructExperimentFieldKindName(field.Kind); row.Offset=field.Offset; row.Address=address; row.WatchType=watchType(field.Kind); const bool pointerLike=field.Kind==StructExperimentFieldKind::Pointer||field.Kind==StructExperimentFieldKind::CString||field.Kind==StructExperimentFieldKind::WString; row.Width=pointerLike?pointerWidth(mode):field.Size; std::vector<std::uint8_t> bytes; std::string error;
            if (pointerLike) { row.Readable=readPointer(pid,address,mode,row.PointerValue,bytes,error); if (row.Readable&&(field.Kind==StructExperimentFieldKind::CString||field.Kind==StructExperimentFieldKind::WString)) row.Readable=readRemoteString(pid,row.PointerValue,field.Kind,field.MaxLength?field.MaxLength:256,row.Value,error); else if (row.Readable) row.Value=runtimeFormatProcessAddress(pid,row.PointerValue); if (!row.Readable) row.Value="<unreadable>"; }
            else { bytes.resize(row.Width); row.Readable=row.Width!=0&&readProcessMemoryBlock(pid,address,bytes,error); row.Value=row.Readable?scalarText(field.Kind,bytes):"<unreadable>"; }
            row.Raw=row.Readable?runtimeFormatHexBytes(bytes):std::move(error); const bool followPointer=field.Kind==StructExperimentFieldKind::Pointer&&field.Nested&&row.Readable&&row.PointerValue!=0; const std::uintptr_t pointedAddress=row.PointerValue; rows.emplace_back(std::move(row)); if (followPointer) for (const auto& nested:field.Nested->Fields) appendFieldRows(pid,mode,nested,pointedAddress,name+"->"+nested.Name,rows,depth+1);
        }

        void refreshRows(ObjectExperimentState& state) { state.Rows.clear(); if (!state.HasDefinition||state.Pid<=0||state.Base==0) return; const RuntimeX86Mode mode=runtimeProcessX86Mode(state.Pid); for (const auto& field:state.Definition.Fields) appendFieldRows(state.Pid,mode,field,state.Base,field.Name,state.Rows,0); state.LastRefresh=runtimeSteadySeconds(); }
        void openInspector(PageManager& manager,const pid_t pid,const std::uintptr_t address) { if (!address) return; requestMemoryInspector(pid,address); manager.open("native"); }
        void openAccessWatch(PageManager& manager,const pid_t pid,const std::uintptr_t address,const std::size_t width) { if (auto* page=dynamic_cast<MemoryWatchPage*>(manager.find("memory-watch"))) page->setTarget(pid,address,width); manager.open("memory-watch"); }
        void addWatch(PageManager& manager,const pid_t pid,const ExperimentRow& row) { if (!row.WatchType||!row.Width) return; if (auto* page=dynamic_cast<MemoryScannerPage*>(manager.find("memory-scanner"))) page->addWatch(pid,row.Address,*row.WatchType,row.Width); manager.open("memory-scanner"); }
    }

    void drawObjectExperiments(PageContext&, PageManager& manager)
    {
        static ObjectExperimentState state; initializeEditor(state); applyQuartzTextEditorPalette(state.Editor); ImGui::TextWrapped("Define a temporary object layout in TypeScript and bind the returned Struct.define({...}) value directly to a live process address. The script is the source of truth; no persistent C++ object descriptor is created.");
        if (drawProcessPicker("ObjectExperimentProcess",state.Processes,state.Pid,state.ProcessSearch.data(),state.ProcessSearch.size(),520.0f)) { state.Base=0; state.Rows.clear(); state.LastRefresh=0.0; }
        drawAddressInput("Base address",state.Address.data(),state.Address.size(),state.Pid,360.0f); ImGui::SameLine(); if (ImGui::Button("Bind address")) { std::uintptr_t address=0; std::string error; if (state.Pid<=0) state.Status="select a process"; else if (!evaluateAddressExpression(state.Pid,state.Address.data(),address,error)||address==0) state.Status=error.empty()?"invalid base address":error; else { state.Base=address; state.LastRefresh=0.0; state.Status="address bound"; refreshRows(state); } }
        ImGui::SameLine(); if (ImGui::Button("Use inspector address")) { const auto& inspector=runtimeMemoryInspectorState(); if (inspector.Pid>0&&inspector.Address!=0) { state.Pid=inspector.Pid; state.Base=inspector.Address; std::snprintf(state.Address.data(),state.Address.size(),"0x%llX",static_cast<unsigned long long>(state.Base)); state.LastRefresh=0.0; state.Status="using inspector address"; refreshRows(state); } else state.Status="memory inspector has no active address"; }
        ImGui::SetNextItemWidth(180.0f); ImGui::SliderFloat("Refresh rate",&state.RefreshHz,0.5f,60.0f,"%.1f Hz",ImGuiSliderFlags_Logarithmic); ImGui::SameLine(); ImGui::TextDisabled("Address expressions support module names and + - * /; Ctrl+Shift+E collapses them to hex.");
        ImGui::SeparatorText("TypeScript definition"); if (ImGui::Button("Apply definition")) { StructExperimentDefinition definition; std::string error; if (runtimeEvaluateStructExperiment(state.Editor.GetText(),definition,error)) { state.Definition=std::move(definition); state.HasDefinition=true; state.LastRefresh=0.0; state.Status="definition applied: "+std::to_string(state.Definition.Fields.size())+" top-level fields"; refreshRows(state); } else { state.HasDefinition=false; state.Rows.clear(); state.Status=std::move(error); } }
        ImGui::SameLine(); if (ImGui::Button("Reset example")) { state.EditorInitialized=false; initializeEditor(state); } ImGui::SameLine(); ImGui::TextDisabled("Struct and Field are injected automatically. Field. autocomplete and Ctrl+Space are enabled; hover API names for signatures."); state.Editor.Render("##ObjectExperimentEditor",ImVec2(-1.0f,230.0f));
        if (!state.EditorDiagnostic.empty()) { if (state.EditorValid) ImGui::TextColored(ImVec4(0.35f,0.86f,0.58f,1.0f),"%s",state.EditorDiagnostic.c_str()); else ImGui::TextWrapped("%s",state.EditorDiagnostic.c_str()); } if (!state.Status.empty()) ImGui::TextWrapped("%s",state.Status.c_str());
        if (!state.HasDefinition) { ImGui::TextDisabled("Apply a Struct.define({...}) definition to inspect fields."); return; } if (state.Pid<=0||state.Base==0) { ImGui::TextDisabled("Select a process and bind a non-zero address to start live reads."); return; }
        const double now=runtimeSteadySeconds(); if (state.LastRefresh==0.0||now-state.LastRefresh>=1.0/std::max(static_cast<double>(state.RefreshHz),0.1)) refreshRows(state); const auto modules=enumerateRuntimeModules(state.Pid); ImGui::SeparatorText("Bound object"); const std::string baseText=runtimeFormatProcessAddress(modules,state.Base); ImGui::TextUnformatted(baseText.c_str()); ImGui::SameLine(); ImGui::TextDisabled("PID %d | %s | %zu rows",static_cast<int>(state.Pid),runtimeX86ModeName(runtimeProcessX86Mode(state.Pid)),state.Rows.size()); if (ImGui::Button("Inspect base")) openInspector(manager,state.Pid,state.Base); ImGui::SameLine(); if (ImGui::Button("Refresh now")) refreshRows(state);
        if (ImGui::BeginTable("ObjectExperimentFields",6,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollY,ImVec2(0.0f,360.0f)))
        {
            ImGui::TableSetupColumn("Field"); ImGui::TableSetupColumn("Offset",ImGuiTableColumnFlags_WidthFixed,88.0f); ImGui::TableSetupColumn("Address",ImGuiTableColumnFlags_WidthFixed,190.0f); ImGui::TableSetupColumn("Type",ImGuiTableColumnFlags_WidthFixed,105.0f); ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("Raw bytes"); ImGui::TableHeadersRow();
            for (std::size_t i=0;i<state.Rows.size();++i)
            {
                const auto& row=state.Rows[i]; ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Selectable("##ObjectExperimentRow",false,ImGuiSelectableFlags_SpanAllColumns|ImGuiSelectableFlags_AllowOverlap,ImVec2(0.0f,ImGui::GetTextLineHeight())); if (ImGui::IsItemHovered()&&ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openInspector(manager,state.Pid,row.Address);
                if (ImGui::BeginPopupContextItem("ObjectExperimentContext")) { if (ImGui::MenuItem("Inspect field")) openInspector(manager,state.Pid,row.Address); ImGui::BeginDisabled(!row.WatchType||row.Width==0); if (ImGui::MenuItem("Add field to watch list")) addWatch(manager,state.Pid,row); if (ImGui::MenuItem("Watch field accesses")) openAccessWatch(manager,state.Pid,row.Address,row.Width); ImGui::EndDisabled(); ImGui::BeginDisabled(row.PointerValue==0); if (ImGui::MenuItem("Inspect pointer value")) openInspector(manager,state.Pid,row.PointerValue); ImGui::EndDisabled(); ImGui::Separator(); if (ImGui::MenuItem("Copy address")) { const std::string text=runtimeHexAddress(row.Address); ImGui::SetClipboardText(text.c_str()); } if (ImGui::MenuItem("Copy module-relative address")) { const std::string text=runtimeFormatProcessAddress(modules,row.Address); ImGui::SetClipboardText(text.c_str()); } if (ImGui::MenuItem("Copy value")) ImGui::SetClipboardText(row.Value.c_str()); ImGui::EndPopup(); }
                ImGui::SameLine(); ImGui::TextUnformatted(row.Name.c_str()); ImGui::TableNextColumn(); ImGui::Text("+0x%llX",static_cast<unsigned long long>(row.Offset)); ImGui::TableNextColumn(); const std::string addressText=runtimeFormatProcessAddress(modules,row.Address); ImGui::TextUnformatted(addressText.c_str()); ImGui::TableNextColumn(); ImGui::TextUnformatted(row.Type.c_str()); ImGui::TableNextColumn(); ImGui::TextUnformatted(row.Value.c_str()); ImGui::TableNextColumn(); ImGui::TextDisabled("%s",row.Raw.c_str()); ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
}
