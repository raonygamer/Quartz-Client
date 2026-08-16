#include "quartz/client/ui/pages/MemoryScannerPage.hpp"
#include "quartz/client/ui/pages/MemoryWatchPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/AddressInput.hpp"
#include "quartz/client/ui/I18n.hpp"
#include "quartz/client/ui/ProcessPicker.hpp"
#include "quartz/client/ui/ReverseEngineeringNavigation.hpp"
#include "quartz/client/ui/ReverseEngineeringTools.hpp"
#include "quartz/client/ui/SignatureMaker.hpp"
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

        std::size_t fixedValueWidth(const MemoryScanValueType type, const pid_t pid = 0) noexcept
        {
            switch (type)
            {
            case MemoryScanValueType::U8: case MemoryScanValueType::I8: case MemoryScanValueType::Bool: return 1;
            case MemoryScanValueType::U16: case MemoryScanValueType::I16: return 2;
            case MemoryScanValueType::U32: case MemoryScanValueType::I32: case MemoryScanValueType::Float: return 4;
            case MemoryScanValueType::U64: case MemoryScanValueType::I64: case MemoryScanValueType::Double: return 8;
            case MemoryScanValueType::Pointer: return pid > 0 && runtimeProcessX86Mode(pid) == RuntimeX86Mode::X86 ? 4 : 8;
            default: return 0;
            }
        }

        const char* scanValueTypeLabel(const MemoryScanValueType type) noexcept
        {
            static constexpr const char* English[] = {"u8","i8","u16","i16","u32","i32","u64","i64","float","double","pointer","bool","UTF-8 string","UTF-16 string","byte array"};
            static constexpr const char* Portuguese[] = {"u8","i8","u16","i16","u32","i32","u64","i64","float","double","ponteiro","bool","string UTF-8","string UTF-16","array de bytes"};
            const int index = std::clamp(static_cast<int>(type),0,static_cast<int>(std::size(English))-1); return i18n::language()==i18n::Language::PortugueseBrazil?Portuguese[index]:English[index];
        }

        const char* scanComparisonLabel(const MemoryScanComparison comparison) noexcept
        {
            static constexpr const char* English[] = {"Exact value","Not equal","Unknown initial value","Changed","Unchanged","Increased","Decreased","Increased by","Decreased by","Greater than","Less than","Between","Changed from -> to"};
            static constexpr const char* Portuguese[] = {"Valor exato","Diferente","Valor inicial desconhecido","Alterado","Inalterado","Aumentou","Diminuiu","Aumentou em","Diminuiu em","Maior que","Menor que","Entre","Alterou de -> para"};
            const int index = std::clamp(static_cast<int>(comparison),0,static_cast<int>(std::size(English))-1); return i18n::language()==i18n::Language::PortugueseBrazil?Portuguese[index]:English[index];
        }

        bool integerValueType(const MemoryScanValueType type) noexcept
        {
            return type == MemoryScanValueType::U8 || type == MemoryScanValueType::I8 || type == MemoryScanValueType::U16 || type == MemoryScanValueType::I16 || type == MemoryScanValueType::U32 || type == MemoryScanValueType::I32 || type == MemoryScanValueType::U64 || type == MemoryScanValueType::I64 || type == MemoryScanValueType::Pointer || type == MemoryScanValueType::Bool;
        }

        bool signedValueType(const MemoryScanValueType type) noexcept { return type == MemoryScanValueType::I8 || type == MemoryScanValueType::I16 || type == MemoryScanValueType::I32 || type == MemoryScanValueType::I64; }

        bool parseNumericInput(const MemoryScanValueType type, const std::string_view text, long double& value)
        {
            if (integerValueType(type))
            {
                if (signedValueType(type)) { std::int64_t parsed = 0; if (!parseNumber(text, parsed)) return false; value = static_cast<long double>(parsed); return true; }
                std::uint64_t parsed = 0; if (!parseNumber(text, parsed)) return false; value = static_cast<long double>(parsed); return true;
            }
            std::string copy(text); char* end = nullptr; errno = 0; value = std::strtold(copy.c_str(), &end); return errno == 0 && end == copy.c_str() + copy.size() && std::isfinite(value);
        }

        std::string normalizedScanInput(const MemoryScanValueType type, const char* text)
        {
            if (!text) return {};
            if (!integerValueType(type)) return text;
            if (signedValueType(type)) { std::int64_t value = 0; if (parseNumber<std::int64_t>(text, value)) return std::to_string(value); }
            else { std::uint64_t value = 0; if (parseNumber<std::uint64_t>(text, value)) return std::to_string(value); }
            return text;
        }

        std::size_t resultValueWidth(const MemoryScanValueType type, const std::string_view value, const pid_t pid = 0) noexcept
        {
            if (const auto fixed = fixedValueWidth(type, pid); fixed != 0) return fixed;
            if (type == MemoryScanValueType::Utf8String) return value.size();
            if (type == MemoryScanValueType::Utf16String) return value.size() * 2;
            if (type == MemoryScanValueType::ByteArray) return value.empty() ? 0 : (value.size() + 1) / 3;
            return 0;
        }

        long double numericAt(const MemoryScanValueType type, const std::uint8_t* data, const std::size_t width = 0)
        {
            switch (type)
            {
            case MemoryScanValueType::U8: { std::uint8_t v; std::memcpy(&v, data, 1); return v; }
            case MemoryScanValueType::I8: { std::int8_t v; std::memcpy(&v, data, 1); return v; }
            case MemoryScanValueType::U16: { std::uint16_t v; std::memcpy(&v, data, 2); return v; }
            case MemoryScanValueType::I16: { std::int16_t v; std::memcpy(&v, data, 2); return v; }
            case MemoryScanValueType::U32: { std::uint32_t v; std::memcpy(&v, data, 4); return v; }
            case MemoryScanValueType::I32: { std::int32_t v; std::memcpy(&v, data, 4); return v; }
            case MemoryScanValueType::U64: { std::uint64_t v; std::memcpy(&v, data, 8); return static_cast<long double>(v); }
            case MemoryScanValueType::Pointer: { if (width == 4) { std::uint32_t v; std::memcpy(&v, data, 4); return static_cast<long double>(v); } std::uint64_t v; std::memcpy(&v, data, 8); return static_cast<long double>(v); }
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
            long double parsed = 0.0L; if (!parseNumericInput(type, text, parsed)) return std::nullopt; return parsed;
        }

        std::string formatSample(const MemoryScanValueType type, const std::span<const std::uint8_t> bytes)
        {
            std::ostringstream out;
            if (fixedValueWidth(type) != 0)
            {
                if (type == MemoryScanValueType::Pointer) { if (bytes.size() == 4) { std::uint32_t value; std::memcpy(&value, bytes.data(), 4); out << "0x" << std::hex << std::uppercase << value; } else { std::uint64_t value; std::memcpy(&value, bytes.data(), 8); out << "0x" << std::hex << std::uppercase << value; } }
                else if (type == MemoryScanValueType::Float || type == MemoryScanValueType::Double) out << std::setprecision(12) << static_cast<double>(numericAt(type, bytes.data(), bytes.size()));
                else out << std::fixed << std::setprecision(0) << numericAt(type, bytes.data(), bytes.size());
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
            if (pid <= 0 || address == 0 || width == 0) { error = i18n::tr("re.invalidValueTarget"); return false; }
            std::vector<std::uint8_t> bytes(width);
            if (!readProcessMemoryBlock(pid, address, std::span<std::uint8_t>(bytes.data(), bytes.size()), error)) return false;
            sample.Value = formatSample(type, bytes);
            sample.Numeric = fixedValueWidth(type, pid) != 0 ? std::optional<long double>(numericAt(type, bytes.data(), width)) : std::nullopt;
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
                if (value < static_cast<long double>(std::numeric_limits<T>::lowest()) || value > static_cast<long double>(std::numeric_limits<T>::max())) { error = i18n::tr("re.valueOutsideTypeRange"); return false; }
            }
            else if (std::abs(value) > static_cast<long double>(std::numeric_limits<T>::max())) { error = i18n::tr("re.valueOutsideTypeRange"); return false; }
            const T converted = static_cast<T>(value); bytes.resize(sizeof(T)); std::memcpy(bytes.data(), &converted, sizeof(T)); return true;
        }

        bool writeValueSample(const pid_t pid, const std::uintptr_t address, const MemoryScanValueType type, const std::size_t width, const std::string_view text, std::string& error)
        {
            if (pid <= 0 || address == 0 || width == 0) { error = i18n::tr("re.invalidValueTarget"); return false; }
            std::vector<std::uint8_t> bytes;
            if (fixedValueWidth(type) != 0)
            {
                if (type == MemoryScanValueType::Pointer)
                {
                    std::uint64_t parsed = 0; if (!parseNumber(text, parsed)) { error = i18n::tr("re.invalidPointerValue"); return false; }
                    if (width == 4) { if (parsed > std::numeric_limits<std::uint32_t>::max()) { error = i18n::tr("re.valueOutsideTypeRange"); return false; } const auto value = static_cast<std::uint32_t>(parsed); bytes.resize(sizeof(value)); std::memcpy(bytes.data(), &value, sizeof(value)); }
                    else { bytes.resize(sizeof(parsed)); std::memcpy(bytes.data(), &parsed, sizeof(parsed)); }
                }
                else if (type == MemoryScanValueType::Bool)
                {
                    std::string value = runtimeLower(text); std::uint8_t converted = 0;
                    if (value == "true" || value == "1" || value == "0x1") converted = 1; else if (value == "false" || value == "0" || value == "0x0") converted = 0; else { error = i18n::tr("re.boolValueHint"); return false; }
                    bytes.push_back(converted);
                }
                else
                {
                    long double parsed = 0.0L; if (!parseNumericInput(type, text, parsed)) { error = i18n::tr("re.invalidNumericValue"); return false; }
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
                bytes.assign(text.begin(), text.end()); if (bytes.size() > width) { error = i18n::tr("re.stringTooWide"); return false; } bytes.resize(width, 0);
            }
            else if (type == MemoryScanValueType::Utf16String)
            {
                bytes = utf16Bytes(text); if (bytes.size() > width) { error = i18n::tr("re.stringTooWide"); return false; } bytes.resize(width, 0);
            }
            else
            {
                if (!runtimeParseHexBytes(text, bytes, error)) return false;
                if (bytes.size() != width) { error = i18n::tr("re.byteArrayWidthMismatch"); return false; }
            }
            return runtimeWriteProcessMemory(pid, address, std::span<const std::uint8_t>(bytes.data(), bytes.size()), error);
        }

        void openInspector(PageManager& manager, const pid_t pid, const std::uintptr_t address)
        {
            requestMemoryInspector(pid, address); manager.open("native");
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
        ImGui::TextWrapped("%s",i18n::tr("re.scannerDescription"));
        drawProcessPicker("MemoryScannerProcess", _processes, _pid, _processSearch.data(), _processSearch.size(), 520.0f);
        const RuntimeProcessInfo* selected = nullptr; for (const auto& process : _processes) if (process.Pid == _pid) { selected = &process; break; }

        ImGui::SeparatorText(i18n::tr("re.manualWatch"));
        drawManualMemoryWatch(context, manager);

        ImGui::SeparatorText(i18n::tr("re.scan"));
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::BeginCombo(i18n::tr("re.valueType"), scanValueTypeLabel(static_cast<MemoryScanValueType>(_type))))
        {
            for (int i = 0; i <= static_cast<int>(MemoryScanValueType::ByteArray); ++i) if (ImGui::Selectable(scanValueTypeLabel(static_cast<MemoryScanValueType>(i)), i == _type)) _type = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo(i18n::tr("re.comparison"), scanComparisonLabel(static_cast<MemoryScanComparison>(_comparison))))
        {
            for (int i = 0; i <= static_cast<int>(MemoryScanComparison::ChangedFromTo); ++i) if (ImGui::Selectable(scanComparisonLabel(static_cast<MemoryScanComparison>(i)), i == _comparison)) _comparison = i;
            ImGui::EndCombo();
        }
        const auto comparison = static_cast<MemoryScanComparison>(_comparison);
        const bool needsA = comparison == MemoryScanComparison::Exact || comparison == MemoryScanComparison::NotEqual || comparison == MemoryScanComparison::IncreasedBy || comparison == MemoryScanComparison::DecreasedBy || comparison == MemoryScanComparison::Greater || comparison == MemoryScanComparison::Less || comparison == MemoryScanComparison::Between || comparison == MemoryScanComparison::ChangedFromTo;
        const bool needsB = comparison == MemoryScanComparison::Between || comparison == MemoryScanComparison::ChangedFromTo;
        if (needsA) { ImGui::SetNextItemWidth(360.0f); ImGui::InputText(comparison == MemoryScanComparison::ChangedFromTo ? i18n::tr("re.from") : i18n::tr("re.value"), _valueA.data(), _valueA.size()); }
        if (needsB) { ImGui::SetNextItemWidth(360.0f); ImGui::InputText(comparison == MemoryScanComparison::ChangedFromTo ? i18n::tr("re.to") : i18n::tr("re.valueB"), _valueB.data(), _valueB.size()); }
        if (integerValueType(static_cast<MemoryScanValueType>(_type))) ImGui::TextDisabled("%s",i18n::tr("re.integerInputHint"));
        if (_type == static_cast<int>(MemoryScanValueType::ByteArray)) ImGui::TextDisabled("%s",i18n::tr("re.byteArrayInputHint"));
        ImGui::Checkbox(i18n::tr("re.writableOnly"), &_writableOnly); ImGui::SameLine(); ImGui::Checkbox(i18n::tr("re.executableOnly"), &_executableOnly); ImGui::SameLine(); ImGui::Checkbox(i18n::tr("re.alignedValues"), &_aligned);
        if (_type == static_cast<int>(MemoryScanValueType::Utf8String) || _type == static_cast<int>(MemoryScanValueType::Utf16String)) { ImGui::SameLine(); ImGui::Checkbox(i18n::tr("re.caseSensitive"), &_caseSensitive); }

        const auto requestType = static_cast<MemoryScanValueType>(_type); const std::string requestA = normalizedScanInput(requestType, _valueA.data()), requestB = normalizedScanInput(requestType, _valueB.data());
        MemoryScanRequest request{_pid, requestType, comparison, requestA, requestB, _writableOnly, _executableOnly, _aligned, _caseSensitive};
        const bool scanRunning = _scanner.running();
        const bool hasSnapshot = _scanner.hasSnapshot();
        ImGui::BeginDisabled(scanRunning);
        if (ImGui::Button(i18n::tr("re.newScan"))) { _liveValues.clear(); _scanner.newScan(request, _status); }
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasSnapshot);
        if (ImGui::Button(i18n::tr("re.nextScan"))) { _liveValues.clear(); _scanner.nextScan(request, _status); }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!scanRunning);
        if (ImGui::Button(i18n::tr("common.cancel"))) _scanner.cancel();
        ImGui::EndDisabled();

        const auto stats = _scanner.stats();
        if (stats.Running) drawIndeterminateProgressBar(ImVec2(360.0f, 0.0f));
        if (stats.MiBs > 0.0) { if (stats.MiBs >= 1024.0) ImGui::Text("%.2f GiB/s", stats.MiBs / 1024.0); else ImGui::Text("%.1f MiB/s", stats.MiBs); ImGui::SameLine(); ImGui::TextDisabled("%.1f MiB in %.3f s", stats.Bytes / (1024.0 * 1024.0), stats.Seconds); }
        ImGui::Text(i18n::tr("re.candidates"), static_cast<unsigned long long>(stats.Candidates)); ImGui::SameLine(); ImGui::TextDisabled("%s%s", stats.Status.c_str(), _status.empty() ? "" : (" | " + _status).c_str());
        ImGui::SetNextItemWidth(220.0f); ImGui::SliderFloat(i18n::tr("re.visibleValueRefresh"), &_refreshHz, 0.2f, 30.0f, "%.1f Hz", ImGuiSliderFlags_Logarithmic); ImGui::SameLine(); ImGui::TextDisabled("%s",i18n::tr("re.visibleValueRefreshHint"));

        const auto rows = _scanner.results(256);
        const auto scanType = _scanner.valueType();
        const double now = runtimeSteadySeconds();
        const double refreshInterval = 1.0 / std::max(static_cast<double>(_refreshHz), 0.1);
        auto addWatch = [&](const MemoryScanResultRow& row, const LiveValue* live)
        {
            const std::size_t width = resultValueWidth(scanType, row.Value, _scanner.pid());
            if (width == 0) { _status = i18n::tr("re.watchWidthUnknown"); return; }
            for (const auto& watch : _watchList) if (watch.Pid == _scanner.pid() && watch.Address == row.Address && watch.Type == scanType) { _status = i18n::tr("re.watchAlreadyExists"); return; }
            WatchedValue watch; watch.Id = _nextWatchId++; watch.Pid = _scanner.pid(); watch.Address = row.Address; watch.Type = scanType; watch.Width = width; watch.Value = live && !live->Value.empty() ? live->Value : row.Value; watch.PreviousValue = row.Value; watch.Numeric = live ? live->Numeric : parseDisplayedNumeric(scanType, row.Value); watch.FrozenValue = watch.Value; std::snprintf(watch.AddressText.data(), watch.AddressText.size(), "0x%llX", static_cast<unsigned long long>(watch.Address)); std::snprintf(watch.ValueText.data(), watch.ValueText.size(), "%s", watch.Value.c_str()); _watchList.emplace_back(std::move(watch)); _status = i18n::tr("re.watchAdded");
        };

        if (!rows.empty() && ImGui::BeginTable("MemoryScanResults", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0.0f, 300.0f)))
        {
            ImGui::TableSetupColumn(i18n::tr("re.address"), ImGuiTableColumnFlags_WidthFixed, 190.0f); ImGui::TableSetupColumn(i18n::tr("re.value")); ImGui::TableSetupColumn(i18n::tr("re.previousValue")); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 125.0f); ImGui::TableHeadersRow();
            ImGuiListClipper clipper; clipper.Begin(static_cast<int>(rows.size()));
            while (clipper.Step()) for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
            {
                const auto& row = rows[static_cast<std::size_t>(rowIndex)];
                auto& live = _liveValues[row.Address];
                if (live.LastRefresh == 0.0 || now - live.LastRefresh >= refreshInterval)
                {
                    ValueSample sample; std::string error;
                    if (readValueSample(_scanner.pid(), row.Address, scanType, resultValueWidth(scanType, row.Value, _scanner.pid()), sample, error)) { live.Value = std::move(sample.Value); live.Numeric = sample.Numeric; }
                    else { live.Value = i18n::tr("re.unreadable"); live.Numeric.reset(); }
                    live.LastRefresh = now;
                }
                const auto previousNumeric = parseDisplayedNumeric(scanType, row.Value);
                const ImVec4 valueColor = valueChangeColor(live.Numeric, previousNumeric);
                const std::string& currentValue = live.Value.empty() ? row.Value : live.Value;
                ImGui::PushID(static_cast<int>(row.Address & 0x7fffffffULL)); ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::Selectable("##ScanResultRow", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeight()));
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openInspector(manager, _scanner.pid(), row.Address);
                if (ImGui::BeginPopupContextItem("ScanResultContext"))
                {
                    if (ImGui::IsWindowAppearing() || _scanWriteAddress != row.Address) { _scanWriteAddress = row.Address; std::snprintf(_scanWriteValue.data(), _scanWriteValue.size(), "%s", currentValue.c_str()); }
                    if (ImGui::MenuItem(i18n::tr("re.inspectMemory"))) openInspector(manager, _scanner.pid(), row.Address);
                    if (ImGui::MenuItem(i18n::tr("re.disassemble"))) openInspector(manager, _scanner.pid(), row.Address);
                    if (ImGui::MenuItem(i18n::tr("re.createSignatureHere"))) { requestSignatureMaker(_scanner.pid(), row.Address); manager.open("native"); }
                    if (ImGui::MenuItem(i18n::tr("re.watchAccesses"))) openAccessWatch(manager, _scanner.pid(), row.Address, resultValueWidth(scanType, row.Value, _scanner.pid()));
                    if (ImGui::MenuItem(i18n::tr("re.addWatchList"))) addWatch(row, &live);
                    std::uintptr_t pointedAddress = 0; const bool hasPointedAddress = parseAddress(currentValue.c_str(), pointedAddress) && pointedAddress != 0;
                    ImGui::BeginDisabled(!hasPointedAddress); if (ImGui::MenuItem(i18n::tr("re.inspectCurrentAddress"))) openInspector(manager, _scanner.pid(), pointedAddress); ImGui::EndDisabled();
                    ImGui::Separator();
                    if (ImGui::MenuItem(i18n::tr("re.copyAddress"))) { const std::string text = runtimeFormatProcessAddress(_scanner.pid(),row.Address); ImGui::SetClipboardText(text.c_str()); }
                    if (ImGui::MenuItem(i18n::tr("re.copyCurrentValue"))) ImGui::SetClipboardText(currentValue.c_str());
                    if (ImGui::MenuItem(i18n::tr("re.copyPreviousValue"))) ImGui::SetClipboardText(row.Value.c_str());
                    if (ImGui::MenuItem(i18n::tr("re.copyAddressValue"))) { const std::string text = runtimeFormatProcessAddress(_scanner.pid(),row.Address) + " = " + currentValue; ImGui::SetClipboardText(text.c_str()); }
                    ImGui::SeparatorText(i18n::tr("re.changeValue"));
                    ImGui::SetNextItemWidth(220.0f); ImGui::InputText("##ScanWriteValue", _scanWriteValue.data(), _scanWriteValue.size());
                    if (ImGui::Button(i18n::tr("re.writeValue"))) { if (writeValueSample(_scanner.pid(), row.Address, scanType, resultValueWidth(scanType, row.Value, _scanner.pid()), _scanWriteValue.data(), _status)) { live.LastRefresh = 0.0; _status = i18n::tr("re.valueWritten"); } }
                    ImGui::EndPopup();
                }
                ImGui::SameLine(); ImGui::TextUnformatted(runtimeFormatProcessAddress(_scanner.pid(),row.Address).c_str()); ImGui::TableNextColumn(); ImGui::TextColored(valueColor, "%s", currentValue.c_str()); ImGui::TableNextColumn(); ImGui::TextUnformatted(row.Value.c_str()); ImGui::TableNextColumn();
                if (ImGui::SmallButton(i18n::tr("re.inspect"))) openInspector(manager, _scanner.pid(), row.Address);
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (stats.Candidates > rows.size()) ImGui::TextDisabled(i18n::tr("re.showingCandidates"), rows.size(), static_cast<unsigned long long>(stats.Candidates));
        }

        ImGui::SeparatorText(i18n::tr("re.watchList"));
        ImGui::SetNextItemWidth(180.0f); ImGui::SliderFloat(i18n::tr("re.freezeRate"), &_freezeHz, 1.0f, 120.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic); ImGui::SameLine(); if (ImGui::Button(i18n::tr("re.clearWatchList"))) _watchList.clear();
        ImGui::TextDisabled("%s",i18n::tr("re.watchListHint"));
        std::optional<std::size_t> eraseWatch;
        if (ImGui::BeginTable("MemoryWatchList", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable, ImVec2(0.0f, 0.0f)))
        {
            ImGui::TableSetupColumn(i18n::tr("re.address"), ImGuiTableColumnFlags_WidthFixed, 190.0f); ImGui::TableSetupColumn(i18n::tr("re.type"), ImGuiTableColumnFlags_WidthFixed, 100.0f); ImGui::TableSetupColumn(i18n::tr("re.value")); ImGui::TableSetupColumn(i18n::tr("re.previous")); ImGui::TableSetupColumn(i18n::tr("re.freeze"), ImGuiTableColumnFlags_WidthFixed, 60.0f); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 65.0f); ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < _watchList.size(); ++i)
            {
                auto& watch = _watchList[i]; ImGui::PushID(static_cast<int>(watch.Id & 0x7fffffffULL));
                const double freezeInterval = 1.0 / std::max(static_cast<double>(_freezeHz), 1.0);
                if (watch.Frozen && (watch.LastFreeze == 0.0 || now - watch.LastFreeze >= freezeInterval)) { if (!writeValueSample(watch.Pid, watch.Address, watch.Type, watch.Width, watch.FrozenValue, _status)) watch.Frozen = false; watch.LastFreeze = now; }
                if (watch.LastRefresh == 0.0 || now - watch.LastRefresh >= refreshInterval)
                {
                    ValueSample sample; std::string error;
                    if (readValueSample(watch.Pid, watch.Address, watch.Type, watch.Width, sample, error)) { watch.PreviousValue = watch.Value; watch.Value = std::move(sample.Value); watch.Numeric = sample.Numeric; if (!watch.Frozen) std::snprintf(watch.ValueText.data(), watch.ValueText.size(), "%s", watch.Value.c_str()); }
                    else { watch.PreviousValue = watch.Value; watch.Value = i18n::tr("re.unreadable"); watch.Numeric.reset(); }
                    watch.LastRefresh = now;
                }
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::Selectable("##WatchRow", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeight()));
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openInspector(manager, watch.Pid, watch.Address);
                if (ImGui::BeginPopupContextItem("WatchValueContext"))
                {
                    ImGui::SeparatorText(i18n::tr("re.changeAddress")); drawAddressInput("##WatchAddress", watch.AddressText.data(), watch.AddressText.size(), watch.Pid, 260.0f);
                    if (ImGui::Button(i18n::tr("re.applyAddress"))) { std::uintptr_t address = 0; std::string error; if (!evaluateAddressExpression(watch.Pid, watch.AddressText.data(), address, error) || address == 0) _status = error.empty() ? i18n::tr("re.invalidWatchAddress") : error; else { watch.Address = address; watch.LastRefresh = 0.0; watch.LastFreeze = 0.0; _status = i18n::tr("re.watchAddressChanged"); } }
                    ImGui::SeparatorText(i18n::tr("re.changeType")); ImGui::SetNextItemWidth(180.0f);
                    if (ImGui::BeginCombo("##WatchType", scanValueTypeLabel(watch.Type)))
                    {
                        for (int typeIndex = 0; typeIndex <= static_cast<int>(MemoryScanValueType::ByteArray); ++typeIndex)
                        {
                            const auto type = static_cast<MemoryScanValueType>(typeIndex); const bool active = type == watch.Type;
                            if (ImGui::Selectable(scanValueTypeLabel(type), active)) { watch.Type = type; if (const auto fixed = fixedValueWidth(type, watch.Pid); fixed != 0) watch.Width = fixed; watch.LastRefresh = 0.0; watch.Frozen = false; }
                            if (active) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (fixedValueWidth(watch.Type, watch.Pid) == 0) { int width = static_cast<int>(watch.Width); ImGui::SetNextItemWidth(120.0f); if (ImGui::InputInt(i18n::tr("re.width"), &width)) { watch.Width = static_cast<std::size_t>(std::max(width, 1)); watch.LastRefresh = 0.0; watch.Frozen = false; } }
                    ImGui::SeparatorText(i18n::tr("re.changeValue")); ImGui::SetNextItemWidth(220.0f); ImGui::InputText("##WatchValue", watch.ValueText.data(), watch.ValueText.size());
                    if (ImGui::Button(i18n::tr("re.write"))) { if (writeValueSample(watch.Pid, watch.Address, watch.Type, watch.Width, watch.ValueText.data(), _status)) { watch.LastRefresh = 0.0; if (watch.Frozen) watch.FrozenValue = watch.ValueText.data(); _status = i18n::tr("re.watchValueWritten"); } }
                    ImGui::SameLine(); bool frozen = watch.Frozen; if (ImGui::Checkbox(i18n::tr("re.freeze"), &frozen)) { watch.Frozen = frozen; watch.LastFreeze = 0.0; if (watch.Frozen) { watch.FrozenValue = watch.ValueText.data(); if (watch.FrozenValue.empty() || watch.FrozenValue == i18n::tr("re.unreadable")) watch.FrozenValue = watch.Value; } }
                    ImGui::Separator();
                    if (ImGui::MenuItem(i18n::tr("re.inspectMemory"))) openInspector(manager, watch.Pid, watch.Address);
                    if (ImGui::MenuItem(i18n::tr("re.disassemble"))) openInspector(manager, watch.Pid, watch.Address);
                    if (ImGui::MenuItem(i18n::tr("re.createSignatureHere"))) { requestSignatureMaker(watch.Pid, watch.Address); manager.open("native"); }
                    if (ImGui::MenuItem(i18n::tr("re.watchAccesses"))) openAccessWatch(manager, watch.Pid, watch.Address, watch.Width);
                    std::uintptr_t pointedAddress = 0; const bool hasPointedAddress = parseAddress(watch.Value.c_str(), pointedAddress) && pointedAddress != 0; ImGui::BeginDisabled(!hasPointedAddress); if (ImGui::MenuItem(i18n::tr("re.inspectValueAsAddress"))) openInspector(manager, watch.Pid, pointedAddress); ImGui::EndDisabled();
                    if (ImGui::MenuItem(i18n::tr("re.copyAddress"))) { const std::string text = runtimeFormatProcessAddress(watch.Pid,watch.Address); ImGui::SetClipboardText(text.c_str()); }
                    if (ImGui::MenuItem(i18n::tr("re.copyValue"))) ImGui::SetClipboardText(watch.Value.c_str());
                    if (ImGui::MenuItem(i18n::tr("re.copyAddressValue"))) { const std::string text = runtimeFormatProcessAddress(watch.Pid,watch.Address) + " = " + watch.Value; ImGui::SetClipboardText(text.c_str()); }
                    ImGui::Separator(); if (ImGui::MenuItem(i18n::tr("re.removeWatchList"))) eraseWatch = i;
                    ImGui::EndPopup();
                }
                ImGui::SameLine(); ImGui::TextUnformatted(runtimeFormatProcessAddress(watch.Pid,watch.Address).c_str()); ImGui::TableNextColumn(); ImGui::TextUnformatted(scanValueTypeLabel(watch.Type)); ImGui::TableNextColumn(); ImGui::TextUnformatted(watch.Value.c_str()); ImGui::TableNextColumn(); ImGui::TextUnformatted(watch.PreviousValue.c_str()); ImGui::TableNextColumn(); bool frozen = watch.Frozen; if (ImGui::Checkbox("##Frozen", &frozen)) { watch.Frozen = frozen; watch.LastFreeze = 0.0; if (watch.Frozen) { watch.FrozenValue = watch.Value; std::snprintf(watch.ValueText.data(), watch.ValueText.size(), "%s", watch.Value.c_str()); } } ImGui::TableNextColumn(); if (ImGui::SmallButton(i18n::tr("re.remove"))) eraseWatch = i; ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (eraseWatch && *eraseWatch < _watchList.size()) _watchList.erase(_watchList.begin() + static_cast<std::ptrdiff_t>(*eraseWatch));
        if (_watchList.empty()) ImGui::TextDisabled("%s",i18n::tr("re.emptyWatchList"));

        ImGui::SeparatorText(i18n::tr("re.derivePattern"));
        ImGui::TextDisabled("%s",i18n::tr("re.derivePatternDescription"));
        drawAddressInput(i18n::tr("re.start"), _rangeStart.data(), _rangeStart.size(), _pid, 220.0f); ImGui::SameLine(); drawAddressInput(i18n::tr("re.end"), _rangeEnd.data(), _rangeEnd.size(), _pid, 220.0f);
        ImGui::Checkbox(i18n::tr("re.wildcardRelocations"), &_wildcardRelocations);
        if (ImGui::Button(i18n::tr("re.derivePatternAction")))
        {
            std::uintptr_t start = 0, end = 0; std::string error;
            if (!evaluateAddressExpression(_pid, _rangeStart.data(), start, error) || !evaluateAddressExpression(_pid, _rangeEnd.data(), end, error) || start == 0 || end <= start) _status = error.empty() ? i18n::tr("re.invalidRange") : error;
            else _derivedPattern = deriveRuntimeBytePattern(_pid, start, end, _wildcardRelocations, _status);
        }
        if (!_derivedPattern.empty())
        {
            ImGui::InputTextMultiline(i18n::tr("re.derivedPattern"), _derivedPattern.data(), _derivedPattern.size() + 1, ImVec2(-1.0f, 90.0f), ImGuiInputTextFlags_ReadOnly);
            if (ImGui::Button(i18n::tr("re.copyPattern"))) ImGui::SetClipboardText(_derivedPattern.c_str()); ImGui::SameLine();
            if (ImGui::Button(i18n::tr("re.createNativeBinding")))
            {
                auto& binding = context.runtimeBindings.add(); std::snprintf(binding.Name, sizeof(binding.Name), "%s", "Derived signature"); binding.Source = RuntimeSourceKind::NativeAddress; binding.ProcessId = _pid; if (selected) std::snprintf(binding.ProcessName, sizeof(binding.ProcessName), "%s", selected->Name.c_str()); binding.AddressMode = ProcessAddressMode::Signature; binding.SignaturePatternKind = RuntimeSignaturePatternKind::HexadecimalPattern; binding.SignatureExecutableOnly = true; binding.WriteMaterial = false; binding.Clamp = false; binding.SmoothingHz = 0.0f; std::snprintf(binding.Signature, sizeof(binding.Signature), "%s", _derivedPattern.c_str()); _status = i18n::tr("re.nativeBindingCreated");
            }
        }
    }
}
