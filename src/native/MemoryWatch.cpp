#include "quartz/client/native/MemoryWatch.hpp"
#include "quartz/client/native/NativeDisassembly.hpp"
#include "quartz/client/Model.hpp"

namespace quartz::client
{
    struct MemoryWatchState
    {
        std::mutex Mutex;
        bool Finished = false;
        std::string Status;
        std::vector<MemoryWatchHit> Hits;
        std::jthread Worker;
    };

    namespace
    {
        std::string previousInstruction(const pid_t pid, const std::uintptr_t rip, std::uintptr_t& instructionAddress)
        {
            instructionAddress = 0;
            if (rip < 15) return {};
            std::array<std::uint8_t, 15> bytes{}; std::string error;
            if (!readProcessMemoryBlock(pid, rip - bytes.size(), bytes, error)) return {};
            std::string best;
            for (std::size_t length = 15; length > 0; --length)
            {
                const std::size_t offset = bytes.size() - length; std::string text; std::size_t decoded = 0;
                if (runtimeDecodeProcessInstructionText(pid, std::span<const std::uint8_t>(bytes).subspan(offset), rip - length, text, decoded) && decoded == length) { best = std::move(text); instructionAddress = rip - length; break; }
            }
            return best;
        }

        std::uint64_t lengthCode(const std::size_t size) noexcept { return size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 3 : 2; }
        std::uintptr_t hitSite(const MemoryWatchHit& hit) noexcept { return hit.InstructionAddress ? hit.InstructionAddress : hit.Rip; }
    }

    MemoryWatch::~MemoryWatch() { stop(); }
    bool MemoryWatch::running() const noexcept { if (!_state) return false; std::lock_guard lock(_state->Mutex); return !_state->Finished; }
    std::string MemoryWatch::status() const { if (!_state) return {}; std::lock_guard lock(_state->Mutex); return _state->Status; }
    std::vector<MemoryWatchHit> MemoryWatch::hits() const { if (!_state) return {}; std::lock_guard lock(_state->Mutex); return _state->Hits; }
    void MemoryWatch::clearHits() { if (!_state) return; std::lock_guard lock(_state->Mutex); _state->Hits.clear(); }
    void MemoryWatch::stop() noexcept { if (!_state) return; _state->Worker.request_stop(); _state.reset(); }

    bool MemoryWatch::start(const pid_t pid, const std::uintptr_t address, const std::size_t size, const MemoryWatchAccess access, const std::size_t maxHits, std::string& error)
    {
        stop();
        if (pid <= 0 || address == 0) { error = "select a process and enter an address"; return false; }
        if (size != 1 && size != 2 && size != 4 && size != 8) { error = "hardware watchpoint size must be 1, 2, 4 or 8 bytes"; return false; }
        if ((address & (size - 1)) != 0) { error = "hardware watchpoint address must be aligned to its size"; return false; }
        auto state = std::make_shared<MemoryWatchState>(); MemoryWatchState* watch = state.get(); state->Status = "arming hardware watchpoint";
        state->Worker = std::jthread([watch, pid, address, size, access, maxHits](std::stop_token stop)
        {
            struct Thread { pid_t Tid = 0; bool Attached = false; bool Stopped = false; bool Armed = false; std::uint64_t Dr0 = 0, Dr6 = 0, Dr7 = 0; };
            constexpr std::size_t Dr0Offset = offsetof(struct user, u_debugreg[0]), Dr6Offset = offsetof(struct user, u_debugreg[6]), Dr7Offset = offsetof(struct user, u_debugreg[7]);
            std::vector<Thread> threads; std::size_t totalHits = 0; std::string lastError;
            auto setStatus = [&](std::string value) { std::lock_guard lock(watch->Mutex); watch->Status = std::move(value); };
            auto gone = [](Thread& thread) { thread.Attached = thread.Stopped = thread.Armed = false; };
            auto restore = [&](Thread& thread)
            {
                if (!thread.Armed || !thread.Stopped) return;
                runtimePtracePokeUser(thread.Tid, Dr7Offset, thread.Dr7); runtimePtracePokeUser(thread.Tid, Dr0Offset, thread.Dr0); runtimePtracePokeUser(thread.Tid, Dr6Offset, thread.Dr6); thread.Armed = false;
            };
            auto detachStopped = [&](Thread& thread, const int signal = 0)
            {
                if (!thread.Attached || !thread.Stopped) return true;
                restore(thread); errno = 0;
                if (::ptrace(PTRACE_DETACH, thread.Tid, nullptr, reinterpret_cast<void*>(static_cast<std::uintptr_t>(signal))) == 0) { thread.Attached = thread.Stopped = false; return true; }
                if (errno == ESRCH || errno == ECHILD) { gone(thread); return true; }
                return false;
            };
            auto continueStopped = [&](Thread& thread, const int signal = 0)
            {
                if (!thread.Attached || !thread.Stopped) return false;
                errno = 0;
                if (::ptrace(PTRACE_CONT, thread.Tid, nullptr, reinterpret_cast<void*>(static_cast<std::uintptr_t>(signal))) == 0) { thread.Stopped = false; return true; }
                if (errno == ESRCH || errno == ECHILD) gone(thread); else detachStopped(thread, signal);
                return false;
            };
            auto stopForCleanup = [&](Thread& thread, int& status, const bool interruptPending = false)
            {
                status = 0;
                if (!thread.Attached) return false;
                if (thread.Stopped) return true;
                errno = 0;
                const pid_t pending = ::waitpid(thread.Tid, &status, __WALL | WNOHANG);
                if (pending == thread.Tid)
                {
                    if (WIFEXITED(status) || WIFSIGNALED(status)) { gone(thread); return false; }
                    if (WIFSTOPPED(status)) { thread.Stopped = true; return true; }
                }
                else if (pending < 0 && errno != EINTR)
                {
                    if (errno == ESRCH || errno == ECHILD) gone(thread);
                    return false;
                }
                if (!interruptPending)
                {
                    errno = 0;
                    if (::ptrace(PTRACE_INTERRUPT, thread.Tid, nullptr, nullptr) != 0) { if (errno == ESRCH || errno == ECHILD) gone(thread); return false; }
                }
                for (;;)
                {
                    errno = 0;
                    const pid_t result = ::waitpid(thread.Tid, &status, __WALL);
                    if (result == thread.Tid)
                    {
                        if (WIFEXITED(status) || WIFSIGNALED(status)) { gone(thread); return false; }
                        if (WIFSTOPPED(status)) { thread.Stopped = true; return true; }
                        continue;
                    }
                    if (result < 0 && errno == EINTR) continue;
                    if (result < 0 && (errno == ESRCH || errno == ECHILD)) gone(thread);
                    return false;
                }
            };
            auto detach = [&](Thread& thread)
            {
                if (!thread.Attached) return;
                int status = 0;
                if (!stopForCleanup(thread, status)) return;
                const int signal = WIFSTOPPED(status) ? WSTOPSIG(status) : 0;
                const int deliver = signal == SIGTRAP || signal == SIGSTOP ? 0 : signal;
                if (!detachStopped(thread, deliver)) continueStopped(thread, deliver);
            };
            auto tracked = [&](const pid_t tid) { return std::ranges::any_of(threads, [tid](const Thread& thread) { return thread.Tid == tid && thread.Attached; }); };
            auto arm = [&](const pid_t tid)
            {
                if (tracked(tid) || stop.stop_requested()) return;
                errno = 0;
                if (::ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) != 0) { lastError = std::strerror(errno); return; }
                threads.push_back({}); Thread& thread = threads.back(); thread.Tid = tid; thread.Attached = true;
                errno = 0;
                if (::ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) != 0) { lastError = std::strerror(errno); detach(thread); return; }
                int status = 0;
                if (!runtimeWaitForPtraceStop(tid, stop, 0.25, status))
                {
                    lastError = "thread did not stop";
                    int cleanupStatus = 0;
                    if (stopForCleanup(thread, cleanupStatus, true))
                    {
                        const int signal = WIFSTOPPED(cleanupStatus) ? WSTOPSIG(cleanupStatus) : 0;
                        const int deliver = signal == SIGTRAP || signal == SIGSTOP ? 0 : signal;
                        if (!detachStopped(thread, deliver)) continueStopped(thread, deliver);
                    }
                    return;
                }
                thread.Stopped = true;
                if (!runtimePtracePeekUser(tid, Dr0Offset, thread.Dr0) || !runtimePtracePeekUser(tid, Dr6Offset, thread.Dr6) || !runtimePtracePeekUser(tid, Dr7Offset, thread.Dr7)) { lastError = std::strerror(errno); detach(thread); return; }
                std::uint64_t dr7 = thread.Dr7; dr7 &= ~0x3ULL; dr7 &= ~(0xFULL << 16); dr7 |= 1ULL;
                const std::uint64_t rw = access == MemoryWatchAccess::Write ? 1ULL : 3ULL; dr7 |= (rw | (lengthCode(size) << 2)) << 16;
                if (!runtimePtracePokeUser(tid, Dr0Offset, address) || !runtimePtracePokeUser(tid, Dr6Offset, 0) || !runtimePtracePokeUser(tid, Dr7Offset, dr7)) { lastError = std::strerror(errno); detach(thread); return; }
                thread.Armed = true;
                const int signal = WSTOPSIG(status), deliver = signal == SIGTRAP || signal == SIGSTOP ? 0 : signal;
                continueStopped(thread, deliver);
            };
            auto refresh = [&] { for (const pid_t tid : enumerateRuntimeThreads(pid)) arm(tid); };
            refresh(); double nextRefresh = runtimeSteadySeconds() + 0.10, nextStatus = 0.0;
            while (!stop.stop_requested() && (maxHits == 0 || totalHits < maxHits))
            {
                const double now = runtimeSteadySeconds(); if (now >= nextRefresh) { refresh(); nextRefresh = now + 0.10; }
                bool observed = false;
                for (auto& thread : threads)
                {
                    if (!thread.Attached || thread.Stopped) continue;
                    int status = 0; errno = 0; const pid_t result = ::waitpid(thread.Tid, &status, __WALL | WNOHANG); if (result == 0) continue; if (result < 0) { if (errno == ESRCH || errno == ECHILD) gone(thread); continue; }
                    observed = true; if (WIFEXITED(status) || WIFSIGNALED(status)) { gone(thread); continue; } if (!WIFSTOPPED(status)) continue; thread.Stopped = true;
                    const int signal = WSTOPSIG(status); bool watched = false;
                    if (signal == SIGTRAP && thread.Armed)
                    {
                        std::uint64_t dr6 = 0; user_regs_struct regs{}; const bool haveDr6 = runtimePtracePeekUser(thread.Tid, Dr6Offset, dr6), haveRegs = ::ptrace(PTRACE_GETREGS, thread.Tid, nullptr, &regs) == 0;
                        watched = haveDr6 && (dr6 & 1ULL) != 0;
                        if (watched)
                        {
                            ++totalHits;
                            if (haveRegs)
                            {
                                MemoryWatchHit hit; hit.Time = runtimeSteadySeconds(); hit.Tid = thread.Tid; hit.Rip = regs.rip; hit.Instruction = previousInstruction(pid, regs.rip, hit.InstructionAddress); hit.Count = 1; hit.Registers = regs; hit.HasRegisters = true;
                                if (hit.Instruction.empty()) hit.Instruction = "<could not recover preceding instruction>";
                                std::lock_guard lock(watch->Mutex);
                                const std::uintptr_t site = hitSite(hit);
                                const auto existing = std::ranges::find_if(watch->Hits, [site](const MemoryWatchHit& candidate) { return hitSite(candidate) == site; });
                                if (existing == watch->Hits.end()) { watch->Hits.push_back(std::move(hit)); if (watch->Hits.size() > 256) watch->Hits.erase(watch->Hits.begin()); }
                                else { existing->Time = hit.Time; existing->Tid = hit.Tid; existing->Rip = hit.Rip; existing->InstructionAddress = hit.InstructionAddress; existing->Instruction = std::move(hit.Instruction); existing->Registers = hit.Registers; existing->HasRegisters = hit.HasRegisters; ++existing->Count; }
                            }
                        }
                        runtimePtracePokeUser(thread.Tid, Dr6Offset, 0);
                    }
                    const int deliver = signal == SIGTRAP || signal == SIGSTOP ? 0 : signal;
                    continueStopped(thread, deliver);
                    if (maxHits != 0 && totalHits >= maxHits) break;
                }
                if (now >= nextStatus)
                {
                    std::size_t sites = 0; { std::lock_guard lock(watch->Mutex); sites = watch->Hits.size(); }
                    std::ostringstream text; text << "watching 0x" << std::hex << address << std::dec << " | threads " << threads.size() << " | hits " << totalHits << " | sites " << sites; if (!lastError.empty()) text << " | last attach error: " << lastError; setStatus(text.str()); nextStatus = now + 0.25;
                }
                if (!observed) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            for (auto& thread : threads) detach(thread);
            { std::lock_guard lock(watch->Mutex); watch->Finished = true; watch->Status = stop.stop_requested() ? "watch stopped" : "watch hit limit reached"; }
        });
        _state = std::move(state); error.clear(); return true;
    }
}
