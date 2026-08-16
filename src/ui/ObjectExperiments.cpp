#include "quartz/client/ui/ObjectExperiments.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/runtime/StructExperiment.hpp"
#include "quartz/client/ui/AddressInput.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/ProcessPicker.hpp"
#include "quartz/client/ui/ReverseEngineeringNavigation.hpp"
#include "quartz/client/ui/pages/MemoryScannerPage.hpp"
#include "quartz/client/ui/pages/MemoryWatchPage.hpp"
#include <TextEditor.h>
#include <algorithm>
#include <array>
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

        struct ObjectExperimentState
        {
            std::vector<RuntimeProcessInfo> Processes;
            pid_t Pid = 0;
            std::array<char, 256> ProcessSearch{};
            std::array<char, 256> Address{"0x0"};
            std::uintptr_t Base = 0;
            float RefreshHz = 10.0f;
            double LastRefresh = 0.0;
            TextEditor Editor;
            bool EditorInitialized = false;
            bool HasDefinition = false;
            StructExperimentDefinition Definition;
            std::vector<ExperimentRow> Rows;
            std::string Status;
        };

        const TextEditor::Language* typescriptLanguage()
        {
            static const TextEditor::Language language = []
            {
                TextEditor::Language value; value.name = "TypeScript"; value.singleLineComment = "//"; value.commentStart = "/*"; value.commentEnd = "*/"; value.hasSingleQuotedStrings = true; value.hasDoubleQuotedStrings = true; value.otherStringStart = "`"; value.otherStringEnd = "`"; value.stringEscape = '\\';
                value.keywords = {"any","as","bigint","boolean","break","case","catch","const","continue","default","do","else","false","for","function","if","interface","let","new","null","number","object","readonly","return","string","switch","true","type","typeof","undefined","var","while"};
                value.declarations = {"Struct","Field"};
                value.identifiers = {"Struct","Field","BigInt","Number","String","Boolean","Array","Object"};
                value.isPunctuation = [](const ImWchar c) { return std::string_view("[]{}().,;:+-*/%<>=!&|^~?").find(static_cast<char>(c)) != std::string_view::npos; };
                value.getIdentifier = [](TextEditor::Iterator start, const TextEditor::Iterator end) { if (start == end || !(TextEditor::CodePoint::isXidStart(*start) || *start == '_' || *start == '$')) return start; auto current = start; ++current; while (current != end && (TextEditor::CodePoint::isXidContinue(*current) || *current == '$')) ++current; return current; };
                value.getNumber = [](TextEditor::Iterator start, const TextEditor::Iterator end)
                {
                    if (start == end) return start; auto current = start; auto next = current; ++next; const auto digit = [](const ImWchar c) { return c >= '0' && c <= '9'; }; const auto hex = [&](const ImWchar c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
                    if (*current == '.' && (next == end || !digit(*next))) return start;
                    if (*current == '0' && next != end && (*next == 'x' || *next == 'X')) { current = next; ++current; while (current != end && (*current == '_' || hex(*current))) ++current; if (current != end && *current == 'n') ++current; return current; }
                    while (current != end && (digit(*current) || *current == '_' || *current == '.')) ++current; if (current != end && *current == 'n') ++current; return current;
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
    position: Field.Struct(0x8, Vec2),
    owner: Field.Pointer(0x10),
});)TS";
            state.Editor.SetLanguage(typescriptLanguage()); state.Editor.SetPalette(typescriptPalette()); state.Editor.SetTabSize(4); state.Editor.SetInsertSpacesOnTabs(true); state.Editor.SetAutoIndentEnabled(true); state.Editor.SetShowLineNumbersEnabled(true); state.Editor.SetShowMatchingBrackets(true); state.Editor.SetShowMiniMapEnabled(false); state.Editor.SetReadOnlyEnabled(false); state.Editor.SetText(DefaultSource); state.EditorInitialized = true;
        }

        std::size_t pointerWidth(const RuntimeX86Mode mode) noexcept { return mode == RuntimeX86Mode::X86 ? 4 : 8; }

        std::optional<MemoryScanValueType> watchType(const StructExperimentFieldKind kind) noexcept
        {
            switch (kind)
            {
            case StructExperimentFieldKind::I8: return MemoryScanValueType::I8; case StructExperimentFieldKind::U8: return MemoryScanValueType::U8;
            case StructExperimentFieldKind::I16: return MemoryScanValueType::I16; case StructExperimentFieldKind::U16: return MemoryScanValueType::U16;
            case StructExperimentFieldKind::I32: return MemoryScanValueType::I32; case StructExperimentFieldKind::U32: return MemoryScanValueType::U32;
            case StructExperimentFieldKind::I64: return MemoryScanValueType::I64; case StructExperimentFieldKind::U64: return MemoryScanValueType::U64;
            case StructExperimentFieldKind::F32: return MemoryScanValueType::Float; case StructExperimentFieldKind::F64: return MemoryScanValueType::Double;
            case StructExperimentFieldKind::Bool: return MemoryScanValueType::Bool; case StructExperimentFieldKind::Pointer: return MemoryScanValueType::Pointer;
            default: return std::nullopt;
            }
        }

        template<typename T>
        std::string scalarText(const std::span<const std::uint8_t> bytes)
        {
            if (bytes.size() < sizeof(T)) return "<short read>"; T value{}; std::memcpy(&value, bytes.data(), sizeof(T)); std::ostringstream out;
            if constexpr (std::is_floating_point_v<T>) out << std::setprecision(12) << value; else if constexpr (sizeof(T) == 1) out << static_cast<int>(value); else out << value; return out.str();
        }

        std::string scalarText(const StructExperimentFieldKind kind, const std::span<const std::uint8_t> bytes)
        {
            switch (kind)
            {
            case StructExperimentFieldKind::I8: return scalarText<std::int8_t>(bytes); case StructExperimentFieldKind::U8: return scalarText<std::uint8_t>(bytes);
            case StructExperimentFieldKind::I16: return scalarText<std::int16_t>(bytes); case StructExperimentFieldKind::U16: return scalarText<std::uint16_t>(bytes);
            case StructExperimentFieldKind::I32: return scalarText<std::int32_t>(bytes); case StructExperimentFieldKind::U32: return scalarText<std::uint32_t>(bytes);
            case StructExperimentFieldKind::I64: return scalarText<std::int64_t>(bytes); case StructExperimentFieldKind::U64: return scalarText<std::uint64_t>(bytes);
            case StructExperimentFieldKind::F32: return scalarText<float>(bytes); case StructExperimentFieldKind::F64: return scalarText<double>(bytes);
            case StructExperimentFieldKind::Bool: return !bytes.empty() && bytes[0] ? "true" : "false";
            default: return {};
            }
        }

        bool readPointer(const pid_t pid, const std::uintptr_t address, const RuntimeX86Mode mode, std::uintptr_t& value, std::vector<std::uint8_t>& bytes, std::string& error)
        {
            bytes.resize(pointerWidth(mode)); if (!readProcessMemoryBlock(pid, address, bytes, error)) return false;
            if (mode == RuntimeX86Mode::X86) { std::uint32_t raw = 0; std::memcpy(&raw, bytes.data(), sizeof(raw)); value = raw; }
            else { std::uint64_t raw = 0; std::memcpy(&raw, bytes.data(), sizeof(raw)); value = static_cast<std::uintptr_t>(raw); }
            return true;
        }

        void appendFieldRows(const pid_t pid, const RuntimeX86Mode mode, const StructExperimentField& field, const std::uintptr_t base, const std::string& name, std::vector<ExperimentRow>& rows, const std::size_t depth)
        {
            if (depth > 8 || rows.size() >= 2048) return;
            const std::uintptr_t address = base + field.Offset;
            if (field.Kind == StructExperimentFieldKind::Struct)
            {
                rows.push_back({name, "struct", field.Offset, address, 0, 0, std::nullopt, field.Nested ? "{...}" : "<missing type>", {}, field.Nested != nullptr});
                if (field.Nested) for (const auto& nested : field.Nested->Fields) appendFieldRows(pid, mode, nested, address, name + "." + nested.Name, rows, depth + 1);
                return;
            }
            if (field.Kind == StructExperimentFieldKind::Array)
            {
                rows.push_back({name, "array[" + std::to_string(field.Count) + "]", field.Offset, address, 0, 0, std::nullopt, field.Element ? "[...]" : "<missing element>", {}, field.Element != nullptr});
                if (!field.Element) return;
                const std::size_t stride = field.Element->Size ? field.Element->Size : field.Element->Kind == StructExperimentFieldKind::Pointer ? pointerWidth(mode) : 0;
                if (!stride) return;
                const std::size_t visible = std::min<std::size_t>(field.Count, 128);
                for (std::size_t i = 0; i < visible; ++i) appendFieldRows(pid, mode, *field.Element, address + i * stride, name + "[" + std::to_string(i) + "]", rows, depth + 1);
                return;
            }

            ExperimentRow row; row.Name = name; row.Type = runtimeStructExperimentFieldKindName(field.Kind); row.Offset = field.Offset; row.Address = address; row.WatchType = watchType(field.Kind); row.Width = field.Kind == StructExperimentFieldKind::Pointer ? pointerWidth(mode) : field.Size;
            std::vector<std::uint8_t> bytes; std::string error;
            if (field.Kind == StructExperimentFieldKind::Pointer)
            {
                row.Readable = readPointer(pid, address, mode, row.PointerValue, bytes, error); row.Value = row.Readable ? runtimeHexAddress(row.PointerValue) : "<unreadable>";
            }
            else
            {
                bytes.resize(row.Width); row.Readable = row.Width != 0 && readProcessMemoryBlock(pid, address, bytes, error); row.Value = row.Readable ? scalarText(field.Kind, bytes) : "<unreadable>";
            }
            row.Raw = row.Readable ? runtimeFormatHexBytes(bytes) : std::move(error);
            const bool followPointer = field.Kind == StructExperimentFieldKind::Pointer && field.Nested && row.Readable && row.PointerValue != 0;
            const std::uintptr_t pointedAddress = row.PointerValue;
            rows.emplace_back(std::move(row));
            if (followPointer) for (const auto& nested : field.Nested->Fields) appendFieldRows(pid, mode, nested, pointedAddress, name + "->" + nested.Name, rows, depth + 1);
        }

        void refreshRows(ObjectExperimentState& state)
        {
            state.Rows.clear(); if (!state.HasDefinition || state.Pid <= 0 || state.Base == 0) return;
            const RuntimeX86Mode mode = runtimeProcessX86Mode(state.Pid); for (const auto& field : state.Definition.Fields) appendFieldRows(state.Pid, mode, field, state.Base, field.Name, state.Rows, 0); state.LastRefresh = runtimeSteadySeconds();
        }

        void openInspector(PageManager& manager, const pid_t pid, const std::uintptr_t address) { if (!address) return; requestMemoryInspector(pid, address); manager.open("native"); }
        void openAccessWatch(PageManager& manager, const pid_t pid, const std::uintptr_t address, const std::size_t width) { if (auto* page = dynamic_cast<MemoryWatchPage*>(manager.find("memory-watch"))) page->setTarget(pid, address, width); manager.open("memory-watch"); }
        void addWatch(PageManager& manager, const pid_t pid, const ExperimentRow& row) { if (!row.WatchType || !row.Width) return; if (auto* page = dynamic_cast<MemoryScannerPage*>(manager.find("memory-scanner"))) page->addWatch(pid, row.Address, *row.WatchType, row.Width); manager.open("memory-scanner"); }
    }

    void drawObjectExperiments(PageContext&, PageManager& manager)
    {
        static ObjectExperimentState state; initializeEditor(state);
        ImGui::TextWrapped("Define a temporary object layout in TypeScript and bind the returned Struct.define({...}) value directly to a live process address. The script is the source of truth; no persistent C++ object descriptor is created.");
        if (drawProcessPicker("ObjectExperimentProcess", state.Processes, state.Pid, state.ProcessSearch.data(), state.ProcessSearch.size(), 520.0f)) { state.Base = 0; state.Rows.clear(); state.LastRefresh = 0.0; }
        drawAddressInput("Base address", state.Address.data(), state.Address.size(), state.Pid, 360.0f); ImGui::SameLine();
        if (ImGui::Button("Bind address"))
        {
            std::uintptr_t address = 0; std::string error; if (state.Pid <= 0) state.Status = "select a process"; else if (!evaluateAddressExpression(state.Pid, state.Address.data(), address, error) || address == 0) state.Status = error.empty() ? "invalid base address" : error; else { state.Base = address; state.LastRefresh = 0.0; state.Status = "address bound"; refreshRows(state); }
        }
        ImGui::SameLine(); if (ImGui::Button("Use inspector address"))
        {
            const auto& inspector = runtimeMemoryInspectorState(); if (inspector.Pid > 0 && inspector.Address != 0) { state.Pid = inspector.Pid; state.Base = inspector.Address; std::snprintf(state.Address.data(), state.Address.size(), "0x%llX", static_cast<unsigned long long>(state.Base)); state.LastRefresh = 0.0; state.Status = "using inspector address"; refreshRows(state); } else state.Status = "memory inspector has no active address";
        }
        ImGui::SetNextItemWidth(180.0f); ImGui::SliderFloat("Refresh rate", &state.RefreshHz, 0.5f, 60.0f, "%.1f Hz", ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine(); ImGui::TextDisabled("Address expressions support module names and + - * /; Ctrl+Shift+E collapses them to hex.");

        ImGui::SeparatorText("TypeScript definition");
        if (ImGui::Button("Apply definition"))
        {
            StructExperimentDefinition definition; std::string error;
            if (runtimeEvaluateStructExperiment(state.Editor.GetText(), definition, error)) { state.Definition = std::move(definition); state.HasDefinition = true; state.LastRefresh = 0.0; state.Status = "definition applied: " + std::to_string(state.Definition.Fields.size()) + " top-level fields"; refreshRows(state); }
            else { state.HasDefinition = false; state.Rows.clear(); state.Status = std::move(error); }
        }
        ImGui::SameLine(); if (ImGui::Button("Reset example")) { state.EditorInitialized = false; initializeEditor(state); }
        ImGui::SameLine(); ImGui::TextDisabled("Struct and Field are injected automatically; top-level return is valid here.");
        state.Editor.Render("##ObjectExperimentEditor", ImVec2(-1.0f, 230.0f));
        if (!state.Status.empty()) ImGui::TextWrapped("%s", state.Status.c_str());

        if (!state.HasDefinition) { ImGui::TextDisabled("Apply a Struct.define({...}) definition to inspect fields."); return; }
        if (state.Pid <= 0 || state.Base == 0) { ImGui::TextDisabled("Select a process and bind a non-zero address to start live reads."); return; }
        const double now = runtimeSteadySeconds(); if (state.LastRefresh == 0.0 || now - state.LastRefresh >= 1.0 / std::max(static_cast<double>(state.RefreshHz), 0.1)) refreshRows(state);
        ImGui::SeparatorText("Bound object"); ImGui::Text("0x%llX", static_cast<unsigned long long>(state.Base)); ImGui::SameLine(); ImGui::TextDisabled("PID %d | %s | %zu rows", static_cast<int>(state.Pid), runtimeX86ModeName(runtimeProcessX86Mode(state.Pid)), state.Rows.size());
        if (ImGui::Button("Inspect base")) openInspector(manager, state.Pid, state.Base); ImGui::SameLine(); if (ImGui::Button("Refresh now")) refreshRows(state);

        if (ImGui::BeginTable("ObjectExperimentFields", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 360.0f)))
        {
            ImGui::TableSetupColumn("Field"); ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 88.0f); ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 145.0f); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 105.0f); ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("Raw bytes"); ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < state.Rows.size(); ++i)
            {
                const auto& row = state.Rows[i]; ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::Selectable("##ObjectExperimentRow", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeight()));
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openInspector(manager, state.Pid, row.Address);
                if (ImGui::BeginPopupContextItem("ObjectExperimentContext"))
                {
                    if (ImGui::MenuItem("Inspect field")) openInspector(manager, state.Pid, row.Address);
                    ImGui::BeginDisabled(!row.WatchType || row.Width == 0); if (ImGui::MenuItem("Add field to watch list")) addWatch(manager, state.Pid, row); if (ImGui::MenuItem("Watch field accesses")) openAccessWatch(manager, state.Pid, row.Address, row.Width); ImGui::EndDisabled();
                    ImGui::BeginDisabled(row.PointerValue == 0); if (ImGui::MenuItem("Inspect pointer value")) openInspector(manager, state.Pid, row.PointerValue); ImGui::EndDisabled();
                    ImGui::Separator(); if (ImGui::MenuItem("Copy address")) { const std::string text = runtimeHexAddress(row.Address); ImGui::SetClipboardText(text.c_str()); } if (ImGui::MenuItem("Copy value")) ImGui::SetClipboardText(row.Value.c_str()); ImGui::EndPopup();
                }
                ImGui::SameLine(); ImGui::TextUnformatted(row.Name.c_str()); ImGui::TableNextColumn(); ImGui::Text("+0x%llX", static_cast<unsigned long long>(row.Offset)); ImGui::TableNextColumn(); ImGui::Text("0x%llX", static_cast<unsigned long long>(row.Address)); ImGui::TableNextColumn(); ImGui::TextUnformatted(row.Type.c_str()); ImGui::TableNextColumn(); ImGui::TextUnformatted(row.Value.c_str()); ImGui::TableNextColumn(); ImGui::TextDisabled("%s", row.Raw.c_str()); ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
}
