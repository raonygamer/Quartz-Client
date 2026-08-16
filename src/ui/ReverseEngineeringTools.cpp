#include "quartz/client/ui/ReverseEngineeringTools.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/MemoryInspector.hpp"
#include "quartz/client/ui/ReverseEngineeringNavigation.hpp"
#include "quartz/client/ui/SignatureMaker.hpp"
#include "quartz/client/ui/pages/MemoryScannerPage.hpp"
#include "quartz/client/ui/pages/MemoryWatchPage.hpp"
#include "quartz/client/native/SignatureScanner.hpp"
#include "quartz/client/native/NativeDisassembly.hpp"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <type_traits>
#include <unordered_map>

namespace quartz::client::ui
{
    namespace
    {
        bool parseAddress(const std::string_view text, std::uintptr_t& value)
        {
            if (text.empty()) return false;
            std::string_view view = text; int base = 10;
            if (view.starts_with("0x") || view.starts_with("0X")) { view.remove_prefix(2); base = 16; }
            const auto [ptr, ec] = std::from_chars(view.data(), view.data() + view.size(), value, base); return ec == std::errc{} && ptr == view.data() + view.size();
        }

        std::size_t fixedWatchWidth(const MemoryScanValueType type) noexcept
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

        const RuntimeProcessInfo* findProcess(const std::vector<RuntimeProcessInfo>& processes, const pid_t pid)
        {
            const auto it = std::ranges::find(processes, pid, &RuntimeProcessInfo::Pid); return it == processes.end() ? nullptr : &*it;
        }

        bool drawProcessSelector(const char* id, std::vector<RuntimeProcessInfo>& processes, pid_t& pid)
        {
            if (processes.empty()) processes = enumerateRuntimeProcesses();
            bool changed = false; const RuntimeProcessInfo* selected = findProcess(processes, pid);
            ImGui::PushID(id);
            if (ImGui::SmallButton("Refresh")) { processes = enumerateRuntimeProcesses(); selected = findProcess(processes, pid); }
            ImGui::SameLine(); ImGui::SetNextItemWidth(420.0f);
            if (ImGui::BeginCombo("Process", selected ? runtimeProcessDisplayTitle(*selected).c_str() : "<select process>"))
            {
                for (const auto& process : processes)
                {
                    const bool active = process.Pid == pid; const std::string label = runtimeProcessDisplayTitle(process) + "  [" + std::to_string(process.Pid) + "]";
                    if (ImGui::Selectable(label.c_str(), active)) { pid = process.Pid; changed = true; }
                    if (active) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopID(); return changed;
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

        bool addWatch(PageManager& manager, const pid_t pid, const std::uintptr_t address, const MemoryScanValueType type, const std::size_t width)
        {
            auto* page = dynamic_cast<MemoryScannerPage*>(manager.find("memory-scanner")); if (!page) return false;
            const bool added = page->addWatch(pid, address, type, width); manager.open("memory-scanner"); return added;
        }

        std::optional<ProcessValueType> processValueType(const MemoryScanValueType type) noexcept
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

        void createDirectBinding(RuntimeBindingEngine& engine, const RuntimeProcessInfo* process, const pid_t pid, const std::uintptr_t address, const MemoryScanValueType type, const char* name)
        {
            const auto valueType = processValueType(type); if (!valueType) return;
            auto& binding = engine.add(); std::snprintf(binding.Name, sizeof(binding.Name), "%s", name); binding.Source = RuntimeSourceKind::NativeProcess; binding.ProcessId = static_cast<int>(pid); if (process) std::snprintf(binding.ProcessName, sizeof(binding.ProcessName), "%s", process->Name.c_str()); binding.AddressMode = ProcessAddressMode::AddressChain; std::snprintf(binding.Address, sizeof(binding.Address), "0x%llX", static_cast<unsigned long long>(address)); binding.ValueType = *valueType; binding.WriteMaterial = false; binding.Normalize = false; binding.Clamp = false; binding.SmoothingHz = 0.0f; binding.UpdateHz = 30.0f; binding.AutoReattach = false;
        }

        MemoryScanValueType objectWatchType(const RuntimeObjectFieldType type) noexcept
        {
            switch (type)
            {
            case RuntimeObjectFieldType::U8: return MemoryScanValueType::U8;
            case RuntimeObjectFieldType::I8: return MemoryScanValueType::I8;
            case RuntimeObjectFieldType::U16: return MemoryScanValueType::U16;
            case RuntimeObjectFieldType::I16: return MemoryScanValueType::I16;
            case RuntimeObjectFieldType::U32: return MemoryScanValueType::U32;
            case RuntimeObjectFieldType::I32: return MemoryScanValueType::I32;
            case RuntimeObjectFieldType::U64: return MemoryScanValueType::U64;
            case RuntimeObjectFieldType::I64: return MemoryScanValueType::I64;
            case RuntimeObjectFieldType::Float: return MemoryScanValueType::Float;
            case RuntimeObjectFieldType::Double: return MemoryScanValueType::Double;
            case RuntimeObjectFieldType::Bool: return MemoryScanValueType::Bool;
            case RuntimeObjectFieldType::Pointer: case RuntimeObjectFieldType::CStringPointer: case RuntimeObjectFieldType::WStringPointer: return MemoryScanValueType::Pointer;
            case RuntimeObjectFieldType::FixedCString: return MemoryScanValueType::Utf8String;
            case RuntimeObjectFieldType::FixedWString: return MemoryScanValueType::Utf16String;
            default: return MemoryScanValueType::ByteArray;
            }
        }

        std::size_t debugFieldSize(const RuntimeObjectField& field, const RuntimeX86Mode mode) noexcept
        {
            if (field.Type == RuntimeObjectFieldType::Pointer || field.Type == RuntimeObjectFieldType::CStringPointer || field.Type == RuntimeObjectFieldType::WStringPointer) return mode == RuntimeX86Mode::X86 ? 4 : 8;
            return runtimeObjectFieldSize(field);
        }

        std::size_t debugNaturalAlignment(const RuntimeObjectField& field, const RuntimeX86Mode mode) noexcept
        {
            if (runtimeObjectFieldIsFiller(field.Type) || field.Type == RuntimeObjectFieldType::FixedCString || field.Type == RuntimeObjectFieldType::FixedWString) return 1;
            return std::min<std::size_t>(debugFieldSize(field, mode), mode == RuntimeX86Mode::X86 ? 4 : 8);
        }

        std::size_t debugObjectFieldOffset(const RuntimeObjectDescriptor& object, const RuntimeObjectField& wanted, const RuntimeX86Mode mode) noexcept
        {
            std::size_t cursor = 0; const std::size_t pack = runtimeObjectPackingBytes(object.Packing);
            for (const auto& field : object.Fields)
            {
                const std::size_t explicitAlignment = runtimeObjectAlignmentBytes(field.Alignment); std::size_t alignment = explicitAlignment ? explicitAlignment : debugNaturalAlignment(field, mode); if (pack) alignment = std::min(alignment, pack); alignment = std::max<std::size_t>(alignment, 1);
                const std::size_t offset = field.ManualOffset ? static_cast<std::size_t>(std::max(field.Offset, 0)) : runtimeAlignUp(cursor, alignment); if (field.Id == wanted.Id) return offset; cursor = std::max(cursor, offset + debugFieldSize(field, mode));
            }
            return std::numeric_limits<std::size_t>::max();
        }

        template<typename T>
        std::string scalarString(const std::span<const std::uint8_t> bytes)
        {
            if (bytes.size() < sizeof(T)) return "<short read>"; T value{}; std::memcpy(&value, bytes.data(), sizeof(T)); std::ostringstream out; if constexpr (std::is_floating_point_v<T>) out << std::setprecision(12) << value; else if constexpr (sizeof(T) == 1 && std::is_integral_v<T>) out << static_cast<int>(value); else out << value; return out.str();
        }

        std::string formatObjectValue(const RuntimeObjectField& field, const std::span<const std::uint8_t> bytes, const RuntimeX86Mode mode)
        {
            switch (field.Type)
            {
            case RuntimeObjectFieldType::U8: return scalarString<std::uint8_t>(bytes);
            case RuntimeObjectFieldType::I8: return scalarString<std::int8_t>(bytes);
            case RuntimeObjectFieldType::U16: return scalarString<std::uint16_t>(bytes);
            case RuntimeObjectFieldType::I16: return scalarString<std::int16_t>(bytes);
            case RuntimeObjectFieldType::U32: return scalarString<std::uint32_t>(bytes);
            case RuntimeObjectFieldType::I32: return scalarString<std::int32_t>(bytes);
            case RuntimeObjectFieldType::U64: return scalarString<std::uint64_t>(bytes);
            case RuntimeObjectFieldType::I64: return scalarString<std::int64_t>(bytes);
            case RuntimeObjectFieldType::Float: return scalarString<float>(bytes);
            case RuntimeObjectFieldType::Double: return scalarString<double>(bytes);
            case RuntimeObjectFieldType::Bool: return !bytes.empty() && bytes[0] ? "true" : "false";
            case RuntimeObjectFieldType::Pointer: case RuntimeObjectFieldType::CStringPointer: case RuntimeObjectFieldType::WStringPointer:
            {
                std::uintptr_t value = 0; if (mode == RuntimeX86Mode::X86) { std::uint32_t v = 0; if (bytes.size() < 4) return "<short read>"; std::memcpy(&v, bytes.data(), 4); value = v; } else { std::uint64_t v = 0; if (bytes.size() < 8) return "<short read>"; std::memcpy(&v, bytes.data(), 8); value = static_cast<std::uintptr_t>(v); } return runtimeHexAddress(value);
            }
            case RuntimeObjectFieldType::FixedCString:
            {
                const auto end = std::find(bytes.begin(), bytes.end(), 0); return std::string(reinterpret_cast<const char*>(bytes.data()), static_cast<std::size_t>(end - bytes.begin()));
            }
            case RuntimeObjectFieldType::FixedWString:
            {
                std::string result; for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) { const std::uint16_t cp = static_cast<std::uint16_t>(bytes[i] | (bytes[i + 1] << 8)); if (cp == 0) break; result.push_back(cp >= 32 && cp < 127 ? static_cast<char>(cp) : '.'); } return result;
            }
            default: return runtimeFormatHexBytes(bytes);
            }
        }

        struct ManualWatchState
        {
            std::vector<RuntimeProcessInfo> Processes;
            pid_t Pid = 0;
            std::array<char, 32> Address{};
            int Type = static_cast<int>(MemoryScanValueType::I32);
            int Width = 4;
            std::string Status;
        };

        struct QuickSignatureState
        {
            std::vector<RuntimeProcessInfo> Processes;
            pid_t Pid = 0;
            std::array<char, 2048> Pattern{};
            bool ExecutableOnly = true;
            std::shared_ptr<SignatureScanState> Scan;
            SignatureScanResult Result;
            bool HasResult = false;
            std::uint64_t Generation = 0;
            int WatchType = static_cast<int>(MemoryScanValueType::I32);
            int WatchWidth = 4;
            std::string Status;
        };

        struct ObjectDebugSample
        {
            std::uintptr_t Address = 0;
            std::string Value;
            std::string Raw;
            bool Readable = false;
        };

        struct ObjectDebuggerState
        {
            std::vector<RuntimeProcessInfo> Processes;
            pid_t Pid = 0;
            std::uint64_t DescriptorId = 0;
            std::array<char, 32> Address{};
            std::uintptr_t Base = 0;
            float RefreshHz = 10.0f;
            double LastRefresh = 0.0;
            std::unordered_map<std::uint64_t, ObjectDebugSample> Samples;
            std::string Status;
        };
    }

    void drawManualMemoryWatch(PageContext&, PageManager& manager)
    {
        static ManualWatchState state;
        ImGui::TextWrapped("Pin an arbitrary process address directly into the scanner watch list without running a value scan first.");
        drawProcessSelector("ManualWatchProcess", state.Processes, state.Pid);
        ImGui::SetNextItemWidth(200.0f); ImGui::InputText("Address", state.Address.data(), state.Address.size()); ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f);
        if (ImGui::BeginCombo("Type", memoryScanValueTypeName(static_cast<MemoryScanValueType>(state.Type))))
        {
            for (int i = 0; i <= static_cast<int>(MemoryScanValueType::ByteArray); ++i) if (ImGui::Selectable(memoryScanValueTypeName(static_cast<MemoryScanValueType>(i)), i == state.Type)) { state.Type = i; if (const auto width = fixedWatchWidth(static_cast<MemoryScanValueType>(i)); width != 0) state.Width = static_cast<int>(width); }
            ImGui::EndCombo();
        }
        if (fixedWatchWidth(static_cast<MemoryScanValueType>(state.Type)) == 0) { ImGui::SameLine(); ImGui::SetNextItemWidth(100.0f); ImGui::InputInt("Width", &state.Width); state.Width = std::max(state.Width, 1); }
        if (ImGui::Button("Add to watch list"))
        {
            std::uintptr_t address = 0; if (state.Pid <= 0) state.Status = "select a process"; else if (!parseAddress(state.Address.data(), address) || address == 0) state.Status = "invalid address"; else state.Status = addWatch(manager, state.Pid, address, static_cast<MemoryScanValueType>(state.Type), static_cast<std::size_t>(state.Width)) ? "watch added" : "watch already exists or scanner page is unavailable";
        }
        if (!state.Status.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", state.Status.c_str()); }
    }

    void drawQuickSignatureSearch(PageContext& context, PageManager& manager)
    {
        static QuickSignatureState state;
        if (state.Scan)
        {
            SignatureScanResult result; if (tryGetSignatureScanResult(state.Scan, result)) { state.Result = std::move(result); state.HasResult = state.Result.Found; state.Status = state.Result.Cancelled ? "cancelled" : state.Result.Found ? "match found" : state.Result.Error.empty() ? "pattern not found" : state.Result.Error; state.Scan.reset(); }
        }
        ImGui::TextWrapped("Fast first-match hexadecimal signature search using the shared asynchronous libhat scanner. Results can immediately feed the inspector, watch list, hardware watcher or runtime bindings.");
        drawProcessSelector("QuickSignatureProcess", state.Processes, state.Pid);
        ImGui::InputTextMultiline("Pattern", state.Pattern.data(), state.Pattern.size(), ImVec2(-1.0f, 70.0f));
        ImGui::Checkbox("Executable mappings only", &state.ExecutableOnly);
        const bool running = static_cast<bool>(state.Scan);
        ImGui::BeginDisabled(running);
        if (ImGui::Button("Search signature"))
        {
            std::vector<std::uint8_t> bytes, masks; std::string error;
            if (state.Pid <= 0) state.Status = "select a process";
            else if (!parseRuntimeHexPattern(state.Pattern.data(), bytes, masks, error)) state.Status = std::move(error);
            else
            {
                auto regions = enumerateRuntimeRegions(state.Pid); std::erase_if(regions, [&](const RuntimeProcessRegion& region) { return !region.Readable || (state.ExecutableOnly && !region.Executable); });
                if (regions.empty()) state.Status = "no readable mappings matched the selected filter";
                else { state.Result = {}; state.HasResult = false; state.Status = "scanning"; state.Scan = startSignatureScan(state.Pid, std::move(regions), std::move(bytes), std::move(masks), state.ExecutableOnly, ++state.Generation); }
            }
        }
        ImGui::EndDisabled();
        if (running)
        {
            ImGui::SameLine(); if (ImGui::Button("Cancel")) cancelSignatureScan(state.Scan); const double speed = signatureScanAverageMiBs(state.Scan); const auto scanned = state.Scan->ScannedBytes.load(std::memory_order_relaxed); const auto total = state.Scan->TotalBytes; if (total != 0) ImGui::ProgressBar(static_cast<float>(static_cast<double>(scanned) / static_cast<double>(total)), ImVec2(360.0f, 0.0f)); else drawIndeterminateProgressBar(ImVec2(360.0f, 0.0f)); if (speed > 0.0) ImGui::TextDisabled("%.1f MiB/s | %.1f / %.1f MiB", speed, scanned / 1048576.0, total / 1048576.0);
        }
        if (!state.Status.empty()) ImGui::TextDisabled("%s", state.Status.c_str());
        if (!state.HasResult) return;

        const auto address = state.Result.MatchAddress; const RuntimeProcessInfo* process = findProcess(state.Processes, state.Pid);
        ImGui::SeparatorText("Match"); ImGui::Selectable(runtimeHexAddress(address).c_str(), false); if (ImGui::BeginPopupContextItem("QuickSignatureResultContext"))
        {
            if (ImGui::MenuItem("Inspect memory")) openInspector(manager, state.Pid, address);
            if (ImGui::MenuItem("Disassemble")) openInspector(manager, state.Pid, address);
            if (ImGui::MenuItem("Add to watch list")) addWatch(manager, state.Pid, address, static_cast<MemoryScanValueType>(state.WatchType), static_cast<std::size_t>(state.WatchWidth));
            if (ImGui::MenuItem("Watch accesses")) openAccessWatch(manager, state.Pid, address, static_cast<std::size_t>(std::max(state.WatchWidth, 1)));
            if (ImGui::MenuItem("Copy address")) { const std::string text = runtimeHexAddress(address); ImGui::SetClipboardText(text.c_str()); }
            ImGui::EndPopup();
        }
        ImGui::SameLine(); ImGui::TextDisabled("%.3f s, %.1f MiB scanned", state.Result.DurationSeconds, state.Result.ScannedBytes / 1048576.0);
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::BeginCombo("Watch as", memoryScanValueTypeName(static_cast<MemoryScanValueType>(state.WatchType))))
        {
            for (int i = 0; i <= static_cast<int>(MemoryScanValueType::ByteArray); ++i) if (ImGui::Selectable(memoryScanValueTypeName(static_cast<MemoryScanValueType>(i)), i == state.WatchType)) { state.WatchType = i; if (const auto width = fixedWatchWidth(static_cast<MemoryScanValueType>(i)); width != 0) state.WatchWidth = static_cast<int>(width); }
            ImGui::EndCombo();
        }
        if (fixedWatchWidth(static_cast<MemoryScanValueType>(state.WatchType)) == 0) { ImGui::SameLine(); ImGui::SetNextItemWidth(100.0f); ImGui::InputInt("Width", &state.WatchWidth); state.WatchWidth = std::max(state.WatchWidth, 1); }
        if (ImGui::Button("Inspect")) openInspector(manager, state.Pid, address); ImGui::SameLine(); if (ImGui::Button("Disassemble")) openInspector(manager, state.Pid, address); ImGui::SameLine(); if (ImGui::Button("Add to watch list")) addWatch(manager, state.Pid, address, static_cast<MemoryScanValueType>(state.WatchType), static_cast<std::size_t>(state.WatchWidth)); ImGui::SameLine(); if (ImGui::Button("Watch accesses")) openAccessWatch(manager, state.Pid, address, static_cast<std::size_t>(std::max(state.WatchWidth, 1)));
        if (ImGui::Button("Create signature address binding"))
        {
            auto& binding = context.runtimeBindings.add(); std::snprintf(binding.Name, sizeof(binding.Name), "Signature %llX", static_cast<unsigned long long>(address)); binding.Source = RuntimeSourceKind::NativeAddress; binding.ProcessId = static_cast<int>(state.Pid); if (process) std::snprintf(binding.ProcessName, sizeof(binding.ProcessName), "%s", process->Name.c_str()); binding.AddressMode = ProcessAddressMode::Signature; binding.SignaturePatternKind = RuntimeSignaturePatternKind::HexadecimalPattern; binding.SignatureExecutableOnly = state.ExecutableOnly; binding.SignatureResolve = SignatureResultMode::MatchAddress; std::snprintf(binding.Signature, sizeof(binding.Signature), "%s", state.Pattern.data()); binding.WriteMaterial = false; binding.Clamp = false; binding.SmoothingHz = 0.0f; binding.AutoReattach = false; state.Status = "created signature address binding";
        }
        ImGui::SameLine(); if (ImGui::Button("Copy address")) { const std::string text = runtimeHexAddress(address); ImGui::SetClipboardText(text.c_str()); }
    }

    void drawObjectModelDebugger(PageContext& context, PageManager& manager)
    {
        static ObjectDebuggerState state;
        auto& engine = context.runtimeBindings;
        ImGui::TextWrapped("Bind a reusable object descriptor to an arbitrary live process address for debugging without first creating a persistent pointer instance. Fields refresh in-place and can escape into the normal memory tools.");
        if (drawProcessSelector("ObjectDebuggerProcess", state.Processes, state.Pid)) { state.LastRefresh = 0.0; state.Samples.clear(); }
        RuntimeObjectDescriptor* descriptor = engine.findObject(state.DescriptorId);
        ImGui::SetNextItemWidth(320.0f);
        if (ImGui::BeginCombo("Object model", descriptor ? descriptor->Name : "<select descriptor>"))
        {
            for (auto& candidate : engine.objects()) { const bool active = candidate.Id == state.DescriptorId; if (ImGui::Selectable(candidate.Name, active)) { state.DescriptorId = candidate.Id; descriptor = &candidate; state.LastRefresh = 0.0; state.Samples.clear(); } if (active) ImGui::SetItemDefaultFocus(); }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(200.0f); ImGui::InputText("Base address", state.Address.data(), state.Address.size()); ImGui::SameLine();
        if (ImGui::Button("Bind address")) { std::uintptr_t address = 0; if (!parseAddress(state.Address.data(), address) || address == 0) state.Status = "invalid base address"; else { state.Base = address; state.LastRefresh = 0.0; state.Samples.clear(); state.Status = "object model bound"; } }
        ImGui::SameLine(); if (ImGui::Button("Use inspector address")) { const auto& inspector = runtimeMemoryInspectorState(); if (inspector.Pid > 0 && inspector.Address != 0) { state.Pid = inspector.Pid; state.Base = inspector.Address; std::snprintf(state.Address.data(), state.Address.size(), "0x%llX", static_cast<unsigned long long>(state.Base)); state.LastRefresh = 0.0; state.Samples.clear(); state.Status = "using inspector address"; } }
        ImGui::SetNextItemWidth(180.0f); ImGui::SliderFloat("Refresh rate", &state.RefreshHz, 0.5f, 60.0f, "%.1f Hz", ImGuiSliderFlags_Logarithmic);
        if (!state.Status.empty()) ImGui::TextDisabled("%s", state.Status.c_str());
        if (!descriptor || state.Pid <= 0 || state.Base == 0) { ImGui::TextDisabled("Select a process/model and bind a non-zero address to begin."); return; }

        const RuntimeX86Mode mode = runtimeProcessX86Mode(state.Pid); const double now = runtimeSteadySeconds(); const bool refresh = state.LastRefresh == 0.0 || now - state.LastRefresh >= 1.0 / std::max(static_cast<double>(state.RefreshHz), 0.1);
        if (refresh)
        {
            for (const auto& field : descriptor->Fields)
            {
                if (!field.Enabled || runtimeObjectFieldIsFiller(field.Type)) continue;
                const std::size_t offset = debugObjectFieldOffset(*descriptor, field, mode); if (offset == std::numeric_limits<std::size_t>::max()) continue;
                auto& sample = state.Samples[field.Id]; sample.Address = state.Base + offset; const std::size_t width = std::clamp<std::size_t>(debugFieldSize(field, mode), 1, 4096); std::vector<std::uint8_t> bytes(width); std::string error;
                sample.Readable = readProcessMemoryBlock(state.Pid, sample.Address, bytes, error); if (sample.Readable) { sample.Value = formatObjectValue(field, bytes, mode); sample.Raw = runtimeFormatHexBytes(bytes); } else { sample.Value = "<unreadable>"; sample.Raw = std::move(error); }
            }
            state.LastRefresh = now;
        }

        ImGui::Text("%s @ 0x%llX", descriptor->Name, static_cast<unsigned long long>(state.Base)); ImGui::SameLine(); ImGui::TextDisabled("PID %d | %s", static_cast<int>(state.Pid), runtimeX86ModeName(mode));
        if (ImGui::Button("Inspect base")) openInspector(manager, state.Pid, state.Base); ImGui::SameLine();
        if (ImGui::Button("Save as pointer instance"))
        {
            const RuntimeProcessInfo* process = findProcess(state.Processes, state.Pid); auto& base = engine.add(); std::snprintf(base.Name, sizeof(base.Name), "%s debug base", descriptor->Name); base.Source = RuntimeSourceKind::NativeAddress; base.ProcessId = static_cast<int>(state.Pid); if (process) std::snprintf(base.ProcessName, sizeof(base.ProcessName), "%s", process->Name.c_str()); base.AddressMode = ProcessAddressMode::AddressChain; std::snprintf(base.Address, sizeof(base.Address), "0x%llX", static_cast<unsigned long long>(state.Base)); base.WriteMaterial = false; base.Clamp = false; base.SmoothingHz = 0.0f; base.AutoReattach = false; auto& pointer = engine.addPointer(); pointer.DescriptorId = descriptor->Id; pointer.BaseBindingId = base.Id; pointer.ProcessBindingId = base.Id; std::snprintf(pointer.Name, sizeof(pointer.Name), "%s debug instance", descriptor->Name); state.Status = "saved persistent pointer instance";
        }

        if (ImGui::BeginTable("ObjectDebuggerFields", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 360.0f)))
        {
            ImGui::TableSetupColumn("Field"); ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 80.0f); ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 145.0f); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 105.0f); ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("Raw bytes"); ImGui::TableHeadersRow();
            for (const auto& field : descriptor->Fields)
            {
                if (!field.Enabled || runtimeObjectFieldIsFiller(field.Type)) continue; const auto sampleIt = state.Samples.find(field.Id); if (sampleIt == state.Samples.end()) continue; const auto& sample = sampleIt->second; const std::size_t offset = debugObjectFieldOffset(*descriptor, field, mode); const auto watchType = objectWatchType(field.Type); const std::size_t width = debugFieldSize(field, mode);
                ImGui::PushID(static_cast<int>(field.Id & 0x7fffffffULL)); ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::Selectable("##ObjectDebugRow", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeight()));
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openInspector(manager, state.Pid, sample.Address);
                if (ImGui::BeginPopupContextItem("ObjectDebugContext"))
                {
                    if (ImGui::MenuItem("Inspect field")) openInspector(manager, state.Pid, sample.Address);
                    if (ImGui::MenuItem("Disassemble at field")) openInspector(manager, state.Pid, sample.Address);
                    if (ImGui::MenuItem("Add field to watch list")) addWatch(manager, state.Pid, sample.Address, watchType, width);
                    if (ImGui::MenuItem("Watch field accesses")) openAccessWatch(manager, state.Pid, sample.Address, width);
                    std::uintptr_t pointed = 0; const bool pointerValue = parseAddress(sample.Value, pointed) && pointed != 0; ImGui::BeginDisabled(!pointerValue); if (ImGui::MenuItem("Inspect value as address")) openInspector(manager, state.Pid, pointed); ImGui::EndDisabled();
                    if (ImGui::MenuItem("Copy address")) { const std::string text = runtimeHexAddress(sample.Address); ImGui::SetClipboardText(text.c_str()); }
                    if (ImGui::MenuItem("Copy value")) ImGui::SetClipboardText(sample.Value.c_str());
                    ImGui::EndPopup();
                }
                ImGui::SameLine(); ImGui::TextUnformatted(field.Name); ImGui::TableNextColumn(); ImGui::Text("+0x%zX", offset); ImGui::TableNextColumn(); ImGui::Text("0x%llX", static_cast<unsigned long long>(sample.Address)); ImGui::TableNextColumn(); ImGui::TextUnformatted(runtimeObjectFieldTypeName(field.Type)); ImGui::TableNextColumn(); if (sample.Readable) ImGui::TextUnformatted(sample.Value.c_str()); else ImGui::TextDisabled("%s", sample.Value.c_str()); ImGui::TableNextColumn(); if (sample.Readable) ImGui::TextUnformatted(sample.Raw.c_str()); else ImGui::TextDisabled("%s", sample.Raw.c_str()); ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Right-click any field for watch/inspect/binding actions. Debug layout uses the target process pointer width, so 32-bit object models do not inherit the 64-bit host pointer size.");
    }
}
