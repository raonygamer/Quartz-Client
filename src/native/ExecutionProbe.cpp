#include "quartz/client/native/ExecutionProbe.hpp"
#include "quartz/client/Model.hpp"

namespace quartz::client
{
    struct ExecutionProbeState
    {
        std::mutex Mutex;
        bool Finished = false;
        pid_t Pid = 0;
        std::uintptr_t Address = 0;
        std::string Status;
        std::optional<ExecutionProbeHit> Hit;
        std::jthread Worker;
    };

    namespace
    {
        constexpr std::size_t Dr0Offset = offsetof(struct user, u_debugreg[0]);
        constexpr std::size_t Dr6Offset = offsetof(struct user, u_debugreg[6]);
        constexpr std::size_t Dr7Offset = offsetof(struct user, u_debugreg[7]);
    }

    ExecutionProbe::~ExecutionProbe() { stop(); }
    ExecutionProbe& executionProbe() { static ExecutionProbe probe; return probe; }
    bool ExecutionProbe::running() const noexcept { if (!_state) return false; std::lock_guard lock(_state->Mutex); return !_state->Finished; }
    std::string ExecutionProbe::status() const { if (!_state) return {}; std::lock_guard lock(_state->Mutex); return _state->Status; }
    std::optional<ExecutionProbeHit> ExecutionProbe::hit() const { if (!_state) return std::nullopt; std::lock_guard lock(_state->Mutex); return _state->Hit; }
    pid_t ExecutionProbe::pid() const noexcept { if (!_state) return 0; std::lock_guard lock(_state->Mutex); return _state->Pid; }
    std::uintptr_t ExecutionProbe::address() const noexcept { if (!_state) return 0; std::lock_guard lock(_state->Mutex); return _state->Address; }
    void ExecutionProbe::stop() noexcept { if (!_state) return; _state->Worker.request_stop(); _state.reset(); }

    bool ExecutionProbe::start(const pid_t pid, const std::uintptr_t address, std::string& error)
    {
        stop();
        if (pid <= 0 || address == 0) { error = "select a process and enter an instruction address"; return false; }
        auto state = std::make_shared<ExecutionProbeState>(); ExecutionProbeState* probe = state.get(); state->Pid = pid; state->Address = address; state->Status = "arming one-shot execution breakpoint";
        state->Worker = std::jthread([probe, pid, address](std::stop_token stop)
        {
            struct Thread { pid_t Tid = 0; bool Attached = false; bool Stopped = false; bool Armed = false; std::uint64_t Dr0 = 0, Dr6 = 0, Dr7 = 0; };
            std::vector<Thread> threads; std::string lastError; bool captured = false;
            auto setStatus = [&](std::string value) { std::lock_guard lock(probe->Mutex); probe->Status = std::move(value); };
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
                if (tracked(tid) || stop.stop_requested() || captured) return;
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
                if (!runtimePtracePokeUser(tid, Dr0Offset, address) || !runtimePtracePokeUser(tid, Dr6Offset, 0) || !runtimePtracePokeUser(tid, Dr7Offset, dr7)) { lastError = std::strerror(errno); detach(thread); return; }
                thread.Armed = true;
                const int signal = WSTOPSIG(status), deliver = signal == SIGTRAP || signal == SIGSTOP ? 0 : signal;
                continueStopped(thread, deliver);
            };
            auto refresh = [&] { for (const pid_t tid : enumerateRuntimeThreads(pid)) arm(tid); };
            refresh(); double nextRefresh = runtimeSteadySeconds() + 0.10, nextStatus = 0.0;
            while (!stop.stop_requested() && !captured)
            {
                const double now = runtimeSteadySeconds(); if (now >= nextRefresh) { refresh(); nextRefresh = now + 0.10; }
                bool observed = false;
                for (auto& thread : threads)
                {
                    if (!thread.Attached || thread.Stopped) continue;
                    int status = 0; errno = 0; const pid_t result = ::waitpid(thread.Tid, &status, __WALL | WNOHANG); if (result == 0) continue; if (result < 0) { if (errno == ESRCH || errno == ECHILD) gone(thread); continue; }
                    observed = true; if (WIFEXITED(status) || WIFSIGNALED(status)) { gone(thread); continue; } if (!WIFSTOPPED(status)) continue; thread.Stopped = true;
                    const int signal = WSTOPSIG(status); bool ours = false;
                    if (signal == SIGTRAP && thread.Armed)
                    {
                        std::uint64_t dr6 = 0; user_regs_struct regs{}; const bool haveDr6 = runtimePtracePeekUser(thread.Tid, Dr6Offset, dr6), haveRegs = ::ptrace(PTRACE_GETREGS, thread.Tid, nullptr, &regs) == 0;
                        ours = haveDr6 && (dr6 & 1ULL) != 0 && haveRegs && static_cast<std::uintptr_t>(regs.rip) == address;
                        if (ours)
                        {
                            ExecutionProbeHit hit; hit.Time = runtimeSteadySeconds(); hit.Pid = pid; hit.Tid = thread.Tid; hit.Address = address; hit.Registers = regs; hit.HasRegisters = true;
                            { std::lock_guard lock(probe->Mutex); probe->Hit = hit; probe->Status = "captured registers at " + runtimeHexAddress(address) + " on TID " + std::to_string(thread.Tid); }
                            captured = true;
                            break;
                        }
                        runtimePtracePokeUser(thread.Tid, Dr6Offset, 0);
                    }
                    const int deliver = ours || signal == SIGSTOP ? 0 : signal;
                    continueStopped(thread, deliver);
                }
                if (now >= nextStatus)
                {
                    std::ostringstream text; text << "armed for next execution at 0x" << std::hex << address << std::dec << " | threads " << threads.size(); if (!lastError.empty()) text << " | last attach error: " << lastError; setStatus(text.str()); nextStatus = now + 0.25;
                }
                if (!observed) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            for (auto& thread : threads) detach(thread);
            { std::lock_guard lock(probe->Mutex); probe->Finished = true; if (stop.stop_requested() && !captured) probe->Status = "execution probe cancelled"; else if (!captured) probe->Status = "execution probe ended without a hit"; }
        });
        _state = std::move(state); error.clear(); return true;
    }
}
