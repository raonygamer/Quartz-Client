#include "quartz/client/ui/pages/MemoryScannerPage.hpp"
#include "quartz/client/ui/pages/MemoryWatchPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace quartz::client::ui
{
    namespace
    {
        struct ValueSample
        {
            std::string Value;
            std::optional<long double> Numeric;
        };

        bool parseAddress(const char* text, std::uintptr_t& value)
        {
            if (!text || !*text) return false;
            std::string_view view(text); int base = 10;
            if (view.starts_with("0x") || view.starts_with("0X")) { view.remove_prefix(2); base = 16; }
            const auto [ptr, ec] = std::from_chars(view.data(), view.data() + view.size(), value, base); return ec == std::errc{} && ptr == view.data() + view.size();
        }

        std::size_t fixedValueWidth(const MemoryScanValueType type) noexcept
        {
            switch (type)
            {
            case MemoryScanValueType::U8: case MemoryScanValueType::I8: case MemoryScanValueType::Bool: return 1;
            case MemoryScanValueType::U16: case MemoryScanValueType::I16: return 2;
            case MemoryScanValueType::U32: case MemoryScanValueType::I32: case MemoryScanValueType::Float: return 4;
            case MemoryScanValueType::U64: case MemoryScanValueType::I64: case MemoryScanValueType::Double: case MemoryScanValueType::Pointer: return 8;
            default: return 0;
            }
        }

        std::size_t resultValueWidth(const MemoryScanValueType type, const std::string_view value) noexcept
        {
            if (const auto fixed = fixedValueWidth(type); fixed != 0) return fixed;
            if (type == MemoryScanValueType::Utf8String) return value.size();
            if (type == MemoryScanValueType::Utf16String) return value.size() * 2;
            if (type == MemoryScanValueType::ByteArray) return value.empty() ? 0 : (value.size() + 1) / 3;
            return 0;
        }

        long double numericAt(const MemoryScanValueType type, const std::uint8_t* data)
        {
            switch (type)
            {
            case MemoryScanValueType::U8: { std::uint8_t v; std::memcpy(&v, data, 1); return v; }
            case MemoryScanValueType::I8: { std::int8_t v; std::memcpy(&v, data, 1); return v; }
            case MemoryScanValueType::U16: { std::uint16_t v; std::memcpy(&v, data, 2); return v; }
            case MemoryScanValueType::I16: { std::int16_t v; std::memcpy(&v, data, 2); return v; }
            case MemoryScanValueType::U32: { std::uint32_t v; std::memcpy(&v, data, 4); return v; }
            case MemoryScanValueType::I32: { std::int32_t v; std::memcpy(&v, data, 4); return v; }
            case MemoryScanValueType::U64: case MemoryScanValueType::Pointer: { std::uint64_t v; std::memcpy(&v, data, 8); return static_cast<long double>(v); }
            case MemoryScanValueType::I64: { std::int64_t v; std::memcpy(&v, data, 8); return static_cast<long double>(v); }
            case MemoryScanValueType::Float: { float v; std::memcpy(&v, data, 4); return static_cast<long double>(v); }
            case MemoryScanValueType::Double: { double v; std::memcpy(&v, data, 8); return static_cast<long double>(v); }
            case MemoryScanValueType::Bool: { std::uint8_t v; std::memcpy(&v, data, 1); return v != 0 ? 1.0L : 0.0L; }
            default: return 0.0L;
            }
        }

        std::optional<long double> parseDisplayedNumeric(const MemoryScanValueType type, const std::string_view text)
        {
            if (fixedValueWidth(type) == 0) return std::nullopt;
            if (type == MemoryScanValueType::Pointer)
            {
                std::string value(text); char* end = nullptr; errno = 0;
                const unsigned long long parsed = std::strtoull(value.c_str(), &end, 0);
                if (errno != 0 || end != value.c_str() + value.size()) return std::nullopt;
                return static_cast<long double>(parsed);
            }
            std::string value(text); char* end = nullptr; errno = 0;
            const long double parsed = std::strtold(value.c_str(), &end);
            if (errno != 0 || end != value.c_str() + value.size() || !std::isfinite(parsed)) return std::nullopt;
            return parsed;
        }

        std::string formatSample(const MemoryScanValueType type, const std::span<const std::uint8_t> bytes)
        {
            std::ostringstream out;
            if (fixedValueWidth(type) != 0)
            {
                if (type == MemoryScanValueType::Pointer) { std::uint64_t value; std::memcpy(&value, bytes.data(), 8); out << "0x" << std::hex << std::uppercase << value; }
                else if (type == MemoryScanValueType::Float || type == MemoryScanValueType::Double) out << std::setprecision(12) << static_cast<double>(numericAt(type, bytes.data()));
                else out << std::fixed << std::setprecision(0) << numericAt(type, bytes.data());
                return out.str();
            }
            if (type == MemoryScanValueType::Utf8String) return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (type == MemoryScanValueType::Utf16String)
            {
                std::string result; result.reserve(bytes.size() / 2);
                for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) { const std::uint16_t cp = static_cast<std::uint16_t>(bytes[i] | (bytes[i + 1] << 8)); result.push_back(cp >= 32 && cp < 127 ? static_cast<char>(cp) : '.'); }
                return result;
            }
            return runtimeFormatHexBytes(bytes);
        }

        bool readValueSample(const pid_t pid, const std::uintptr_t address, const MemoryScanValueType type, const std::size_t width, ValueSample& sample, std::string& error)
        {
            if (pid <= 0 || address == 0 || width == 0) { error = "invalid value target"; return false; }
            std::vector<std::uint8_t> bytes(width);
            if (!readProcessMemoryBlock(pid, address, std::span<std::uint8_t>(bytes.data(), bytes.size()), error)) return false;
            sample.Value = formatSample(type, bytes);
            sample.Numeric = fixedValueWidth(type) != 0 ? std::optional<long double>(numericAt(type, bytes.data())) : std::nullopt;
            return true;
        }

        std::vector<std::uint8_t> utf16Bytes(const std::string_view text)
        {
            std::vector<std::uint8_t> result; result.reserve(text.size() * 2);
            const unsigned char* p = reinterpret_cast<const unsigned char*>(text.data()); const unsigned char* end = p + text.size();
            while (p < end)
            {
                std::uint32_t cp = *p++;
                if (cp >= 0xC2 && cp <= 0xDF && p < end) cp = ((cp & 0x1F) << 6) | (*p++ & 0x3F);
                else if (cp >= 0xE0 && cp <= 0xEF && p + 1 < end) { cp = ((cp & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
                else if (cp >= 0xF0 && cp <= 0xF4 && p + 2 < end) { cp = ((cp & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
                if (cp <= 0xFFFF) { result.push_back(static_cast<std::uint8_t>(cp)); result.push_back(static_cast<std::uint8_t>(cp >> 8)); }
                else { cp -= 0x10000; const std::uint16_t hi = static_cast<std::uint16_t>(0xD800 + (cp >> 10)), lo = static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF)); result.push_back(static_cast<std::uint8_t>(hi)); result.push_back(static_cast<std::uint8_t>(hi >> 8)); result.push_back(static_cast<std::uint8_t>(lo)); result.push_back(static_cast<std::uint8_t>(lo >> 8)); }
            }
            return result;
        }

        template<typename T>
        bool storeNumeric(const long double value, std::vector<std::uint8_t>& bytes, std::string& error)
        {
            if constexpr (std::is_integral_v<T>)
            {
                if (value < static_cast<long double>(std::numeric_limits<T>::lowest()) || value > static_cast<long double>(std::numeric_limits<T>::max())) { error = "value is outside the selected type range"; return false; }
            }
            else if (std::abs(value) > static_cast<long double>(std::numeric_limits<T>::max())) { error = "value is outside the selected type range"; return false; }
            const T converted = static_cast<T>(value); bytes.resize(sizeof(T)); std::memcpy(bytes.data(), &converted, sizeof(T)); return true;
        }

        bool writeValueSample(const pid_t pid, const std::uintptr_t address, const MemoryScanValueType type, const std::size_t width, const std::string_view text, std::string& error)
        {
            if (pid <= 0 || address == 0 || width == 0) { error = "invalid value target"; return false; }
            std::vector<std::uint8_t> bytes;
            if (fixedValueWidth(type) != 0)
            {
                if (type == MemoryScanValueType::Pointer)
                {
                    std::string value(text); char* end = nullptr; errno = 0; const unsigned long long parsed = std::strtoull(value.c_str(), &end, 0);
                    if (errno != 0 || end != value.c_str() + value.size()) { error = "invalid pointer value"; return false; }
                    const std::uint64_t converted = parsed; bytes.resize(sizeof(converted)); std::memcpy(bytes.data(), &converted, sizeof(converted));
                }
                else if (type == MemoryScanValueType::Bool)
                {
                    std::string value = runtimeLower(text); std::uint8_t converted = 0;
                    if (value == "true" || value == "1") converted = 1; else if (value == "false" || value == "0") converted = 0; else { error = "bool must be true/false or 1/0"; return false; }
                    bytes.push_back(converted);
                }
                else
                {
                    std::string value(text); char* end = nullptr; errno = 0; const long double parsed = std::strtold(value.c_str(), &end);
                    if (errno != 0 || end != value.c_str() + value.size() || !std::isfinite(parsed)) { error = "invalid numeric value"; return false; }
                    switch (type)
                    {
                    case MemoryScanValueType::U8: if (!storeNumeric<std::uint8_t>(parsed, bytes, error)) return false; break;
                    case MemoryScanValueType::I8: if (!storeNumeric<std::int8_t>(parsed, bytes, error)) return false; break;
                    case MemoryScanValueType::U16: if (!storeNumeric<std::uint16_t>(parsed, bytes, error)) return false; break;
                    case MemoryScanValueType::I16: if (!storeNumeric<std::int16_t>(parsed, bytes, error)) return false; break;
                    case MemoryScanValueType::U32: if (!storeNumeric<std::uint32_t>(parsed, bytes, error)) return false; break;
                    case MemoryScanValueType::I32: if (!storeNumeric<std::int32_t>(parsed, bytes, error)) return false; break;
                    case MemoryScanValueType::U64: if (!storeNumeric<std::uint64_t>(parsed, bytes, error)) return false; break;
                    case MemoryScanValueType::I64: if (!storeNumeric<std::int64_t>(parsed, bytes, error)) return false; break;
                    case MemoryScanValueType::Float: if (!storeNumeric<float>(parsed, bytes, error)) return false; break;
                    case MemoryScanValueType::Double: if (!storeNumeric<double>(parsed, bytes, error)) return false; break;
                    default: break;
                    }
                }
            }
            else if (type == MemoryScanValueType::Utf8String)
            {
                bytes.assign(text.begin(), text.end()); if (bytes.size() > width) { error = "string is wider than the watched value"; return false; } bytes.resize(width, 0);
            }
            else if (type == MemoryScanValueType::Utf16String)
            {
                bytes = utf16Bytes(text); if (bytes.size() > width) { error = "string is wider than the watched value"; return false; } bytes.resize(width, 0);
            }
            else
            {
                if (!runtimeParseHexBytes(text, bytes, error)) return false;
                if (bytes.size() != width) { error = "byte-array write must keep the watched width"; return false; }
            }
            return runtimeWriteProcessMemory(pid, address, std::span<const std::uint8_t>(bytes.data(), bytes.size()), error);
        }

        std::optional<ProcessValueType> bindingValueType(const MemoryScanValueType type) noexcept
        {
            switch (type)
            {
            case MemoryScanValueType::U8: return ProcessValueType::U8;
            case MemoryScanValueType::I8: return ProcessValueType::I8;
            case MemoryScanValueType::U16: return ProcessValueType::U16;
            case MemoryScanValueType::I16: return ProcessValueType::I16;
            case MemoryScanValueType::U32: return ProcessValueType::U32;
            case MemoryScanValueType::I32: return ProcessValueType::I32;
            case MemoryScanValueType::U64: case MemoryScanValueType::Pointer: return ProcessValueType::U64;
            case MemoryScanValueType::I64: return ProcessValueType::I64;
            case MemoryScanValueType::Float: return ProcessValueType::Float;
            case MemoryScanValueType::Double: return ProcessValueType::Double;
            case MemoryScanValueType::Bool: return ProcessValueType::Bool;
            default: return std::nullopt;
            }
        }

        void createValueBinding(RuntimeBindingEngine& engine, const pid_t pid, const RuntimeProcessInfo* process, const std::uintptr_t address, const ProcessValueType type)
        {
            auto& binding = engine.add();
            std::snprintf(binding.Name, sizeof(binding.Name), "Scan %llX", static_cast<unsigned long long>(address));
            binding.Source = RuntimeSourceKind::NativeProcess;
            binding.ProcessId = static_cast<int>(pid);
            if (process) std::snprintf(binding.ProcessName, sizeof(binding.ProcessName), "%s", process->Name.c_str());
            binding.AddressMode = ProcessAddressMode::AddressChain;
            std::snprintf(binding.Address, sizeof(binding.Address), "0x%llX", static_cast<unsigned long long>(address));
            binding.ValueType = type;
            binding.WriteMaterial = false;
            binding.Normalize = false;
            binding.Clamp = false;
            binding.SmoothingHz = 0.0f;
            binding.UpdateHz = 30.0f;
            binding.AutoReattach = false;
        }

        void openInspector(PageManager& manager, const pid_t pid, const std::uintptr_t address)
        {
            auto& inspector = runtimeMemoryInspectorState(); inspector.Pid = pid; inspector.Address = address; runtimeRefreshMemoryInspector(inspector); manager.open("native");
        }

        void openAccessWatch(PageManager& manager, const pid_t pid, const std::uintptr_t address, const std::size_t width)
        {
            if (auto* page = dynamic_cast<MemoryWatchPage*>(manager.find("memory-watch"))) page->setTarget(pid, address, width);
            manager.open("memory-watch");
        }

        ImVec4 valueChangeColor(const std::optional<long double> current, const std::optional<long double> previous)
        {
            if (!current || !previous) return ImGui::GetStyleColorVec4(ImGuiCol_Text);
            const long double epsilon = std::max(std::abs(*previous), 1.0L) * 1e-9L;
            if (*current > *previous + epsilon) return {0.30f, 0.95f, 0.40f, 1.0f};
            if (*current < *previous - epsilon) return {0.95f, 0.30f, 0.30f, 1.0f};
            return ImGui::GetStyleColorVec4(ImGuiCol_Text);
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
        if (ImGui::Button("New Scan")) { _liveValues.clear(); _scanner.newScan(request, _status); }
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasSnapshot);
        if (ImGui::Button("Next Scan")) { _liveValues.clear(); _scanner.nextScan(request, _status); }
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
        ImGui::SetNextItemWidth(220.0f); ImGui::SliderFloat("Visible value refresh", &_refreshHz, 0.2f, 30.0f, "%.1f Hz", ImGuiSliderFlags_Logarithmic); ImGui::SameLine(); ImGui::TextDisabled("Only rows currently on screen are reread. Previous value always means the actual last scan snapshot.");

        const auto rows = _scanner.results(256);
        const auto scanType = _scanner.valueType();
        const auto bindType = bindingValueType(scanType);
        const double now = runtimeSteadySeconds();
        const double refreshInterval = 1.0 / std::max(static_cast<double>(_refreshHz), 0.1);
        auto addWatch = [&](const MemoryScanResultRow& row, const LiveValue* live)
        {
            const std::size_t width = resultValueWidth(scanType, row.Value);
            if (width == 0) { _status = "cannot determine watched value width"; return; }
            for (const auto& watch : _watchList) if (watch.Pid == _scanner.pid() && watch.Address == row.Address && watch.Type == scanType) { _status = "value is already in the watch list"; return; }
            WatchedValue watch; watch.Id = _nextWatchId++; watch.Pid = _scanner.pid(); watch.Address = row.Address; watch.Type = scanType; watch.Width = width; watch.Value = live && !live->Value.empty() ? live->Value : row.Value; watch.PreviousValue = row.Value; watch.Numeric = live ? live->Numeric : parseDisplayedNumeric(scanType, row.Value); watch.FrozenValue = watch.Value; std::snprintf(watch.AddressText.data(), watch.AddressText.size(), "0x%llX", static_cast<unsigned long long>(watch.Address)); std::snprintf(watch.ValueText.data(), watch.ValueText.size(), "%s", watch.Value.c_str()); _watchList.emplace_back(std::move(watch)); _status = "added value to watch list";
        };

        if (!rows.empty() && ImGui::BeginTable("MemoryScanResults", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0.0f, 300.0f)))
        {
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 150.0f); ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("Previous value"); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 125.0f); ImGui::TableHeadersRow();
            ImGuiListClipper clipper; clipper.Begin(static_cast<int>(rows.size()));
            while (clipper.Step()) for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
            {
                const auto& row = rows[static_cast<std::size_t>(rowIndex)];
                auto& live = _liveValues[row.Address];
                if (live.LastRefresh == 0.0 || now - live.LastRefresh >= refreshInterval)
                {
                    ValueSample sample; std::string error;
                    if (readValueSample(_scanner.pid(), row.Address, scanType, resultValueWidth(scanType, row.Value), sample, error)) { live.Value = std::move(sample.Value); live.Numeric = sample.Numeric; }
                    else { live.Value = "<unreadable>"; live.Numeric.reset(); }
                    live.LastRefresh = now;
                }
                const auto previousNumeric = parseDisplayedNumeric(scanType, row.Value);
                const ImVec4 valueColor = valueChangeColor(live.Numeric, previousNumeric);
                ImGui::PushID(static_cast<int>(row.Address & 0x7fffffffULL)); ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("0x%llX", static_cast<unsigned long long>(row.Address)); ImGui::TableNextColumn(); ImGui::TextColored(valueColor, "%s", live.Value.empty() ? row.Value.c_str() : live.Value.c_str());
                if (ImGui::BeginPopupContextItem("ScanResultContext"))
                {
                    if (ImGui::IsWindowAppearing() || _scanWriteAddress != row.Address) { _scanWriteAddress = row.Address; std::snprintf(_scanWriteValue.data(), _scanWriteValue.size(), "%s", live.Value.empty() ? row.Value.c_str() : live.Value.c_str()); }
                    if (ImGui::MenuItem("Inspect memory")) openInspector(manager, _scanner.pid(), row.Address);
                    if (ImGui::MenuItem("Disassemble")) openInspector(manager, _scanner.pid(), row.Address);
                    if (ImGui::MenuItem("Watch accesses...")) openAccessWatch(manager, _scanner.pid(), row.Address, resultValueWidth(scanType, row.Value));
                    if (ImGui::MenuItem("Add to watch list")) addWatch(row, &live);
                    ImGui::BeginDisabled(!bindType.has_value());
                    if (ImGui::MenuItem("Create binding") && bindType) { createValueBinding(context.runtimeBindings, _scanner.pid(), selected, row.Address, *bindType); _status = "created native value binding"; }
                    ImGui::EndDisabled();
                    ImGui::Separator();
                    if (ImGui::MenuItem("Copy address")) { const std::string text = runtimeHexAddress(row.Address); ImGui::SetClipboardText(text.c_str()); }
                    if (ImGui::MenuItem("Copy current value")) ImGui::SetClipboardText((live.Value.empty() ? row.Value : live.Value).c_str());
                    if (ImGui::MenuItem("Copy previous value")) ImGui::SetClipboardText(row.Value.c_str());
                    ImGui::SeparatorText("Change value");
                    ImGui::SetNextItemWidth(220.0f); ImGui::InputText("##ScanWriteValue", _scanWriteValue.data(), _scanWriteValue.size());
                    if (ImGui::Button("Write value")) { if (writeValueSample(_scanner.pid(), row.Address, scanType, resultValueWidth(scanType, row.Value), _scanWriteValue.data(), _status)) { live.LastRefresh = 0.0; _status = "value written"; } }
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn(); ImGui::TextUnformatted(row.Value.c_str()); ImGui::TableNextColumn();
                if (ImGui::SmallButton("Inspect")) openInspector(manager, _scanner.pid(), row.Address);
                ImGui::SameLine(); ImGui::BeginDisabled(!bindType.has_value()); if (ImGui::SmallButton("Bind") && bindType) { createValueBinding(context.runtimeBindings, _scanner.pid(), selected, row.Address, *bindType); _status = "created native value binding"; } ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (stats.Candidates > rows.size()) ImGui::TextDisabled("Showing first %zu of %llu candidates.", rows.size(), static_cast<unsigned long long>(stats.Candidates));
        }

        ImGui::SeparatorText("Watch list");
        ImGui::SetNextItemWidth(180.0f); ImGui::SliderFloat("Freeze rate", &_freezeHz, 1.0f, 120.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic); ImGui::SameLine(); if (ImGui::Button("Clear watch list")) _watchList.clear();
        ImGui::TextDisabled("Right-click a watched row to change its address, type or value, inspect/disassemble it, create a binding, or send it to the hardware access watcher.");
        std::optional<std::size_t> eraseWatch;
        if (ImGui::BeginTable("MemoryWatchList", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable, ImVec2(0.0f, 0.0f)))
        {
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 150.0f); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100.0f); ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("Previous"); ImGui::TableSetupColumn("Freeze", ImGuiTableColumnFlags_WidthFixed, 60.0f); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 65.0f); ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < _watchList.size(); ++i)
            {
                auto& watch = _watchList[i]; ImGui::PushID(static_cast<int>(watch.Id & 0x7fffffffULL));
                const double freezeInterval = 1.0 / std::max(static_cast<double>(_freezeHz), 1.0);
                if (watch.Frozen && (watch.LastFreeze == 0.0 || now - watch.LastFreeze >= freezeInterval)) { if (!writeValueSample(watch.Pid, watch.Address, watch.Type, watch.Width, watch.FrozenValue, _status)) watch.Frozen = false; watch.LastFreeze = now; }
                if (watch.LastRefresh == 0.0 || now - watch.LastRefresh >= refreshInterval)
                {
                    ValueSample sample; std::string error;
                    if (readValueSample(watch.Pid, watch.Address, watch.Type, watch.Width, sample, error)) { watch.PreviousValue = watch.Value; watch.Value = std::move(sample.Value); watch.Numeric = sample.Numeric; if (!watch.Frozen) std::snprintf(watch.ValueText.data(), watch.ValueText.size(), "%s", watch.Value.c_str()); }
                    else { watch.PreviousValue = watch.Value; watch.Value = "<unreadable>"; watch.Numeric.reset(); }
                    watch.LastRefresh = now;
                }
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("0x%llX", static_cast<unsigned long long>(watch.Address)); ImGui::TableNextColumn(); ImGui::TextUnformatted(memoryScanValueTypeName(watch.Type)); ImGui::TableNextColumn(); ImGui::TextUnformatted(watch.Value.c_str());
                if (ImGui::BeginPopupContextItem("WatchValueContext"))
                {
                    ImGui::SeparatorText("Change address"); ImGui::SetNextItemWidth(220.0f); ImGui::InputText("##WatchAddress", watch.AddressText.data(), watch.AddressText.size());
                    if (ImGui::Button("Apply address")) { std::uintptr_t address = 0; if (!parseAddress(watch.AddressText.data(), address)) _status = "invalid watch address"; else { watch.Address = address; watch.LastRefresh = 0.0; watch.LastFreeze = 0.0; _status = "watch address changed"; } }
                    ImGui::SeparatorText("Change type"); ImGui::SetNextItemWidth(180.0f);
                    if (ImGui::BeginCombo("##WatchType", memoryScanValueTypeName(watch.Type)))
                    {
                        for (int typeIndex = 0; typeIndex <= static_cast<int>(MemoryScanValueType::ByteArray); ++typeIndex)
                        {
                            const auto type = static_cast<MemoryScanValueType>(typeIndex); const bool active = type == watch.Type;
                            if (ImGui::Selectable(memoryScanValueTypeName(type), active)) { watch.Type = type; if (const auto fixed = fixedValueWidth(type); fixed != 0) watch.Width = fixed; watch.LastRefresh = 0.0; watch.Frozen = false; }
                            if (active) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (fixedValueWidth(watch.Type) == 0) { int width = static_cast<int>(watch.Width); ImGui::SetNextItemWidth(120.0f); if (ImGui::InputInt("Width", &width)) { watch.Width = static_cast<std::size_t>(std::max(width, 1)); watch.LastRefresh = 0.0; watch.Frozen = false; } }
                    ImGui::SeparatorText("Change value"); ImGui::SetNextItemWidth(220.0f); ImGui::InputText("##WatchValue", watch.ValueText.data(), watch.ValueText.size());
                    if (ImGui::Button("Write")) { if (writeValueSample(watch.Pid, watch.Address, watch.Type, watch.Width, watch.ValueText.data(), _status)) { watch.LastRefresh = 0.0; if (watch.Frozen) watch.FrozenValue = watch.ValueText.data(); _status = "watch value written"; } }
                    ImGui::SameLine(); bool frozen = watch.Frozen; if (ImGui::Checkbox("Freeze", &frozen)) { watch.Frozen = frozen; watch.LastFreeze = 0.0; if (watch.Frozen) { watch.FrozenValue = watch.ValueText.data(); if (watch.FrozenValue.empty() || watch.FrozenValue == "<unreadable>") watch.FrozenValue = watch.Value; } }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Inspect memory")) openInspector(manager, watch.Pid, watch.Address);
                    if (ImGui::MenuItem("Disassemble")) openInspector(manager, watch.Pid, watch.Address);
                    if (ImGui::MenuItem("Watch accesses...")) openAccessWatch(manager, watch.Pid, watch.Address, watch.Width);
                    const auto watchBindType = bindingValueType(watch.Type); ImGui::BeginDisabled(!watchBindType.has_value()); if (ImGui::MenuItem("Create binding") && watchBindType) { const RuntimeProcessInfo* process = nullptr; for (const auto& candidate : _processes) if (candidate.Pid == watch.Pid) { process = &candidate; break; } createValueBinding(context.runtimeBindings, watch.Pid, process, watch.Address, *watchBindType); _status = "created binding from watch list"; } ImGui::EndDisabled();
                    if (ImGui::MenuItem("Copy address")) { const std::string text = runtimeHexAddress(watch.Address); ImGui::SetClipboardText(text.c_str()); }
                    if (ImGui::MenuItem("Copy value")) ImGui::SetClipboardText(watch.Value.c_str());
                    ImGui::Separator(); if (ImGui::MenuItem("Remove from watch list")) eraseWatch = i;
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn(); ImGui::TextUnformatted(watch.PreviousValue.c_str()); ImGui::TableNextColumn(); bool frozen = watch.Frozen; if (ImGui::Checkbox("##Frozen", &frozen)) { watch.Frozen = frozen; watch.LastFreeze = 0.0; if (watch.Frozen) { watch.FrozenValue = watch.Value; std::snprintf(watch.ValueText.data(), watch.ValueText.size(), "%s", watch.Value.c_str()); } } ImGui::TableNextColumn(); if (ImGui::SmallButton("Remove")) eraseWatch = i; ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (eraseWatch && *eraseWatch < _watchList.size()) _watchList.erase(_watchList.begin() + static_cast<std::ptrdiff_t>(*eraseWatch));
        if (_watchList.empty()) ImGui::TextDisabled("Right-click a scan result and choose Add to watch list.");

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
                auto& binding = context.runtimeBindings.add(); std::snprintf(binding.Name, sizeof(binding.Name), "%s", "Derived signature"); binding.Source = RuntimeSourceKind::NativeAddress; binding.ProcessId = _pid; if (selected) std::snprintf(binding.ProcessName, sizeof(binding.ProcessName), "%s", selected->Name.c_str()); binding.AddressMode = ProcessAddressMode::Signature; binding.SignaturePatternKind = RuntimeSignaturePatternKind::HexadecimalPattern; binding.SignatureExecutableOnly = true; binding.WriteMaterial = false; binding.Clamp = false; binding.SmoothingHz = 0.0f; std::snprintf(binding.Signature, sizeof(binding.Signature), "%s", _derivedPattern.c_str()); _status = "created NativeAddress binding";
            }
        }
    }
}
