#ifdef _WIN32
#include "quartz/client/Model.hpp"
#include "quartz/client/native/ExecutionProbe.hpp"
#include "quartz/client/native/MemoryWatch.hpp"
#include "quartz/client/native/NativeDisassembly.hpp"

namespace quartz::client
{
    namespace
    {
        struct ThreadContext
        {
            CONTEXT Native{};
            WOW64_CONTEXT Wow{};
            bool Wow64 = false;
            explicit ThreadContext(bool wow) : Wow64(wow) {}
            bool read(HANDLE thread)
            {
                if (Wow64) { Wow.ContextFlags = WOW64_CONTEXT_FULL | WOW64_CONTEXT_DEBUG_REGISTERS; return Wow64GetThreadContext(thread, &Wow); }
                Native.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
                return GetThreadContext(thread, &Native);
            }
            bool write(HANDLE thread) { return Wow64 ? Wow64SetThreadContext(thread, &Wow) : SetThreadContext(thread, &Native); }
            std::uint64_t dr(unsigned index) const
            {
                if (Wow64) { switch (index) { case 0:return Wow.Dr0; case 1:return Wow.Dr1; case 2:return Wow.Dr2; case 3:return Wow.Dr3; case 6:return Wow.Dr6; default:return Wow.Dr7; } }
                switch (index) { case 0:return Native.Dr0; case 1:return Native.Dr1; case 2:return Native.Dr2; case 3:return Native.Dr3; case 6:return Native.Dr6; default:return Native.Dr7; }
            }
            void setDr(unsigned index, std::uint64_t value)
            {
                if (Wow64) { const DWORD v = static_cast<DWORD>(value); switch(index) { case 0:Wow.Dr0=v;break; case 1:Wow.Dr1=v;break; case 2:Wow.Dr2=v;break; case 3:Wow.Dr3=v;break; case 6:Wow.Dr6=v;break; default:Wow.Dr7=v;break; } }
                else { switch(index) { case 0:Native.Dr0=value;break; case 1:Native.Dr1=value;break; case 2:Native.Dr2=value;break; case 3:Native.Dr3=value;break; case 6:Native.Dr6=value;break; default:Native.Dr7=value;break; } }
            }
            void resumeFlag() { if (Wow64) Wow.EFlags |= 0x10000; else Native.EFlags |= 0x10000; }
            NativeRegisters registers() const
            {
                NativeRegisters r{};
                if (Wow64)
                {
                    r.rax=Wow.Eax; r.rbx=Wow.Ebx; r.rcx=Wow.Ecx; r.rdx=Wow.Edx; r.rsi=Wow.Esi; r.rdi=Wow.Edi;
                    r.rbp=Wow.Ebp; r.rsp=Wow.Esp; r.rip=Wow.Eip; r.eflags=Wow.EFlags;
                    r.cs=Wow.SegCs; r.ss=Wow.SegSs; r.ds=Wow.SegDs; r.es=Wow.SegEs; r.fs=Wow.SegFs; r.gs=Wow.SegGs;
                }
                else
                {
                    r.rax=Native.Rax; r.rbx=Native.Rbx; r.rcx=Native.Rcx; r.rdx=Native.Rdx; r.rsi=Native.Rsi; r.rdi=Native.Rdi;
                    r.rbp=Native.Rbp; r.rsp=Native.Rsp; r.rip=Native.Rip; r.eflags=Native.EFlags;
                    r.r8=Native.R8; r.r9=Native.R9; r.r10=Native.R10; r.r11=Native.R11; r.r12=Native.R12; r.r13=Native.R13; r.r14=Native.R14; r.r15=Native.R15;
                    r.cs=Native.SegCs; r.ss=Native.SegSs; r.ds=Native.SegDs; r.es=Native.SegEs; r.fs=Native.SegFs; r.gs=Native.SegGs;
                }
                return r;
            }
        };

        std::mutex DebugOwnersMutex;
        std::set<pid_t> DebugOwners;

        // All attach/wait/continue/detach operations remain on the owning worker thread.
        // Windows permits one debugger per process, just as ptrace does on Linux.
        std::string watchBreakpoint(pid_t pid, std::uintptr_t address, std::size_t size, unsigned access,
                                    std::stop_token stop, double timeout,
                                    const std::function<bool(pid_t, const NativeRegisters&)>& hit,
                                    const std::function<void(std::string)>& status)
        {
            {
                std::lock_guard lock(DebugOwnersMutex);
                if (!DebugOwners.insert(pid).second) return "another Quartz debug operation owns this process; stop it before arming this one";
            }
            struct Owner { pid_t Pid; ~Owner() { std::lock_guard lock(DebugOwnersMutex); DebugOwners.erase(Pid); } } owner{pid};
            win::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)));
            if (!process) return win::error("OpenProcess for debugging");
            BOOL wow = FALSE;
            if (!IsWow64Process(process.get(), &wow)) return win::error("IsWow64Process");
            if (wow && (size == 8 || address > UINT32_MAX)) return "32-bit targets require an address below 4 GiB and watch sizes of 1, 2, or 4 bytes";
            if (!DebugActiveProcess(static_cast<DWORD>(pid))) return win::error("DebugActiveProcess");
            if (!DebugSetProcessKillOnExit(FALSE))
            {
                const auto failure = win::error("DebugSetProcessKillOnExit");
                DebugActiveProcessStop(static_cast<DWORD>(pid)); return failure;
            }
            struct TrackedThread
            {
                win::Handle Handle;
                unsigned Slot = 0;
                std::uint64_t Address = 0, Control = 0, Status = 0;
            };
            std::map<DWORD, TrackedThread> threads;
            bool initialBreak = true, done = false, exited = false;
            std::string failure;
            const double deadline = timeout > 0 ? runtimeSteadySeconds() + timeout : std::numeric_limits<double>::max();
            const auto arm = [&](DWORD tid)
            {
                win::Handle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME | SYNCHRONIZE, FALSE, tid));
                if (!thread) { failure = win::error("OpenThread"); return; }
                ThreadContext context(wow);
                if (!context.read(thread.get())) { failure = win::error("GetThreadContext"); return; }
                unsigned slot = 0;
                while (slot < 4 && (context.dr(7) & (3ULL << (slot * 2)))) ++slot;
                if (slot == 4) { failure = "all hardware breakpoint slots are already occupied"; return; }
                TrackedThread saved{std::move(thread), slot, context.dr(slot), context.dr(7), context.dr(6)};
                const auto length = size == 1 ? 0ULL : size == 2 ? 1ULL : size == 4 ? 3ULL : 2ULL;
                auto control = saved.Control & ~(0xFULL << (16 + slot * 4));
                control |= 1ULL << (slot * 2);
                control |= (access | (length << 2)) << (16 + slot * 4);
                context.setDr(slot, address); context.setDr(6, context.dr(6) & ~(1ULL << slot)); context.setDr(7, control);
                if (!context.write(saved.Handle.get())) { failure = win::error("SetThreadContext"); return; }
                threads.emplace(tid, std::move(saved));
            };
            status("attaching Windows debugger");
            while (!done && failure.empty() && !stop.stop_requested() && runtimeSteadySeconds() < deadline)
            {
                DEBUG_EVENT event{};
                if (!WaitForDebugEvent(&event, 20))
                {
                    if (GetLastError() != ERROR_SEM_TIMEOUT) failure = win::error("WaitForDebugEvent");
                    continue;
                }
                DWORD continuation = DBG_CONTINUE;
                switch (event.dwDebugEventCode)
                {
                case CREATE_PROCESS_DEBUG_EVENT:
                    if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
                    arm(event.dwThreadId); break;
                case CREATE_THREAD_DEBUG_EVENT: arm(event.dwThreadId); break;
                case EXIT_THREAD_DEBUG_EVENT: threads.erase(event.dwThreadId); break;
                case LOAD_DLL_DEBUG_EVENT: if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile); break;
                case EXIT_PROCESS_DEBUG_EVENT: exited = done = true; break;
                case EXCEPTION_DEBUG_EVENT:
                {
                    continuation = DBG_EXCEPTION_NOT_HANDLED;
                    const auto code = event.u.Exception.ExceptionRecord.ExceptionCode;
                    if (initialBreak && (code == EXCEPTION_BREAKPOINT || code == 0x4000001FU))
                    { initialBreak = false; continuation = DBG_CONTINUE; status("hardware breakpoint armed on " + std::to_string(threads.size()) + " threads"); }
                    else if (code == EXCEPTION_SINGLE_STEP || code == 0x4000001EU)
                    {
                        const auto found = threads.find(event.dwThreadId);
                        if (found == threads.end()) break;
                        ThreadContext context(wow);
                        if (!context.read(found->second.Handle.get())) { failure = win::error("reading breakpoint context"); break; }
                        const auto mask = 1ULL << found->second.Slot;
                        if (context.dr(6) & mask)
                        {
                            done = hit(static_cast<pid_t>(event.dwThreadId), context.registers());
                            context.setDr(6, context.dr(6) & ~mask);
                            if (access == 0) context.resumeFlag();
                            if (!context.write(found->second.Handle.get())) failure = win::error("continuing breakpoint context");
                            continuation = DBG_CONTINUE;
                        }
                    }
                    break;
                }
                default: break;
                }
                if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation)) { failure = win::error("ContinueDebugEvent"); break; }
            }
            if (!exited)
            {
                // Suspend all surviving threads first, restore only our slot, then resume.
                std::vector<HANDLE> suspended;
                for (auto& [tid, thread] : threads)
                {
                    if (SuspendThread(thread.Handle.get()) == DWORD(-1)) continue;
                    suspended.push_back(thread.Handle.get());
                    ThreadContext context(wow);
                    if (context.read(thread.Handle.get()))
                    {
                        const auto controlMask = (3ULL << (thread.Slot * 2)) | (0xFULL << (16 + thread.Slot * 4));
                        context.setDr(thread.Slot, thread.Address);
                        context.setDr(7, (context.dr(7) & ~controlMask) | (thread.Control & controlMask));
                        context.setDr(6, (context.dr(6) & ~(1ULL << thread.Slot)) | (thread.Status & (1ULL << thread.Slot)));
                        if (!context.write(thread.Handle.get())) failure = win::error("restoring debug registers");
                    }
                    else if (WaitForSingleObject(thread.Handle.get(), 0) != WAIT_OBJECT_0) failure = win::error("reading debug registers for restoration");
                }
                for (HANDLE thread : suspended) ResumeThread(thread);
                if (!DebugActiveProcessStop(static_cast<DWORD>(pid))) failure = win::error("DebugActiveProcessStop");
            }
            if (!failure.empty()) return failure;
            if (done && !exited) return "breakpoint capture complete";
            return exited ? "target process exited" : stop.stop_requested() ? "debug operation stopped" : "register capture timed out";
        }
    }

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
    ExecutionProbe::~ExecutionProbe() { stop(); }
    ExecutionProbe& executionProbe() { static ExecutionProbe probe; return probe; }
    bool ExecutionProbe::running() const noexcept { if (!_state) return false; std::lock_guard lock(_state->Mutex); return !_state->Finished; }
    std::string ExecutionProbe::status() const { if (!_state) return {}; std::lock_guard lock(_state->Mutex); return _state->Status; }
    std::optional<ExecutionProbeHit> ExecutionProbe::hit() const { if (!_state) return {}; std::lock_guard lock(_state->Mutex); return _state->Hit; }
    pid_t ExecutionProbe::pid() const noexcept { return _state ? _state->Pid : 0; }
    std::uintptr_t ExecutionProbe::address() const noexcept { return _state ? _state->Address : 0; }
    void ExecutionProbe::stop() noexcept { if (_state) { _state->Worker.request_stop(); _state.reset(); } }
    bool ExecutionProbe::start(pid_t pid, std::uintptr_t address, std::string& error)
    {
        stop();
        if (pid <= 0 || !address) { error = "select a process and an instruction address"; return false; }
        auto state = std::make_shared<ExecutionProbeState>(); auto* probe = state.get();
        state->Pid = pid; state->Address = address;
        state->Worker = std::jthread([probe, pid, address](std::stop_token stop)
        {
            auto status = [probe](std::string text) { std::lock_guard lock(probe->Mutex); probe->Status = std::move(text); };
            const auto result = watchBreakpoint(pid, address, 1, 0, stop, 0, [&](pid_t tid, const NativeRegisters& registers)
            {
                std::lock_guard lock(probe->Mutex);
                probe->Hit = ExecutionProbeHit{runtimeSteadySeconds(), pid, tid, address, registers, true};
                return true;
            }, status);
            std::lock_guard lock(probe->Mutex); probe->Status = result; probe->Finished = true;
        });
        _state = std::move(state); error.clear(); return true;
    }

    struct MemoryWatchState
    {
        std::mutex Mutex;
        bool Finished = false;
        std::string Status;
        std::vector<MemoryWatchHit> Hits;
        std::jthread Worker;
    };
    MemoryWatch::~MemoryWatch() { stop(); }
    bool MemoryWatch::running() const noexcept { if (!_state) return false; std::lock_guard lock(_state->Mutex); return !_state->Finished; }
    std::string MemoryWatch::status() const { if (!_state) return {}; std::lock_guard lock(_state->Mutex); return _state->Status; }
    std::vector<MemoryWatchHit> MemoryWatch::hits() const { if (!_state) return {}; std::lock_guard lock(_state->Mutex); return _state->Hits; }
    void MemoryWatch::clearHits() { if (_state) { std::lock_guard lock(_state->Mutex); _state->Hits.clear(); } }
    void MemoryWatch::stop() noexcept { if (_state) { _state->Worker.request_stop(); _state.reset(); } }
    bool MemoryWatch::start(pid_t pid, std::uintptr_t address, std::size_t size, MemoryWatchAccess access, std::size_t maxHits, std::string& error)
    {
        stop();
        if (pid <= 0 || !address || (size != 1 && size != 2 && size != 4 && size != 8) || (address & (size - 1)))
        { error = "select a process and a naturally aligned address of size 1, 2, 4, or 8"; return false; }
        auto state = std::make_shared<MemoryWatchState>(); auto* watch = state.get();
        state->Worker = std::jthread([watch,pid,address,size,access,maxHits](std::stop_token stop)
        {
            std::size_t total = 0;
            auto status = [watch](std::string text) { std::lock_guard lock(watch->Mutex); watch->Status = std::move(text); };
            const auto result = watchBreakpoint(pid,address,size,access == MemoryWatchAccess::Write ? 1 : 3,stop,0,[&](pid_t tid,const NativeRegisters& registers)
            {
                MemoryWatchHit hit; hit.Time = runtimeSteadySeconds(); hit.Tid = tid; hit.Rip = registers.rip;
                hit.Registers = registers; hit.HasRegisters = true; hit.Count = 1;
                std::array<std::uint8_t,15> bytes{}; std::string error;
                if (hit.Rip >= bytes.size() && readProcessMemoryBlock(pid, hit.Rip - bytes.size(), bytes, error))
                    for (std::size_t length = 15; length > 0; --length)
                    {
                        std::size_t decoded = 0; std::string text;
                        if (runtimeDecodeProcessInstructionText(pid, std::span<const std::uint8_t>(bytes).last(length), hit.Rip - length, text, decoded) && decoded == length)
                        { hit.InstructionAddress = hit.Rip - length; hit.Instruction = std::move(text); break; }
                    }
                std::lock_guard lock(watch->Mutex);
                auto found = std::ranges::find(watch->Hits, hit.Rip, &MemoryWatchHit::Rip);
                if (found == watch->Hits.end()) { watch->Hits.push_back(std::move(hit)); if (watch->Hits.size() > 256) watch->Hits.erase(watch->Hits.begin()); }
                else { hit.Count = found->Count + 1; *found = std::move(hit); }
                ++total; watch->Status = "watching | hits " + std::to_string(total) + " | sites " + std::to_string(watch->Hits.size());
                return maxHits && total >= maxHits;
            },status);
            std::lock_guard lock(watch->Mutex); watch->Status = result; watch->Finished = true;
        });
        _state = std::move(state); error.clear(); return true;
    }

    void startRuntimeRegisterCapture(RuntimeBinding& binding, pid_t pid, std::uintptr_t instruction, std::intptr_t displacement)
    {
        auto state = std::make_shared<RuntimeRegisterCaptureState>(); auto* capture = state.get();
        const auto selected = binding.SignatureRegister;
        const double timeout = std::clamp<double>(binding.SignatureCaptureTimeoutSeconds, 0.1, 120);
        state->Worker = std::jthread([capture,pid,instruction,displacement,selected,timeout](std::stop_token stop)
        {
            const auto result = watchBreakpoint(pid,instruction,1,0,stop,timeout,[&](pid_t,const NativeRegisters& registers)
            {
                std::lock_guard lock(capture->Mutex);
                capture->RegisterValue = runtimeX64RegisterValue(registers, selected);
                capture->Displacement = displacement;
                capture->ResolvedAddress = static_cast<std::uintptr_t>(capture->RegisterValue + displacement);
                capture->Success = true; return true;
            },[capture](std::string text) { std::lock_guard lock(capture->Mutex); capture->Status = std::move(text); });
            std::lock_guard lock(capture->Mutex); capture->Status = result; capture->Finished = true;
        });
        binding.SignatureRegisterCapture = std::move(state);
    }
}
#endif
