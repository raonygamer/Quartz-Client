#include "quartz/client/ui/ReverseEngineeringTools.hpp"
#include "quartz/client/ui/AddressInput.hpp"
#include "quartz/client/ui/ObjectExperiments.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/ProcessPicker.hpp"
#include "quartz/client/ui/MemoryInspector.hpp"
#include "quartz/client/ui/ReverseEngineeringNavigation.hpp"
#include "quartz/client/ui/pages/MemoryScannerPage.hpp"
#include "quartz/client/ui/pages/MemoryWatchPage.hpp"
#include "quartz/client/native/SignatureScanner.hpp"
#include <algorithm>
#include <array>
#include <cstdio>

namespace quartz::client::ui
{
    namespace
    {
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

        struct ManualWatchState
        {
            std::vector<RuntimeProcessInfo> Processes;
            pid_t Pid = 0;
            std::array<char, 256> Search{};
            std::array<char, 256> Address{};
            int Type = static_cast<int>(MemoryScanValueType::I32);
            int Width = 4;
            std::string Status;
        };

        struct QuickSignatureState
        {
            std::vector<RuntimeProcessInfo> Processes;
            pid_t Pid = 0;
            std::array<char, 256> Search{};
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
    }

    void drawManualMemoryWatch(PageContext&, PageManager& manager)
    {
        static ManualWatchState state;
        ImGui::TextWrapped("Pin an arbitrary process address directly into the scanner watch list without running a value scan first.");
        drawProcessPicker("ManualWatchProcess", state.Processes, state.Pid, state.Search.data(), state.Search.size(), 520.0f);
        drawAddressInput("Address", state.Address.data(), state.Address.size(), state.Pid, 300.0f); ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f);
        if (ImGui::BeginCombo("Type", memoryScanValueTypeName(static_cast<MemoryScanValueType>(state.Type))))
        {
            for (int i = 0; i <= static_cast<int>(MemoryScanValueType::ByteArray); ++i) if (ImGui::Selectable(memoryScanValueTypeName(static_cast<MemoryScanValueType>(i)), i == state.Type)) { state.Type = i; if (const auto width = fixedWatchWidth(static_cast<MemoryScanValueType>(i)); width != 0) state.Width = static_cast<int>(width); }
            ImGui::EndCombo();
        }
        if (fixedWatchWidth(static_cast<MemoryScanValueType>(state.Type)) == 0) { ImGui::SameLine(); ImGui::SetNextItemWidth(100.0f); ImGui::InputInt("Width", &state.Width); state.Width = std::max(state.Width, 1); }
        if (ImGui::Button("Add to watch list"))
        {
            std::uintptr_t address = 0; std::string error;
            if (state.Pid <= 0) state.Status = "select a process";
            else if (!evaluateAddressExpression(state.Pid, state.Address.data(), address, error) || address == 0) state.Status = error.empty() ? "invalid address" : error;
            else state.Status = addWatch(manager, state.Pid, address, static_cast<MemoryScanValueType>(state.Type), static_cast<std::size_t>(state.Width)) ? "watch added" : "watch already exists or scanner page is unavailable";
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
        drawProcessPicker("QuickSignatureProcess", state.Processes, state.Pid, state.Search.data(), state.Search.size(), 520.0f);
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

    void drawObjectModelDebugger(PageContext& context, PageManager& manager) { drawObjectExperiments(context, manager); }
}
