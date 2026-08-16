#include "quartz/client/ui/SignatureMaker.hpp"
#include "quartz/client/ui/AddressInput.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/I18n.hpp"
#include "quartz/client/ui/ReverseEngineeringNavigation.hpp"
#include "quartz/client/native/NativeDisassembly.hpp"
#include "quartz/client/Model.hpp"
#include <libhat.hpp>

namespace quartz::client::ui
{
    namespace
    {
        constexpr std::size_t SnapshotChunkBytes = 8 * 1024 * 1024;
        constexpr std::size_t SnapshotOverlapBytes = 1024;
        constexpr std::size_t MaximumReportedMatches = 64;
        constexpr std::size_t MaximumDisplayedMatches = 12;

        struct SnapshotChunk
        {
            std::uintptr_t Base = 0;
            std::size_t LogicalSize = 0;
            std::vector<std::uint8_t> Bytes;
        };

        struct MatchSummary
        {
            std::size_t Count = 0;
            bool Truncated = false;
            std::vector<std::uintptr_t> Addresses;
        };

        struct SignatureMakerJob
        {
            std::mutex Mutex;
            std::atomic_bool Finished = false;
            std::atomic_bool Success = false;
            std::atomic<float> Progress = 0.0f;
            std::string Status = "starting";
            std::string Pattern;
            std::size_t Bytes = 0;
            std::size_t Instructions = 0;
            std::size_t Wildcards = 0;
            std::size_t Matches = 0;
            bool MatchesTruncated = false;
            std::vector<std::uintptr_t> MatchAddresses;
            std::jthread Worker;
        };

        struct SignatureMakerState
        {
            pid_t Pid = 0;
            std::uintptr_t Address = 0;
            std::array<char, 256> AddressText{};
            int MinimumInstructions = 1;
            int MaxInstructions = 32;
            bool FocusRequested = false;
            std::unique_ptr<SignatureMakerJob> Job;
        };

        SignatureMakerState& state()
        {
            static SignatureMakerState value;
            return value;
        }

        void setJobStatus(SignatureMakerJob& job, std::string value)
        {
            std::lock_guard lock(job.Mutex);
            job.Status = std::move(value);
        }

        std::string signatureText(const hat::signature_view signature)
        {
            constexpr char Hex[] = "0123456789ABCDEF";
            std::string result; result.reserve(signature.size() * 3);
            for (std::size_t i = 0; i < signature.size(); ++i)
            {
                if (i != 0) result.push_back(' ');
                const auto& element = signature[i];
                if (element.none()) { result += "??"; continue; }
                const std::uint8_t value = std::to_integer<std::uint8_t>(element.value());
                result.push_back(Hex[value >> 4]); result.push_back(Hex[value & 0x0F]);
            }
            return result;
        }

        void wildcard(std::vector<std::uint8_t>& masks, const std::size_t instructionOffset, const std::size_t instructionLength, const std::size_t offset, const std::size_t size)
        {
            if (offset >= instructionLength) return;
            const std::size_t count = std::min(size, instructionLength - offset);
            for (std::size_t i = 0; i < count; ++i) masks[instructionOffset + offset + i] = 0;
        }

        MatchSummary countMatches(const std::vector<SnapshotChunk>& chunks, const hat::signature_view signature, const RuntimeX86Mode mode, const std::stop_token stop)
        {
            MatchSummary summary;
            const hat::scan_hint hint = mode == RuntimeX86Mode::X64 ? hat::scan_hint::x86_64 : hat::scan_hint::none;
            for (const auto& chunk : chunks)
            {
                if (stop.stop_requested()) break;
                const auto* begin = reinterpret_cast<const std::byte*>(chunk.Bytes.data());
                const auto* end = begin + chunk.Bytes.size();
                const std::byte* cursor = begin;
                while (cursor < end)
                {
                    const auto found = hat::find_pattern(cursor, end, signature, hat::scan_alignment::X1, hint);
                    if (!found.has_result()) break;
                    const std::size_t offset = static_cast<std::size_t>(found.get() - begin);
                    if (offset < chunk.LogicalSize)
                    {
                        ++summary.Count;
                        if (summary.Addresses.size() < MaximumDisplayedMatches) summary.Addresses.push_back(chunk.Base + offset);
                        if (summary.Count >= MaximumReportedMatches) { summary.Truncated = true; return summary; }
                    }
                    cursor = found.get() + 1;
                }
            }
            return summary;
        }

        bool snapshotExecutableMemory(SignatureMakerJob& job, const pid_t pid, std::vector<SnapshotChunk>& chunks, const std::stop_token stop, std::string& error)
        {
            const auto regions = enumerateRuntimeRegions(pid);
            std::uint64_t total = 0;
            for (const auto& region : regions) if (region.Readable && region.Executable && region.End > region.Base) total += region.End - region.Base;
            if (total == 0) { error = "process has no readable executable regions"; return false; }

            std::uint64_t completed = 0;
            for (const auto& region : regions)
            {
                if (stop.stop_requested()) { error = "cancelled"; return false; }
                if (!region.Readable || !region.Executable || region.End <= region.Base) continue;
                for (std::uintptr_t base = region.Base; base < region.End;)
                {
                    const std::size_t logical = static_cast<std::size_t>(std::min<std::uint64_t>(SnapshotChunkBytes, region.End - base));
                    const std::size_t available = static_cast<std::size_t>(region.End - base);
                    const std::size_t readSize = std::min(available, logical + SnapshotOverlapBytes);
                    SnapshotChunk chunk; chunk.Base = base; chunk.LogicalSize = logical; chunk.Bytes.resize(readSize);
                    std::string readError;
                    if (readProcessMemoryBlock(pid, base, std::span<std::uint8_t>(chunk.Bytes.data(), chunk.Bytes.size()), readError)) chunks.emplace_back(std::move(chunk));
                    completed += logical;
                    job.Progress.store(0.05f + static_cast<float>(completed) / static_cast<float>(total) * 0.35f, std::memory_order_relaxed);
                    base += logical;
                }
            }
            if (chunks.empty()) { error = "could not snapshot executable process memory"; return false; }
            return true;
        }

        bool findTargetRegion(const pid_t pid, const std::uintptr_t address, RuntimeProcessRegion& result)
        {
            for (const auto& region : enumerateRuntimeRegions(pid))
            {
                if (region.Readable && address >= region.Base && address < region.End) { result = region; return true; }
            }
            return false;
        }

        void finishJob(SignatureMakerJob& job, const bool success, std::string status, const hat::signature& signature, const std::size_t instructions, const std::size_t wildcards, const MatchSummary& matches)
        {
            std::lock_guard lock(job.Mutex);
            job.Success.store(success, std::memory_order_relaxed);
            job.Pattern = signatureText(signature);
            job.Bytes = signature.size();
            job.Instructions = instructions;
            job.Wildcards = wildcards;
            job.Matches = matches.Count;
            job.MatchesTruncated = matches.Truncated;
            job.MatchAddresses = matches.Addresses;
            job.Status = std::move(status);
            job.Progress.store(1.0f, std::memory_order_relaxed);
            job.Finished.store(true, std::memory_order_release);
        }

        void runSignatureMaker(SignatureMakerJob& job, const pid_t pid, const std::uintptr_t address, const int minimumInstructions, const int maxInstructions, const std::stop_token stop)
        {
            RuntimeProcessRegion targetRegion;
            if (!findTargetRegion(pid, address, targetRegion)) { finishJob(job, false, "target address is not readable", {}, 0, 0, {}); return; }
            if (!targetRegion.Executable) { finishJob(job, false, "target address is not executable", {}, 0, 0, {}); return; }

            setJobStatus(job, "snapshotting executable memory for conflict checks");
            std::vector<SnapshotChunk> chunks; std::string error;
            if (!snapshotExecutableMemory(job, pid, chunks, stop, error)) { finishJob(job, false, error, {}, 0, 0, {}); return; }
            if (stop.stop_requested()) { finishJob(job, false, "cancelled", {}, 0, 0, {}); return; }

            const std::size_t targetBytes = static_cast<std::size_t>(std::min<std::uint64_t>(1024, targetRegion.End - address));
            std::vector<std::uint8_t> bytes(targetBytes), masks(targetBytes, 0xFF);
            if (!readProcessMemoryBlock(pid, address, std::span<std::uint8_t>(bytes.data(), bytes.size()), error)) { finishJob(job, false, "target read failed: " + error, {}, 0, 0, {}); return; }

            const RuntimeX86Mode mode = runtimeProcessX86Mode(pid);
            const ZydisMachineMode machine = mode == RuntimeX86Mode::X86 ? ZYDIS_MACHINE_MODE_LEGACY_32 : ZYDIS_MACHINE_MODE_LONG_64;
            hat::signature signature; signature.reserve(256);
            std::size_t offset = 0, instructionCount = 0, wildcardCount = 0;
            MatchSummary matches;
            setJobStatus(job, "building instruction-aware pattern");

            while (instructionCount < static_cast<std::size_t>(maxInstructions) && offset < bytes.size() && !stop.stop_requested())
            {
#if QUARTZ_HAS_ZYDIS
                ZydisDisassembledInstruction instruction{};
                if (!ZYAN_SUCCESS(ZydisDisassembleIntel(machine, address + offset, bytes.data() + offset, bytes.size() - offset, &instruction)) || instruction.info.length == 0) break;
                const std::size_t length = instruction.info.length;
                for (std::size_t i = 0; i < length; ++i) signature.emplace_back(std::byte{bytes[offset + i]}, std::byte{0xFF});

                for (std::size_t operandIndex = 0; operandIndex < instruction.info.operand_count_visible; ++operandIndex)
                {
                    const auto& operand = instruction.operands[operandIndex];
                    if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.disp.size != 0 && (operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP || operand.mem.base == ZYDIS_REGISTER_NONE))
                        wildcard(masks, offset, length, operand.mem.disp.offset, operand.mem.disp.size / 8);
                    else if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && (operand.imm.is_relative || operand.imm.is_address))
                        wildcard(masks, offset, length, operand.imm.offset, operand.imm.size / 8);
                }

                for (std::size_t i = 0; i < length; ++i)
                {
                    if (masks[offset + i] != 0) continue;
                    signature[signature.size() - length + i] = hat::signature_element{std::nullopt};
                    ++wildcardCount;
                }
                offset += length;
#else
                finishJob(job, false, "signature maker requires Zydis", {}, 0, 0, {}); return;
#endif
                ++instructionCount;
                if (signature.size() < 4 || instructionCount < static_cast<std::size_t>(minimumInstructions)) continue;
                job.Progress.store(0.40f + static_cast<float>(instructionCount) / static_cast<float>(std::max(maxInstructions, 1)) * 0.58f, std::memory_order_relaxed);
                matches = countMatches(chunks, signature, mode, stop);
                if (matches.Count == 1 && !matches.Addresses.empty() && matches.Addresses.front() == address)
                {
                    finishJob(job, true, "unique executable-memory signature", signature, instructionCount, wildcardCount, matches);
                    return;
                }
            }

            if (stop.stop_requested()) { finishJob(job, false, "cancelled", signature, instructionCount, wildcardCount, matches); return; }
            if (signature.empty()) { finishJob(job, false, "could not decode target instructions", {}, 0, 0, {}); return; }
            if (matches.Count == 0) matches = countMatches(chunks, signature, mode, stop);
            finishJob(job, false, matches.Count > 1 ? "pattern still conflicts; increase the instruction limit" : "pattern did not resolve uniquely to the target", signature, instructionCount, wildcardCount, matches);
        }

        void startJob(SignatureMakerState& ui)
        {
            std::uintptr_t address = 0; std::string error;
            if (ui.Pid <= 0 || !evaluateAddressExpression(ui.Pid, ui.AddressText.data(), address, error) || address == 0) return;
            ui.Address = address;
            ui.Job = std::make_unique<SignatureMakerJob>();
            SignatureMakerJob* job = ui.Job.get();
            const pid_t pid = ui.Pid; const int minimumInstructions = ui.MinimumInstructions; const int maxInstructions = std::max(ui.MaxInstructions, minimumInstructions);
            job->Worker = std::jthread([job, pid, address, minimumInstructions, maxInstructions](const std::stop_token stop) { runSignatureMaker(*job, pid, address, minimumInstructions, maxInstructions, stop); });
        }
    }

    void requestSignatureMaker(const pid_t pid, const std::uintptr_t address, const int minimumInstructions) noexcept
    {
        auto& ui = state(); ui.Pid = pid; ui.Address = address; ui.FocusRequested = true; ui.MinimumInstructions = std::clamp(minimumInstructions, 1, 64); ui.MaxInstructions = std::max(ui.MaxInstructions, ui.MinimumInstructions);
        std::snprintf(ui.AddressText.data(), ui.AddressText.size(), "0x%llX", static_cast<unsigned long long>(address));
    }

    bool signatureMakerWantsFocus() noexcept
    {
        auto& ui = state(); const bool requested = ui.FocusRequested; ui.FocusRequested = false; return requested;
    }

    void drawSignatureMaker(PageContext& context, PageManager& manager)
    {
        (void)context;
        auto& ui = state();
        ImGui::TextWrapped("%s", i18n::tr("re.signatureDescription"));
        int pid = static_cast<int>(ui.Pid); ImGui::SetNextItemWidth(110.0f); if (ImGui::InputInt("PID##SignatureMaker", &pid)) ui.Pid = static_cast<pid_t>(std::max(pid, 0)); ImGui::SameLine();
        drawAddressInput(i18n::tr("re.signatureAddress"), ui.AddressText.data(), ui.AddressText.size(), ui.Pid, 300.0f); ImGui::SameLine();
        ImGui::SetNextItemWidth(145.0f); ImGui::SliderInt(i18n::tr("re.signatureMinimumInstructions"), &ui.MinimumInstructions, 1, 64); ImGui::SameLine();
        ui.MaxInstructions = std::max(ui.MaxInstructions, ui.MinimumInstructions); ImGui::SetNextItemWidth(145.0f); ImGui::SliderInt(i18n::tr("re.signatureMaximumInstructions"), &ui.MaxInstructions, ui.MinimumInstructions, 64);

        const bool running = ui.Job && !ui.Job->Finished.load(std::memory_order_acquire);
        std::uintptr_t candidate = 0; std::string expressionError; const bool validAddress = ui.Pid > 0 && evaluateAddressExpression(ui.Pid, ui.AddressText.data(), candidate, expressionError) && candidate != 0;
        ImGui::BeginDisabled(running || !validAddress);
        if (ImGui::Button(i18n::tr("re.makeSignature"))) startJob(ui);
        ImGui::EndDisabled();
        if (!validAddress && ui.AddressText[0] != '\0') { ImGui::SameLine(); ImGui::TextDisabled("%s", expressionError.c_str()); }
        if (running)
        {
            ImGui::SameLine();
            if (ImGui::Button(i18n::tr("common.cancel"))) ui.Job->Worker.request_stop();
        }

        if (!ui.Job) return;
        std::string status, pattern; std::vector<std::uintptr_t> matches; std::size_t bytes = 0, instructions = 0, wildcards = 0, matchCount = 0; bool truncated = false;
        {
            std::lock_guard lock(ui.Job->Mutex);
            status = ui.Job->Status; pattern = ui.Job->Pattern; matches = ui.Job->MatchAddresses; bytes = ui.Job->Bytes; instructions = ui.Job->Instructions; wildcards = ui.Job->Wildcards; matchCount = ui.Job->Matches; truncated = ui.Job->MatchesTruncated;
        }
        if (running) ImGui::ProgressBar(ui.Job->Progress.load(std::memory_order_relaxed), ImVec2(-1.0f, 0.0f), status.c_str());
        else
        {
            const bool success = ui.Job->Success.load(std::memory_order_relaxed);
            ImGui::TextColored(success ? ImVec4(0.35f, 0.86f, 0.58f, 1.0f) : ImVec4(0.95f, 0.62f, 0.28f, 1.0f), "%s", status.c_str());
            ImGui::TextDisabled("%zu instructions | %zu bytes | %zu wildcards | %zu%s matches", instructions, bytes, wildcards, matchCount, truncated ? "+" : "");
        }

        if (!pattern.empty())
        {
            ImGui::SeparatorText(i18n::tr("re.signatureMaker"));
            ImGui::PushTextWrapPos(); ImGui::TextUnformatted(pattern.c_str()); ImGui::PopTextWrapPos();
            if (ImGui::Button(i18n::tr("re.copySignature"))) ImGui::SetClipboardText(pattern.c_str());
        }
        if (!matches.empty())
        {
            ImGui::SeparatorText(i18n::tr("re.signatureMatches"));
            for (std::size_t i = 0; i < matches.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i)); const std::string addressText = runtimeHexAddress(matches[i]);
                ImGui::TextUnformatted(addressText.c_str()); ImGui::SameLine();
                if (ImGui::SmallButton(i18n::tr("re.inspect"))) { requestMemoryInspector(ui.Pid, matches[i]); manager.open("native"); }
                ImGui::PopID();
            }
        }
    }
}
