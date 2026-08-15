#include "quartz/client/ui/pages/MemoryScannerPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    namespace
    {
        bool parseAddress(const char* text, std::uintptr_t& value)
        {
            if (!text || !*text) return false;
            std::string_view view(text); int base = 10;
            if (view.starts_with("0x") || view.starts_with("0X")) { view.remove_prefix(2); base = 16; }
            const auto [ptr, ec] = std::from_chars(view.data(), view.data() + view.size(), value, base); return ec == std::errc{} && ptr == view.data() + view.size();
        }
    }

    void MemoryScannerPage::render(PageContext& context, PageManager& manager)
    {
        _scanner.poll();
        if (_processes.empty()) _processes = enumerateRuntimeProcesses();
        ImGui::TextWrapped("Cheat-Engine-style value scanning backed by process_vm_readv() and the shared Quartz worker pool. New Scan snapshots candidates; Next Scan filters the previous candidate bitmap without storing millions of uintptr_t values.");
        if (ImGui::Button("Refresh processes")) _processes = enumerateRuntimeProcesses();
        ImGui::SameLine(); ImGui::SetNextItemWidth(420.0f);
        const RuntimeProcessInfo* selected = nullptr; for (const auto& process : _processes) if (process.Pid == _pid) { selected = &process; break; }
        if (ImGui::BeginCombo("Process", selected ? runtimeProcessDisplayTitle(*selected).c_str() : "<select process>"))
        {
            for (const auto& process : _processes) { const bool active = process.Pid == _pid; const std::string label = runtimeProcessDisplayTitle(process) + "  [" + std::to_string(process.Pid) + "]"; if (ImGui::Selectable(label.c_str(), active)) _pid = process.Pid; if (active) ImGui::SetItemDefaultFocus(); }
            ImGui::EndCombo();
        }

        ImGui::SeparatorText("Scan");
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::BeginCombo("Value type", memoryScanValueTypeName(static_cast<MemoryScanValueType>(_type))))
        {
            for (int i = 0; i <= static_cast<int>(MemoryScanValueType::ByteArray); ++i) if (ImGui::Selectable(memoryScanValueTypeName(static_cast<MemoryScanValueType>(i)), i == _type)) _type = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("Comparison", memoryScanComparisonName(static_cast<MemoryScanComparison>(_comparison))))
        {
            for (int i = 0; i <= static_cast<int>(MemoryScanComparison::ChangedFromTo); ++i) if (ImGui::Selectable(memoryScanComparisonName(static_cast<MemoryScanComparison>(i)), i == _comparison)) _comparison = i;
            ImGui::EndCombo();
        }
        const auto comparison = static_cast<MemoryScanComparison>(_comparison);
        const bool needsA = comparison == MemoryScanComparison::Exact || comparison == MemoryScanComparison::NotEqual || comparison == MemoryScanComparison::IncreasedBy || comparison == MemoryScanComparison::DecreasedBy || comparison == MemoryScanComparison::Greater || comparison == MemoryScanComparison::Less || comparison == MemoryScanComparison::Between || comparison == MemoryScanComparison::ChangedFromTo;
        const bool needsB = comparison == MemoryScanComparison::Between || comparison == MemoryScanComparison::ChangedFromTo;
        if (needsA) { ImGui::SetNextItemWidth(360.0f); ImGui::InputText(comparison == MemoryScanComparison::ChangedFromTo ? "From" : "Value", _valueA.data(), _valueA.size()); }
        if (needsB) { ImGui::SetNextItemWidth(360.0f); ImGui::InputText(comparison == MemoryScanComparison::ChangedFromTo ? "To" : "Value B", _valueB.data(), _valueB.size()); }
        if (_type == static_cast<int>(MemoryScanValueType::ByteArray)) ImGui::TextDisabled("Byte arrays use Quartz hex syntax, including ?? and nibble wildcards A? / ?F.");
        ImGui::Checkbox("Writable mappings only", &_writableOnly); ImGui::SameLine(); ImGui::Checkbox("Executable only", &_executableOnly); ImGui::SameLine(); ImGui::Checkbox("Aligned values", &_aligned);
        if (_type == static_cast<int>(MemoryScanValueType::Utf8String) || _type == static_cast<int>(MemoryScanValueType::Utf16String)) { ImGui::SameLine(); ImGui::Checkbox("Case sensitive", &_caseSensitive); }

        MemoryScanRequest request{_pid, static_cast<MemoryScanValueType>(_type), comparison, _valueA.data(), _valueB.data(), _writableOnly, _executableOnly, _aligned, _caseSensitive};
        const bool scanRunning = _scanner.running();
        const bool hasSnapshot = _scanner.hasSnapshot();
        ImGui::BeginDisabled(scanRunning);
        if (ImGui::Button("New Scan")) _scanner.newScan(request, _status);
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasSnapshot);
        if (ImGui::Button("Next Scan")) _scanner.nextScan(request, _status);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!scanRunning);
        if (ImGui::Button("Cancel")) _scanner.cancel();
        ImGui::EndDisabled();

        const auto stats = _scanner.stats();
        if (stats.Running) drawIndeterminateProgressBar(ImVec2(360.0f, 0.0f));
        if (stats.MiBs > 0.0) { if (stats.MiBs >= 1024.0) ImGui::Text("%.2f GiB/s", stats.MiBs / 1024.0); else ImGui::Text("%.1f MiB/s", stats.MiBs); ImGui::SameLine(); ImGui::TextDisabled("%.1f MiB in %.3f s", stats.Bytes / (1024.0 * 1024.0), stats.Seconds); }
        ImGui::Text("Candidates: %llu", static_cast<unsigned long long>(stats.Candidates)); ImGui::SameLine(); ImGui::TextDisabled("%s%s", stats.Status.c_str(), _status.empty() ? "" : (" | " + _status).c_str());

        const auto rows = _scanner.results(256);
        if (!rows.empty() && ImGui::BeginTable("MemoryScanResults", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0.0f, 260.0f)))
        {
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 150.0f); ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 62.0f); ImGui::TableHeadersRow();
            for (const auto& row : rows)
            {
                ImGui::PushID(static_cast<int>(row.Address & 0x7fffffffULL)); ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("0x%llX", static_cast<unsigned long long>(row.Address)); ImGui::TableNextColumn(); ImGui::TextUnformatted(row.Value.c_str()); ImGui::TableNextColumn();
                if (ImGui::SmallButton("Inspect")) { auto& inspector = runtimeMemoryInspectorState(); inspector.Pid = _pid; inspector.Address = row.Address; runtimeRefreshMemoryInspector(inspector); manager.open("native"); }
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (stats.Candidates > rows.size()) ImGui::TextDisabled("Showing first %zu of %llu candidates.", rows.size(), static_cast<unsigned long long>(stats.Candidates));
        }

        ImGui::SeparatorText("Derive byte pattern from address range");
        ImGui::TextDisabled("Reads [start, end) and optionally wildcards relative immediates plus RIP-relative displacements decoded by Zydis.");
        ImGui::SetNextItemWidth(180.0f); ImGui::InputText("Start", _rangeStart.data(), _rangeStart.size()); ImGui::SameLine(); ImGui::SetNextItemWidth(180.0f); ImGui::InputText("End", _rangeEnd.data(), _rangeEnd.size());
        ImGui::Checkbox("Wildcard relocation-sensitive operands", &_wildcardRelocations);
        if (ImGui::Button("Derive pattern"))
        {
            std::uintptr_t start = 0, end = 0;
            if (!parseAddress(_rangeStart.data(), start) || !parseAddress(_rangeEnd.data(), end)) _status = "invalid range";
            else _derivedPattern = deriveRuntimeBytePattern(_pid, start, end, _wildcardRelocations, _status);
        }
        if (!_derivedPattern.empty())
        {
            ImGui::InputTextMultiline("Derived pattern", _derivedPattern.data(), _derivedPattern.size() + 1, ImVec2(-1.0f, 90.0f), ImGuiInputTextFlags_ReadOnly);
            if (ImGui::Button("Copy pattern")) ImGui::SetClipboardText(_derivedPattern.c_str()); ImGui::SameLine();
            if (ImGui::Button("Create native-address binding"))
            {
                auto& binding = context.runtimeBindings.add(); std::snprintf(binding.Name, sizeof(binding.Name), "%s", "Derived signature"); binding.Source = RuntimeSourceKind::NativeAddress; binding.ProcessId = _pid; binding.AddressMode = ProcessAddressMode::Signature; binding.SignaturePatternKind = RuntimeSignaturePatternKind::HexadecimalPattern; binding.SignatureExecutableOnly = true; binding.WriteMaterial = false; binding.Clamp = false; binding.SmoothingHz = 0.0f; std::snprintf(binding.Signature, sizeof(binding.Signature), "%s", _derivedPattern.c_str()); _status = "created NativeAddress binding";
            }
        }
    }
}
