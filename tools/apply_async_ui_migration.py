from pathlib import Path
import re


def write(path: str, content: str) -> None:
    file = Path(path)
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(content)


def replace_once(path: str, old: str, new: str) -> None:
    file = Path(path)
    text = file.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one replacement, found {count}: {old[:80]!r}")
    file.write_text(text.replace(old, new, 1))


# ---- Generic async job pool -------------------------------------------------
write("include/quartz/client/async/ThreadPool.hpp", r'''#pragma once
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace quartz::client::async
{
    enum class TaskPriority : std::uint8_t
    {
        Background,
        Normal,
        High,
        Count
    };

    class ThreadPool
    {
    public:
        using Task = std::function<void(std::stop_token)>;

        static std::size_t defaultWorkerCount() noexcept;

        explicit ThreadPool(std::size_t workerCount = defaultWorkerCount());
        ~ThreadPool();

        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;
        ThreadPool(ThreadPool&&) = delete;
        ThreadPool& operator=(ThreadPool&&) = delete;

        void submit(Task task, TaskPriority priority = TaskPriority::Normal);
        [[nodiscard]] std::size_t workerCount() const noexcept { return _workerCount; }

    private:
        [[nodiscard]] bool hasTasks() const noexcept;
        bool tryPop(Task& task) noexcept;
        void workerLoop(std::stop_token stop);

        std::size_t _workerCount = 0;
        std::array<std::deque<Task>, static_cast<std::size_t>(TaskPriority::Count)> _queues;
        std::mutex _mutex;
        std::condition_variable_any _condition;
        // Keep workers last: they stop/join before queue/synchronization state is destroyed.
        std::vector<std::jthread> _workers;
    };

    ThreadPool& globalThreadPool();
}
''')

write("src/async/ThreadPool.cpp", r'''#include "quartz/client/async/ThreadPool.hpp"
#include <algorithm>

namespace quartz::client::async
{
    std::size_t ThreadPool::defaultWorkerCount() noexcept
    {
        const std::size_t hardware = std::max<std::size_t>(std::thread::hardware_concurrency(), 2);
        return std::clamp<std::size_t>((hardware + 2) / 3, 2, 4);
    }

    ThreadPool::ThreadPool(const std::size_t workerCount) : _workerCount(std::max<std::size_t>(workerCount, 1))
    {
        _workers.reserve(_workerCount);
        for (std::size_t i = 0; i < _workerCount; ++i) _workers.emplace_back([this](std::stop_token stop) { workerLoop(stop); });
    }

    ThreadPool::~ThreadPool()
    {
        for (auto& worker : _workers) worker.request_stop();
        _condition.notify_all();
        _workers.clear();
    }

    void ThreadPool::submit(Task task, const TaskPriority priority)
    {
        if (!task) return;
        {
            std::lock_guard lock(_mutex);
            _queues[static_cast<std::size_t>(priority)].emplace_back(std::move(task));
        }
        _condition.notify_one();
    }

    bool ThreadPool::hasTasks() const noexcept
    {
        for (const auto& queue : _queues) if (!queue.empty()) return true;
        return false;
    }

    bool ThreadPool::tryPop(Task& task) noexcept
    {
        for (std::size_t i = _queues.size(); i-- > 0;)
        {
            auto& queue = _queues[i];
            if (queue.empty()) continue;
            task = std::move(queue.front());
            queue.pop_front();
            return true;
        }
        return false;
    }

    void ThreadPool::workerLoop(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            Task task;
            {
                std::unique_lock lock(_mutex);
                if (!_condition.wait(lock, stop, [this] { return hasTasks(); })) return;
                if (!tryPop(task)) continue;
            }
            try { task(stop); }
            catch (...) { }
        }
    }

    ThreadPool& globalThreadPool()
    {
        static ThreadPool pool;
        return pool;
    }
}
''')

# ---- Async external-process signature scanner -------------------------------
write("include/quartz/client/native/SignatureScanner.hpp", r'''#pragma once
#include "quartz/client/async/ThreadPool.hpp"
#include "quartz/client/native/NativeTypes.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace quartz::client
{
    struct SignatureScanResult
    {
        bool Found = false;
        bool Cancelled = false;
        std::uintptr_t MatchAddress = 0;
        std::uint64_t ScannedBytes = 0;
        double DurationSeconds = 0.0;
        std::string Error;
    };

    struct SignatureScanState
    {
        std::uint64_t Generation = 0;
        std::uint64_t TotalBytes = 0;
        std::atomic<std::uint64_t> ScannedBytes{0};
        std::atomic<std::int64_t> StartedNs{0};
        std::atomic<std::int64_t> FinishedNs{0};
        std::atomic<bool> CancelRequested{false};
        std::atomic<bool> Finished{false};
        std::mutex ResultMutex;
        SignatureScanResult Result;
    };

    std::shared_ptr<SignatureScanState> startSignatureScan(pid_t pid, std::vector<RuntimeProcessRegion> regions, std::vector<std::uint8_t> bytes, std::vector<std::uint8_t> masks, bool executableOnly, std::uint64_t generation);
    void cancelSignatureScan(const std::shared_ptr<SignatureScanState>& state) noexcept;
    bool tryGetSignatureScanResult(const std::shared_ptr<SignatureScanState>& state, SignatureScanResult& result);
    double signatureScanAverageMiBs(const std::shared_ptr<SignatureScanState>& state) noexcept;
}
''')

write("src/native/SignatureScanner.cpp", r'''#include "quartz/client/native/SignatureScanner.hpp"
#include "quartz/client/Functions.hpp"
#include <libhat/scanner.hpp>
#include <libhat/signature.hpp>
#include <algorithm>
#include <chrono>
#include <exception>
#include <span>

namespace quartz::client
{
    namespace
    {
        constexpr std::size_t ReadChunk = 4 * 1024 * 1024;
        constexpr double BytesPerMiB = 1024.0 * 1024.0;

        std::int64_t steadyNowNs() noexcept
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void finishSignatureScan(const std::shared_ptr<SignatureScanState>& state, SignatureScanResult result)
        {
            const std::int64_t finishedNs = steadyNowNs();
            const std::int64_t startedNs = state->StartedNs.load(std::memory_order_acquire);
            result.ScannedBytes = state->ScannedBytes.load(std::memory_order_relaxed);
            result.DurationSeconds = startedNs > 0 && finishedNs > startedNs ? static_cast<double>(finishedNs - startedNs) / 1'000'000'000.0 : 0.0;
            {
                std::lock_guard lock(state->ResultMutex);
                state->Result = std::move(result);
            }
            state->FinishedNs.store(finishedNs, std::memory_order_release);
            state->Finished.store(true, std::memory_order_release);
        }
    }

    std::shared_ptr<SignatureScanState> startSignatureScan(const pid_t pid, std::vector<RuntimeProcessRegion> regions, std::vector<std::uint8_t> bytes, std::vector<std::uint8_t> masks, const bool executableOnly, const std::uint64_t generation)
    {
        auto state = std::make_shared<SignatureScanState>();
        state->Generation = generation;
        for (const auto& region : regions) state->TotalBytes += region.End - region.Base;
        async::globalThreadPool().submit([state, pid, regions = std::move(regions), bytes = std::move(bytes), masks = std::move(masks), executableOnly](std::stop_token stop) mutable
        {
            state->StartedNs.store(steadyNowNs(), std::memory_order_release);
            try
            {
                hat::signature signature;
                signature.reserve(bytes.size());
                for (std::size_t i = 0; i < bytes.size(); ++i) signature.emplace_back(static_cast<std::byte>(bytes[i]), static_cast<std::byte>(masks[i]));
                const hat::scan_hint hint = executableOnly ? hat::scan_hint::x86_64 : hat::scan_hint::none;
                const std::size_t overlap = signature.size() > 1 ? signature.size() - 1 : 0;
                std::vector<std::uint8_t> buffer;

                for (const auto& region : regions)
                {
                    std::uintptr_t cursor = region.Base;
                    while (cursor < region.End)
                    {
                        if (stop.stop_requested() || state->CancelRequested.load(std::memory_order_relaxed))
                        {
                            SignatureScanResult result; result.Cancelled = true; result.Error = "signature scan cancelled";
                            finishSignatureScan(state, std::move(result));
                            return;
                        }
                        const std::size_t remaining = static_cast<std::size_t>(region.End - cursor);
                        const std::size_t readSize = std::min(remaining, ReadChunk + overlap);
                        if (readSize < signature.size()) break;
                        buffer.resize(readSize);
                        std::string readError;
                        if (!readProcessMemoryBlock(pid, cursor, buffer, readError)) break;
                        const std::span<const std::byte> data{reinterpret_cast<const std::byte*>(buffer.data()), buffer.size()};
                        const auto match = hat::find_pattern(data, signature, hat::scan_alignment::X1, hint);
                        state->ScannedBytes.fetch_add(readSize, std::memory_order_relaxed);
                        if (match.has_result())
                        {
                            SignatureScanResult result;
                            result.Found = true;
                            result.MatchAddress = cursor + static_cast<std::size_t>(match.get() - data.data());
                            finishSignatureScan(state, std::move(result));
                            return;
                        }
                        const std::size_t step = readSize > overlap ? readSize - overlap : readSize;
                        cursor += step;
                    }
                }
                finishSignatureScan(state, {});
            }
            catch (const std::exception& e)
            {
                SignatureScanResult result; result.Error = e.what();
                finishSignatureScan(state, std::move(result));
            }
            catch (...)
            {
                SignatureScanResult result; result.Error = "unknown signature scanner failure";
                finishSignatureScan(state, std::move(result));
            }
        }, async::TaskPriority::High);
        return state;
    }

    void cancelSignatureScan(const std::shared_ptr<SignatureScanState>& state) noexcept
    {
        if (state) state->CancelRequested.store(true, std::memory_order_relaxed);
    }

    bool tryGetSignatureScanResult(const std::shared_ptr<SignatureScanState>& state, SignatureScanResult& result)
    {
        if (!state || !state->Finished.load(std::memory_order_acquire)) return false;
        std::lock_guard lock(state->ResultMutex);
        result = state->Result;
        return true;
    }

    double signatureScanAverageMiBs(const std::shared_ptr<SignatureScanState>& state) noexcept
    {
        if (!state) return 0.0;
        const std::int64_t startedNs = state->StartedNs.load(std::memory_order_acquire);
        if (startedNs <= 0) return 0.0;
        const std::int64_t finishedNs = state->FinishedNs.load(std::memory_order_acquire);
        const std::int64_t endNs = finishedNs > 0 ? finishedNs : steadyNowNs();
        if (endNs <= startedNs) return 0.0;
        const double seconds = static_cast<double>(endNs - startedNs) / 1'000'000'000.0;
        return static_cast<double>(state->ScannedBytes.load(std::memory_order_relaxed)) / BytesPerMiB / seconds;
    }
}
''')

replace_once("include/quartz/client/runtime/RuntimeTypes.hpp", "namespace quartz::client\n{\n    enum class RuntimeSourceKind", "namespace quartz::client\n{\n    struct SignatureScanState;\n\n    enum class RuntimeSourceKind")
replace_once("include/quartz/client/runtime/RuntimeTypes.hpp", "        std::shared_ptr<RuntimeRegisterCaptureState> SignatureRegisterCapture;\n        std::uint64_t SignatureConfigHash = 0;", "        std::shared_ptr<RuntimeRegisterCaptureState> SignatureRegisterCapture;\n        std::shared_ptr<SignatureScanState> SignatureScan;\n        std::uint64_t SignatureScanGeneration = 0;\n        bool SignatureScanRunning = false;\n        double SignatureScanAverageMiBs = 0.0;\n        double SignatureScanLastSeconds = 0.0;\n        std::uint64_t SignatureScanLastBytes = 0;\n        std::uint64_t SignatureConfigHash = 0;")

replace_once("src/runtime/RuntimeNative.cpp", '#include "quartz/client/Model.hpp"\n#include <libhat/scanner.hpp>\n#include <libhat/signature.hpp>\n', '#include "quartz/client/Model.hpp"\n#include "quartz/client/native/SignatureScanner.hpp"\n')
replace_once("src/runtime/RuntimeNative.cpp", "    void resetRuntimeSignatureScan(RuntimeBinding& binding, const bool clearResolved)\n    {\n        binding.SignatureBytes.clear();", "    void resetRuntimeSignatureScan(RuntimeBinding& binding, const bool clearResolved)\n    {\n        cancelSignatureScan(binding.SignatureScan);\n        binding.SignatureScan.reset();\n        ++binding.SignatureScanGeneration;\n        binding.SignatureScanRunning = false;\n        binding.SignatureBytes.clear();")

runtime_native = Path("src/runtime/RuntimeNative.cpp")
text = runtime_native.read_text()
start_marker = "    std::optional<std::uintptr_t> advanceRuntimeSignatureScan(RuntimeBinding& binding, const pid_t pid, std::string& error)\n"
end_marker = "\n\n    bool readNativeBinding(RuntimeBinding& binding, float& output)"
start = text.find(start_marker)
end = text.find(end_marker, start)
if start < 0 or end < 0:
    raise RuntimeError("could not locate signature scan function")
new_scan = r'''    std::optional<std::uintptr_t> resolveRuntimeSignatureMatch(RuntimeBinding& binding, const pid_t pid, const std::uintptr_t match, std::string& error)
    {
        std::uintptr_t resolved = 0;
        if (binding.SignatureResolve == SignatureResultMode::MatchAddress)
            resolved = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + binding.SignatureResultOffset);
        else if (binding.SignatureResolve == SignatureResultMode::PointerAtOffset)
        {
            const std::uintptr_t pointerAddress = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + binding.SignatureResultOffset);
            if (!readProcessMemoryValue(pid, pointerAddress, resolved, error)) { binding.SignatureStatus = "signature matched, pointer resolve failed: " + error; return std::nullopt; }
        }
        else if (binding.SignatureResolve == SignatureResultMode::RipRelative32)
        {
            std::int32_t displacement = 0;
            const std::uintptr_t displacementAddress = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + binding.SignatureResultOffset);
            if (!readProcessMemoryValue(pid, displacementAddress, displacement, error)) { binding.SignatureStatus = "signature matched, RIP displacement read failed: " + error; return std::nullopt; }
            resolved = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + std::max(binding.SignatureInstructionSize, 1) + displacement);
        }
        else if (binding.SignatureResolve == SignatureResultMode::Address32AtOffset)
        {
            std::uint32_t address32 = 0;
            const std::uintptr_t immediateAddress = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + binding.SignatureResultOffset);
            if (!readProcessMemoryValue(pid, immediateAddress, address32, error)) { binding.SignatureStatus = "signature matched, 32-bit address read failed: " + error; return std::nullopt; }
            resolved = static_cast<std::uintptr_t>(address32);
        }
        else if (binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture)
        {
            binding.SignatureMatchAddress = match;
            binding.SignatureInstructionAddress = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + binding.SignatureResultOffset);
            std::intptr_t displacement = 0;
            if (!readRuntimeRegisterDisplacement(binding, pid, binding.SignatureInstructionAddress, displacement, error)) { binding.SignatureStatus = "signature matched, displacement read failed: " + error; return std::nullopt; }
            binding.SignatureCapturedDisplacement = displacement;
            binding.SignatureProgress = 1.0f;
            std::ostringstream status; status << "Writer located 0x" << std::hex << binding.SignatureInstructionAddress << "; waiting for " << runtimeX64RegisterName(binding.SignatureRegister); binding.SignatureStatus = status.str();
            return advanceRuntimeRegisterCapture(binding, pid, error);
        }
        binding.SignatureResolvedAddress = resolved;
        binding.SignatureMatchAddress = match;
        binding.SignatureProgress = 1.0f;
        std::ostringstream status; status << "Pattern resolved 0x" << std::hex << resolved << " from match 0x" << match; binding.SignatureStatus = status.str();
        error.clear();
        return resolved;
    }

    std::optional<std::uintptr_t> advanceRuntimeSignatureScan(RuntimeBinding& binding, const pid_t pid, std::string& error)
    {
        const double now = runtimeSteadySeconds();
        const std::uint64_t configurationHash = runtimeSignatureConfigurationHash(binding);
        if (binding.SignatureScanPid != pid || binding.SignatureConfigHash != configurationHash)
        {
            resetRuntimeSignatureScan(binding);
            binding.SignatureScanPid = pid;
            binding.SignatureConfigHash = configurationHash;
        }
        if (binding.SignatureResolvedAddress != 0) { error.clear(); return binding.SignatureResolvedAddress; }
        if (binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture && binding.SignatureInstructionAddress != 0) return advanceRuntimeRegisterCapture(binding, pid, error);
        if (now < binding.NextSignatureScan)
        {
            const double remaining = std::max(binding.NextSignatureScan - now, 0.0);
            std::ostringstream status; status << "pattern not found; retry in " << std::fixed << std::setprecision(1) << remaining << " s"; binding.SignatureStatus = status.str(); error = binding.SignatureStatus; return std::nullopt;
        }
        if (binding.SignatureRegions.empty() && !prepareRuntimeSignatureScan(binding, pid, error))
        {
            binding.SignatureStatus = error; binding.NextSignatureScan = now + std::max(static_cast<double>(binding.SignatureRetrySeconds), 0.1); return std::nullopt;
        }

        if (binding.SignaturePatternKind == RuntimeSignaturePatternKind::HexadecimalPattern)
        {
            if (!binding.SignatureScan)
            {
                binding.SignatureScan = startSignatureScan(pid, binding.SignatureRegions, binding.SignatureBytes, binding.SignatureMasks, binding.SignatureExecutableOnly, binding.SignatureScanGeneration);
                binding.SignatureScanRunning = true;
                binding.SignatureStatus = "Scanning pattern";
            }
            const auto scan = binding.SignatureScan;
            binding.SignatureScannedBytes = scan->ScannedBytes.load(std::memory_order_relaxed);
            const double liveAverage = signatureScanAverageMiBs(scan);
            if (liveAverage > 0.0) binding.SignatureScanAverageMiBs = liveAverage;
            SignatureScanResult result;
            if (!tryGetSignatureScanResult(scan, result)) { binding.SignatureStatus = "Scanning pattern"; error = binding.SignatureStatus; return std::nullopt; }

            const double finalAverage = signatureScanAverageMiBs(scan);
            if (finalAverage > 0.0) binding.SignatureScanAverageMiBs = finalAverage;
            binding.SignatureScanLastBytes = result.ScannedBytes;
            binding.SignatureScanLastSeconds = result.DurationSeconds;
            binding.SignatureScannedBytes = result.ScannedBytes;
            binding.SignatureScanRunning = false;
            binding.SignatureScan.reset();
            if (scan->Generation != binding.SignatureScanGeneration || result.Cancelled) { binding.SignatureStatus = "signature scan cancelled"; error = binding.SignatureStatus; return std::nullopt; }
            if (!result.Error.empty())
            {
                binding.SignatureRegions.clear(); binding.NextSignatureScan = now + std::max(static_cast<double>(binding.SignatureRetrySeconds), 0.1); binding.SignatureProgress = 0.0f; binding.SignatureStatus = "Pattern scan failed: " + result.Error; error = binding.SignatureStatus; return std::nullopt;
            }
            if (result.Found) return resolveRuntimeSignatureMatch(binding, pid, result.MatchAddress, error);
            binding.SignatureRegions.clear(); binding.SignatureRegionIndex = 0; binding.SignatureCursor = 0; binding.NextSignatureScan = now + std::max(static_cast<double>(binding.SignatureRetrySeconds), 0.1); binding.SignatureProgress = 0.0f; binding.SignatureStatus = "Pattern not found; waiting for retry interval"; error = binding.SignatureStatus; return std::nullopt;
        }

        const auto opcodePatterns = parseRuntimeOpcodePattern(binding.Signature);
        constexpr std::size_t ScanBudget = 32 * 1024;
        constexpr std::size_t ReadChunk = 32 * 1024;
        const std::size_t scanOverlap = std::min<std::size_t>(std::max<std::size_t>(opcodePatterns.size() * 15, 15) - 1, 4095);
        std::size_t budget = ScanBudget;
        std::vector<std::uint8_t> buffer;
        while (budget > 0 && binding.SignatureRegionIndex < binding.SignatureRegions.size())
        {
            const auto& region = binding.SignatureRegions[binding.SignatureRegionIndex];
            if (binding.SignatureCursor < region.Base) binding.SignatureCursor = region.Base;
            if (binding.SignatureCursor >= region.End) { ++binding.SignatureRegionIndex; if (binding.SignatureRegionIndex < binding.SignatureRegions.size()) binding.SignatureCursor = binding.SignatureRegions[binding.SignatureRegionIndex].Base; continue; }
            const std::size_t remaining = static_cast<std::size_t>(region.End - binding.SignatureCursor);
            const std::size_t readSize = std::min({remaining, ReadChunk + scanOverlap, budget + scanOverlap});
            if (readSize < binding.SignatureBytes.size()) { binding.SignatureScannedBytes += remaining; binding.SignatureCursor = region.End; continue; }
            buffer.resize(readSize);
            std::string readError;
            if (!readProcessMemoryBlock(pid, binding.SignatureCursor, buffer, readError)) { binding.SignatureScannedBytes += remaining; binding.SignatureCursor = region.End; continue; }
            const std::size_t last = buffer.size() - binding.SignatureBytes.size();
            for (std::size_t offset = 0; offset <= last; ++offset)
            {
                std::size_t opcodeLength = 0;
                if (runtimeOpcodePatternMatches(std::span<const std::uint8_t>(buffer).subspan(offset), binding.SignatureCursor + offset, opcodePatterns, opcodeLength)) return resolveRuntimeSignatureMatch(binding, pid, binding.SignatureCursor + offset, error);
            }
            const std::size_t step = readSize > scanOverlap ? readSize - scanOverlap : readSize;
            binding.SignatureCursor += step; binding.SignatureScannedBytes += step; budget = step >= budget ? 0 : budget - step; binding.SignatureProgress = binding.SignatureTotalBytes ? std::clamp(static_cast<float>(static_cast<double>(binding.SignatureScannedBytes) / binding.SignatureTotalBytes), 0.0f, 1.0f) : 0.0f;
        }
        if (binding.SignatureRegionIndex >= binding.SignatureRegions.size())
        {
            binding.SignatureRegions.clear(); binding.SignatureRegionIndex = 0; binding.SignatureCursor = 0; binding.NextSignatureScan = now + std::max(static_cast<double>(binding.SignatureRetrySeconds), 0.1); binding.SignatureProgress = 0.0f; binding.SignatureStatus = "Pattern not found; waiting for retry interval"; error = binding.SignatureStatus; return std::nullopt;
        }
        std::ostringstream status; status << "Scanning opcode pattern " << std::fixed << std::setprecision(0) << binding.SignatureProgress * 100.0f << "%"; binding.SignatureStatus = status.str(); error = binding.SignatureStatus; return std::nullopt;
    }
'''
runtime_native.write_text(text[:start] + new_scan + text[end:])

# ---- Runtime UI cleanup ------------------------------------------------------
replace_once("src/ui/RuntimeUI.cpp", "        int source = static_cast<int>(binding.Source);", "        ImGui::SeparatorText(\"Source\");\n        int source = static_cast<int>(binding.Source);")
replace_once("src/ui/RuntimeUI.cpp", '            ImGui::SeparatorText("Mass compare");', '            ImGui::SeparatorText("Comparator / mass compare");\n            ImGui::TextDisabled("Compare a set of bindings/controls and reduce the matches to any/all/none/count/fraction/first-index.");')
replace_once("src/ui/RuntimeUI.cpp", '            if (ImGui::BeginTabItem("Output / actions"))', '            if (ImGui::BeginTabItem("Output & actions"))')
replace_once("src/ui/RuntimeUI.cpp", '            if (ImGui::BeginTabItem("Advanced transform"))', '            if (ImGui::BeginTabItem("Transform"))')
replace_once("src/ui/RuntimeUI.cpp", '        ImGui::SetNextItemWidth(300.0f);\n        if (ImGui::BeginCombo("Source binding", source ? source->Name : "<select binding>"))', '        ImGui::SeparatorText("Input & condition");\n        ImGui::SetNextItemWidth(300.0f);\n        if (ImGui::BeginCombo("Input binding", source ? source->Name : "<select binding>"))')
replace_once("src/ui/RuntimeUI.cpp", '        if (ImGui::BeginTabBar("ControlOptions", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyResizeDown))', '        ImGui::SeparatorText("Target & actions");\n        if (ImGui::BeginTabBar("ControlOptions", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyResizeDown))')
replace_once("src/ui/RuntimeUI.cpp", '            if (ImGui::BeginTabItem("Primary action"))', '            if (ImGui::BeginTabItem("Target"))')
replace_once("src/ui/RuntimeUI.cpp", '            if (ImGui::BeginTabItem("Additional actions"))', '            if (ImGui::BeginTabItem("Extra actions"))')
replace_once("src/ui/RuntimeUI.cpp", '        ImGui::TextDisabled("Triggers: %llu   %s", static_cast<unsigned long long>(control.TriggerCount), control.LastTriggerTime > 0.0 ? "has fired" : "never fired");', '        ImGui::SeparatorText("Runtime state");\n        ImGui::TextDisabled("Triggers: %llu   %s", static_cast<unsigned long long>(control.TriggerCount), control.LastTriggerTime > 0.0 ? "has fired" : "never fired");')
replace_once("src/ui/RuntimeUI.cpp", '        static RuntimeMemoryInspectorState inspector;\n        ImGui::TextUnformatted("Pointer assignments")', '        ImGui::TextUnformatted("Pointer assignments")')
replace_once("src/ui/RuntimeUI.cpp", '        drawRuntimeMemoryInspector(inspector);\n    }\n\n    void drawRuntimeObjectDescriptors', '    }\n\n    void drawRuntimeObjectDescriptors')

# Keep a compatibility helper, but remove the obsolete nested Runtime tab bar.
runtime_ui = Path("src/ui/RuntimeUI.cpp")
ui_text = runtime_ui.read_text()
ui_start = ui_text.find("    void drawRuntimeBindingsPage(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer)\n")
ui_end = ui_text.find("\n\n    void drawQRPCInspectorPage", ui_start)
if ui_start < 0 or ui_end < 0:
    raise RuntimeError("could not locate drawRuntimeBindingsPage")
compat = r'''    void drawRuntimeBindingsPage(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer)
    {
        ImGui::TextWrapped("Runtime bindings are priority-ordered dataflow nodes. Related runtime tools now live in their own pages instead of a nested mega-tab.");
        drawGroupedBindings(engine, shaderFramebuffer);
    }
'''
runtime_ui.write_text(ui_text[:ui_start] + compat + ui_text[ui_end:])

# Async scan telemetry remains visible after success/failure; only the sweep is active-only.
replace_once("src/ui/RuntimeUI.cpp", '                if (binding.SignatureResolvedAddress == 0 && binding.SignatureInstructionAddress == 0 && binding.SignatureStatus.starts_with("Scanning pattern")) drawIndeterminateProgressBar(ImVec2(320.0f, 0.0f));', '                if (binding.SignatureScanRunning) drawIndeterminateProgressBar(ImVec2(320.0f, 0.0f));\n                if (binding.SignatureScanAverageMiBs > 0.0)\n                {\n                    if (binding.SignatureScanAverageMiBs >= 1024.0) ImGui::TextDisabled("scan avg %.2f GiB/s%s", binding.SignatureScanAverageMiBs / 1024.0, binding.SignatureScanRunning ? "  (running)" : "");\n                    else ImGui::TextDisabled("scan avg %.1f MiB/s%s", binding.SignatureScanAverageMiBs, binding.SignatureScanRunning ? "  (running)" : "");\n                    if (!binding.SignatureScanRunning && binding.SignatureScanLastSeconds > 0.0) ImGui::SameLine(), ImGui::TextDisabled("%.1f MiB in %.3f s", binding.SignatureScanLastBytes / (1024.0 * 1024.0), binding.SignatureScanLastSeconds);\n                }')

# ---- Page system: grouped sidebar + real runtime pages -----------------------
write("include/quartz/client/ui/Page.hpp", r'''#pragma once
#include <string_view>

namespace quartz::client::ui
{
    struct PageContext;
    class PageManager;

    enum class PagePresentation
    {
        Tab,
        Standalone
    };

    enum class PageSection
    {
        Visual,
        Runtime,
        Device,
        Diagnostics,
        Other
    };

    class Page
    {
    public:
        virtual ~Page() = default;
        [[nodiscard]] virtual std::string_view id() const noexcept = 0;
        [[nodiscard]] virtual std::string_view title() const noexcept = 0;
        [[nodiscard]] virtual PageSection section() const noexcept { return PageSection::Other; }
        [[nodiscard]] virtual PagePresentation presentation() const noexcept { return PagePresentation::Tab; }
        virtual void render(PageContext& context, PageManager& manager) = 0;
    };
}
''')

write("include/quartz/client/ui/PageManager.hpp", r'''#pragma once
#include "Page.hpp"
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace quartz::client::ui
{
    class PageManager
    {
    public:
        PageManager() = default;
        PageManager(PageManager&&) noexcept = default;
        PageManager& operator=(PageManager&&) noexcept = default;
        PageManager(const PageManager&) = delete;
        PageManager& operator=(const PageManager&) = delete;

        template<typename T, typename... Args>
        T& add(Args&&... args)
        {
            static_assert(std::is_base_of_v<Page, T>);
            auto page = std::make_unique<T>(std::forward<Args>(args)...);
            T& result = *page;
            _pages.emplace_back(std::move(page));
            return result;
        }

        [[nodiscard]] Page* find(std::string_view id) noexcept;
        [[nodiscard]] const Page* find(std::string_view id) const noexcept;
        [[nodiscard]] bool hasStandalonePage() const noexcept { return !_standaloneId.empty(); }
        [[nodiscard]] std::string_view standalonePageId() const noexcept { return _standaloneId; }

        bool open(std::string_view id);
        void closeStandalone() noexcept { _standaloneId.clear(); }
        void render(PageContext& context);

    private:
        std::vector<std::unique_ptr<Page>> _pages;
        std::string _standaloneId;
        std::string _activePageId;
    };

    [[nodiscard]] PageManager createDefaultPageManager();
}
''')

sections = {
    "VisualizerPage.hpp": "Visual", "SpectrumPage.hpp": "Visual", "AudioPage.hpp": "Visual", "RGBPage.hpp": "Visual", "ShaderEditorPage.hpp": "Visual",
    "BindingsPage.hpp": "Runtime",
    "DevicePage.hpp": "Device", "USBPage.hpp": "Device", "QRPCPage.hpp": "Device", "InputPage.hpp": "Device", "FirmwarePage.hpp": "Device",
    "TimelinePage.hpp": "Diagnostics", "PerformancePage.hpp": "Diagnostics", "MatrixTimingPage.hpp": "Diagnostics",
}
for name, section in sections.items():
    path = Path("include/quartz/client/ui/pages") / name
    text = path.read_text()
    if "PageSection section()" in text:
        continue
    pattern = r'(\[\[nodiscard\]\] std::string_view title\(\) const noexcept override \{ return "[^"]+"; \}\n)'
    text, count = re.subn(pattern, r'\1        [[nodiscard]] PageSection section() const noexcept override { return PageSection::' + section + r'; }\n', text, count=1)
    if count != 1:
        raise RuntimeError(f"could not add section to {path}")
    path.write_text(text)


def page_header(class_name: str, page_id: str, title: str, section: str = "Runtime") -> str:
    return f'''#pragma once\n#include "quartz/client/ui/Page.hpp"\n\nnamespace quartz::client::ui\n{{\n    class {class_name} final : public Page\n    {{\n    public:\n        [[nodiscard]] std::string_view id() const noexcept override {{ return "{page_id}"; }}\n        [[nodiscard]] std::string_view title() const noexcept override {{ return "{title}"; }}\n        [[nodiscard]] PageSection section() const noexcept override {{ return PageSection::{section}; }}\n        void render(PageContext& context, PageManager& manager) override;\n    }};\n}}\n'''

write("include/quartz/client/ui/pages/ControlsPage.hpp", page_header("ControlsPage", "controls", "Controls"))
write("include/quartz/client/ui/pages/ObjectsPage.hpp", page_header("ObjectsPage", "objects", "Objects & Pointers"))
write("include/quartz/client/ui/pages/NativePage.hpp", page_header("NativePage", "native", "Native / Memory"))
write("include/quartz/client/ui/pages/ValueBankPage.hpp", page_header("ValueBankPage", "value-bank", "Value Bank"))
write("include/quartz/client/ui/pages/ProfilesPage.hpp", page_header("ProfilesPage", "profiles", "Profiles"))

write("src/ui/pages/BindingsPage.cpp", r'''#include "quartz/client/ui/pages/BindingsPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void BindingsPage::render(PageContext& context, PageManager& manager)
    {
        auto& engine = context.runtimeBindings;
        auto& shaderFramebuffer = context.shaderFramebuffer;
        (void)manager;

        ImGui::TextWrapped("Bindings are the data nodes of the runtime graph. Pick a source, optionally compare/aggregate/transform it, then route the result to materials, actions, controls or the value bank.");
        if (ImGui::Button("+ Binding")) engine.add();
        ImGui::SameLine(); if (ImGui::Button("Save graph")) engine.save();
        ImGui::SameLine(); ImGui::TextDisabled("%s", engine.path().string().c_str());
        if (ImGui::CollapsingHeader("Quick create", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto addPreset = [&](const char* name, const RuntimeSourceKind source, const int signal, const char* target)
            {
                auto& binding = engine.add(); std::snprintf(binding.Name, sizeof(binding.Name), "%s", name); binding.Source = source; binding.Signal = signal; std::snprintf(binding.TargetId, sizeof(binding.TargetId), "%s", target); binding.Clamp = true; binding.OutputMin = 0.0f; binding.OutputMax = 1.0f; binding.SmoothingHz = 8.0f; binding.UpdateHz = source == RuntimeSourceKind::NativeProcess ? 20.0f : 60.0f; return &binding;
            };
            if (ImGui::SmallButton("Native value")) addPreset("Native process value", RuntimeSourceKind::NativeProcess, 0, "runtime.native"); ImGui::SameLine();
            if (ImGui::SmallButton("Native address")) { auto* b = addPreset("Native object address", RuntimeSourceKind::NativeAddress, 1, "runtime.address"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; } ImGui::SameLine();
            if (ImGui::SmallButton("Comparator")) { auto* b = addPreset("Comparator", RuntimeSourceKind::MassCompare, 0, "runtime.compare"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; } ImGui::SameLine();
            if (ImGui::SmallButton("Aggregate")) { auto* b = addPreset("Aggregate", RuntimeSourceKind::Aggregate, 0, "runtime.aggregate"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; } ImGui::SameLine();
            if (ImGui::SmallButton("Passthrough")) { auto* b = addPreset("Binding passthrough", RuntimeSourceKind::BindingValue, 0, "runtime.value"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; } ImGui::SameLine();
            if (ImGui::SmallButton("Writable")) { auto* b = addPreset("Writable state", RuntimeSourceKind::Unbound, 0, "runtime.state"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; }
            if (ImGui::SmallButton("Audio RMS")) { auto* b = addPreset("Audio RMS", RuntimeSourceKind::Audio, 0, "runtime.audio"); b->WriteMaterial = false; } ImGui::SameLine();
            if (ImGui::SmallButton("Current shader")) { auto* b = addPreset("Current shader", RuntimeSourceKind::ShaderState, 0, "runtime.shader"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; } ImGui::SameLine();
            if (ImGui::SmallButton("Active profile")) { auto* b = addPreset("Active profile", RuntimeSourceKind::ProfileState, 0, "runtime.profile"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; }
        }
        if (shaderFramebuffer.materialParameters().empty()) ImGui::TextDisabled("Current shader has no reflected material parameters; value-only bindings still work normally.");
        drawGroupedBindings(engine, shaderFramebuffer);
        if (engine.bindings().empty()) ImGui::TextDisabled("No bindings yet.");
    }
}
''')

write("src/ui/pages/ControlsPage.cpp", r'''#include "quartz/client/ui/pages/ControlsPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void ControlsPage::render(PageContext& context, PageManager& manager)
    {
        auto& engine = context.runtimeBindings;
        (void)manager;
        ImGui::TextWrapped("Controls turn binding values into conditions and actions. Configure the input/condition first, then the target and any extra actions.");
        int passes = engine.controlPassLimit(); ImGui::SetNextItemWidth(110.0f); if (ImGui::InputInt("Control passes", &passes)) engine.setControlPassLimit(passes); ImGui::SameLine(); ImGui::TextDisabled("bounded iterative passes/frame");
        if (ImGui::Button("+ Control")) engine.addControl();
        ImGui::SameLine();
        if (ImGui::SmallButton("Threshold preset")) { auto& c = engine.addControl(); std::snprintf(c.Name, sizeof(c.Name), "%s", "Threshold"); c.Condition = RuntimeControlCondition::Greater; c.ValueA = 0.5f; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Rising edge preset")) { auto& c = engine.addControl(); std::snprintf(c.Name, sizeof(c.Name), "%s", "Rising edge"); c.Condition = RuntimeControlCondition::RisingEdge; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Changed preset")) { auto& c = engine.addControl(); std::snprintf(c.Name, sizeof(c.Name), "%s", "On change"); c.Condition = RuntimeControlCondition::OnChange; }
        drawGroupedControls(engine, context.shaderFramebuffer);
        if (engine.controls().empty()) ImGui::TextDisabled("No controls yet. Create one, choose an input binding, then choose its condition and target.");
    }
}
''')

write("src/ui/pages/ObjectsPage.cpp", r'''#include "quartz/client/ui/pages/ObjectsPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void ObjectsPage::render(PageContext& context, PageManager& manager)
    {
        (void)manager;
        ImGui::TextWrapped("Object descriptors define native layouts; pointer instances bind those layouts to exact runtime addresses. They stay together because they are two halves of the same object workflow.");
        if (ImGui::BeginTabBar("ObjectWorkspace"))
        {
            if (ImGui::BeginTabItem("Models")) { drawRuntimeObjectDescriptors(context.runtimeBindings); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Pointer instances")) { drawRuntimePointers(context.runtimeBindings); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
}
''')

write("src/ui/pages/NativePage.cpp", r'''#include "quartz/client/ui/pages/NativePage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void NativePage::render(PageContext& context, PageManager& manager)
    {
        static RuntimeMemoryInspectorState inspector;
        auto& engine = context.runtimeBindings;
        (void)manager;
        ImGui::TextWrapped("Native-process workspace: active process bindings, signature scanner telemetry, and the memory/disassembly inspector live here. Detailed source configuration stays with each binding.");
        ImGui::SeparatorText("Native bindings");
        bool any = false;
        for (auto& binding : engine.bindings())
        {
            if (binding.Source != RuntimeSourceKind::NativeProcess && binding.Source != RuntimeSourceKind::NativeAddress) continue;
            any = true;
            ImGui::PushID(static_cast<int>(binding.Id & 0x7fffffffULL));
            ImGui::Text("%s", binding.Name);
            ImGui::SameLine(); ImGui::TextDisabled("PID %d", binding.ProcessId);
            if (binding.AddressMode == ProcessAddressMode::Signature)
            {
                ImGui::SameLine();
                if (binding.SignatureScanRunning) ImGui::TextDisabled("scanning");
                else if (binding.SignatureResolvedAddress) ImGui::TextDisabled("resolved 0x%llX", static_cast<unsigned long long>(binding.SignatureResolvedAddress));
                else if (!binding.SignatureStatus.empty()) ImGui::TextDisabled("%s", binding.SignatureStatus.c_str());
                if (binding.SignatureScanAverageMiBs > 0.0)
                {
                    if (binding.SignatureScanAverageMiBs >= 1024.0) ImGui::TextDisabled("Average scan speed: %.2f GiB/s", binding.SignatureScanAverageMiBs / 1024.0);
                    else ImGui::TextDisabled("Average scan speed: %.1f MiB/s", binding.SignatureScanAverageMiBs);
                    if (binding.SignatureScanLastSeconds > 0.0) { ImGui::SameLine(); ImGui::TextDisabled("last %.1f MiB / %.3f s", binding.SignatureScanLastBytes / (1024.0 * 1024.0), binding.SignatureScanLastSeconds); }
                }
                if (ImGui::SmallButton("Rescan")) { resetRuntimeSignatureScan(binding); binding.SignatureConfigHash = 0; binding.NextUpdate = 0.0; }
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        if (!any) ImGui::TextDisabled("No NativeProcess/NativeAddress bindings configured.");
        drawRuntimeMemoryInspector(inspector);
    }
}
''')

write("src/ui/pages/ValueBankPage.cpp", r'''#include "quartz/client/ui/pages/ValueBankPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void ValueBankPage::render(PageContext& context, PageManager& manager)
    {
        (void)manager;
        ImGui::TextWrapped("Persistent runtime values used by bindings, controls and actions. Keep state here instead of inventing helper bindings just to remember a number/string/address.");
        drawRuntimeValueBank(context.runtimeBindings);
    }
}
''')

write("src/ui/pages/ProfilesPage.cpp", r'''#include "quartz/client/ui/pages/ProfilesPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void ProfilesPage::render(PageContext& context, PageManager& manager)
    {
        (void)manager;
        ImGui::TextWrapped("Profiles are runtime graph presets: enable/disable coherent sets of bindings and controls without hunting through individual nodes.");
        drawRuntimeProfiles(context.runtimeBindings);
    }
}
''')

write("src/ui/PageManager.cpp", r'''#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/pages/VisualizerPage.hpp"
#include "quartz/client/ui/pages/SpectrumPage.hpp"
#include "quartz/client/ui/pages/AudioPage.hpp"
#include "quartz/client/ui/pages/RGBPage.hpp"
#include "quartz/client/ui/pages/BindingsPage.hpp"
#include "quartz/client/ui/pages/ControlsPage.hpp"
#include "quartz/client/ui/pages/ObjectsPage.hpp"
#include "quartz/client/ui/pages/NativePage.hpp"
#include "quartz/client/ui/pages/ValueBankPage.hpp"
#include "quartz/client/ui/pages/ProfilesPage.hpp"
#include "quartz/client/ui/pages/DevicePage.hpp"
#include "quartz/client/ui/pages/InputPage.hpp"
#include "quartz/client/ui/pages/USBPage.hpp"
#include "quartz/client/ui/pages/QRPCPage.hpp"
#include "quartz/client/ui/pages/FirmwarePage.hpp"
#include "quartz/client/ui/pages/TimelinePage.hpp"
#include "quartz/client/ui/pages/PerformancePage.hpp"
#include "quartz/client/ui/pages/MatrixTimingPage.hpp"
#include "quartz/client/ui/pages/ShaderEditorPage.hpp"
#include <array>
#include <imgui.h>

namespace quartz::client::ui
{
    namespace
    {
        constexpr auto Sections = std::to_array<PageSection>({PageSection::Visual, PageSection::Runtime, PageSection::Device, PageSection::Diagnostics, PageSection::Other});
        const char* sectionName(const PageSection section) noexcept
        {
            switch (section)
            {
            case PageSection::Visual: return "VISUAL";
            case PageSection::Runtime: return "RUNTIME";
            case PageSection::Device: return "DEVICE";
            case PageSection::Diagnostics: return "DIAGNOSTICS";
            case PageSection::Other: return "OTHER";
            }
            return "OTHER";
        }
    }

    Page* PageManager::find(const std::string_view id) noexcept { for (const auto& page : _pages) if (page->id() == id) return page.get(); return nullptr; }
    const Page* PageManager::find(const std::string_view id) const noexcept { for (const auto& page : _pages) if (page->id() == id) return page.get(); return nullptr; }

    bool PageManager::open(const std::string_view id)
    {
        Page* page = find(id);
        if (!page) return false;
        if (page->presentation() == PagePresentation::Standalone) _standaloneId.assign(id);
        else _activePageId.assign(id);
        return true;
    }

    void PageManager::render(PageContext& context)
    {
        if (!_standaloneId.empty())
        {
            Page* page = find(_standaloneId);
            if (!page || page->presentation() != PagePresentation::Standalone) { _standaloneId.clear(); return; }
            page->render(context, *this);
            return;
        }
        Page* active = find(_activePageId);
        if (!active || active->presentation() != PagePresentation::Tab)
        {
            active = nullptr;
            for (const auto& page : _pages) if (page->presentation() == PagePresentation::Tab) { active = page.get(); _activePageId.assign(page->id()); break; }
        }
        if (!active) return;

        const ImVec2 available = ImGui::GetContentRegionAvail();
        constexpr float NavigationWidth = 158.0f;
        if (ImGui::BeginChild("PageNavigation", ImVec2(NavigationWidth, available.y), ImGuiChildFlags_Borders))
        {
            for (const PageSection section : Sections)
            {
                bool hasPages = false;
                for (const auto& page : _pages) if (page->presentation() == PagePresentation::Tab && page->section() == section) { hasPages = true; break; }
                if (!hasPages) continue;
                ImGui::TextDisabled("%s", sectionName(section));
                for (const auto& page : _pages)
                {
                    if (page->presentation() != PagePresentation::Tab || page->section() != section) continue;
                    const bool selected = page->id() == _activePageId;
                    if (ImGui::Selectable(page->title().data(), selected, ImGuiSelectableFlags_None, ImVec2(-1.0f, 0.0f))) _activePageId.assign(page->id());
                }
                ImGui::Spacing();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("PageContent", ImVec2(0.0f, available.y), ImGuiChildFlags_None))
        {
            if (Page* selected = find(_activePageId); selected && selected->presentation() == PagePresentation::Tab) selected->render(context, *this);
        }
        ImGui::EndChild();
    }

    PageManager createDefaultPageManager()
    {
        PageManager manager;
        manager.add<VisualizerPage>();
        manager.add<SpectrumPage>();
        manager.add<AudioPage>();
        manager.add<RGBPage>();
        manager.add<BindingsPage>();
        manager.add<ControlsPage>();
        manager.add<ObjectsPage>();
        manager.add<NativePage>();
        manager.add<ValueBankPage>();
        manager.add<ProfilesPage>();
        manager.add<DevicePage>();
        manager.add<InputPage>();
        manager.add<USBPage>();
        manager.add<QRPCPage>();
        manager.add<FirmwarePage>();
        manager.add<TimelinePage>();
        manager.add<PerformancePage>();
        manager.add<MatrixTimingPage>();
        manager.add<ShaderEditorPage>();
        return manager;
    }
}
''')

print("async scanner + UI organization migration applied")
