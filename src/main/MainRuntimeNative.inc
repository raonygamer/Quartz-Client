
    static std::vector<pid_t> enumerateRuntimeThreads(const pid_t pid)
    {
        std::vector<pid_t> result;
        std::error_code ec;
        const std::filesystem::path taskPath = "/proc/" + std::to_string(pid) + "/task";
        for (const auto& entry : std::filesystem::directory_iterator(taskPath, ec))
        {
            const std::string name = entry.path().filename().string();
            pid_t tid = 0;
            const auto [pointer, error] = std::from_chars(name.data(), name.data() + name.size(), tid);
            if (error == std::errc{} && pointer == name.data() + name.size() && tid > 0) result.push_back(tid);
        }
        std::ranges::sort(result);
        return result;
    }

    static bool runtimePtracePeekUser(const pid_t tid, const std::size_t offset, std::uint64_t& value) noexcept
    {
        errno = 0;
        const long result = ::ptrace(PTRACE_PEEKUSER, tid, reinterpret_cast<void*>(offset), nullptr);
        if (result == -1 && errno != 0) return false;
        value = static_cast<std::uint64_t>(static_cast<unsigned long>(result));
        return true;
    }

    static bool runtimePtracePokeUser(const pid_t tid, const std::size_t offset, const std::uint64_t value) noexcept
    {
        errno = 0;
        return ::ptrace(PTRACE_POKEUSER, tid, reinterpret_cast<void*>(offset), reinterpret_cast<void*>(static_cast<std::uintptr_t>(value))) == 0;
    }

    static bool runtimeWaitForPtraceStop(const pid_t tid, std::stop_token stop, const double timeoutSeconds, int& status) noexcept
    {
        const double deadline = runtimeSteadySeconds() + timeoutSeconds;
        while (!stop.stop_requested() && runtimeSteadySeconds() < deadline)
        {
            const pid_t result = ::waitpid(tid, &status, __WALL | WNOHANG);
            if (result == tid) return WIFSTOPPED(status);
            if (result < 0 && errno != EINTR) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    static void startRuntimeRegisterCapture(RuntimeBinding& binding, const pid_t pid, const std::uintptr_t instruction, const std::intptr_t displacement)
    {
        auto state = std::make_shared<RuntimeRegisterCaptureState>();
        RuntimeRegisterCaptureState* capture = state.get();
        const RuntimeX64Register targetRegister = binding.SignatureRegister;
        const double timeoutSeconds = std::clamp(static_cast<double>(binding.SignatureCaptureTimeoutSeconds), 0.1, 120.0);
        {
            std::lock_guard lock(capture->Mutex);
            std::ostringstream status;
            status << "Waiting for " << runtimeX64RegisterName(targetRegister) << " at 0x" << std::hex << instruction;
            capture->Status = status.str();
        }
        state->Worker = std::jthread([capture, pid, instruction, displacement, targetRegister, timeoutSeconds](std::stop_token stop)
        {
            struct TracedThread
            {
                pid_t Tid = 0;
                bool Attached = false;
                bool Stopped = false;
                bool Armed = false;
                std::uint64_t Dr0 = 0;
                std::uint64_t Dr6 = 0;
                std::uint64_t Dr7 = 0;
            };

            auto finish = [&](const bool success, const std::string& status, const std::uint64_t registerValue = 0, const std::uintptr_t resolved = 0)
            {
                std::lock_guard lock(capture->Mutex);
                capture->Finished = true;
                capture->Success = success;
                capture->RegisterValue = registerValue;
                capture->Displacement = displacement;
                capture->ResolvedAddress = resolved;
                capture->Status = status;
            };

            constexpr std::size_t Dr0Offset = offsetof(struct user, u_debugreg[0]);
            constexpr std::size_t Dr6Offset = offsetof(struct user, u_debugreg[6]);
            constexpr std::size_t Dr7Offset = offsetof(struct user, u_debugreg[7]);
            std::vector<TracedThread> threads;
            std::size_t armedCount = 0;
            std::uint64_t trapCount = 0;
            std::uint64_t lastTrapRip = 0;
            std::uint64_t lastTrapDr6 = 0;
            std::size_t seizeFailures = 0;
            std::size_t setupFailures = 0;
            int lastSeizeError = 0;
            int lastSetupError = 0;

            auto markGone = [&](TracedThread& thread)
            {
                thread.Attached = false;
                thread.Stopped = false;
                if (thread.Armed && armedCount > 0) --armedCount;
                thread.Armed = false;
            };

            auto restoreDebugRegisters = [&](TracedThread& thread)
            {
                if (!thread.Armed || !thread.Stopped) return;
                runtimePtracePokeUser(thread.Tid, Dr7Offset, thread.Dr7);
                runtimePtracePokeUser(thread.Tid, Dr0Offset, thread.Dr0);
                runtimePtracePokeUser(thread.Tid, Dr6Offset, thread.Dr6);
                thread.Armed = false;
                if (armedCount > 0) --armedCount;
            };

            auto detachStopped = [&](TracedThread& thread, const int signal = 0)
            {
                if (!thread.Attached || !thread.Stopped) return true;
                restoreDebugRegisters(thread);
                errno = 0;
                if (::ptrace(PTRACE_DETACH, thread.Tid, nullptr, reinterpret_cast<void*>(static_cast<std::uintptr_t>(signal))) == 0)
                {
                    thread.Attached = false;
                    thread.Stopped = false;
                    return true;
                }
                if (errno == ESRCH || errno == ECHILD)
                {
                    markGone(thread);
                    return true;
                }
                return false;
            };

            auto continueStopped = [&](TracedThread& thread, const int signal = 0)
            {
                if (!thread.Attached || !thread.Stopped) return false;
                errno = 0;
                if (::ptrace(PTRACE_CONT, thread.Tid, nullptr, reinterpret_cast<void*>(static_cast<std::uintptr_t>(signal))) == 0)
                {
                    thread.Stopped = false;
                    return true;
                }
                if (errno == ESRCH || errno == ECHILD) markGone(thread);
                else detachStopped(thread, signal);
                return false;
            };

            auto stopForCleanup = [&](TracedThread& thread, int& status, const bool interruptPending = false)
            {
                status = 0;
                if (!thread.Attached) return false;
                if (thread.Stopped) return true;

                errno = 0;
                const pid_t pending = ::waitpid(thread.Tid, &status, __WALL | WNOHANG);
                if (pending == thread.Tid)
                {
                    if (WIFEXITED(status) || WIFSIGNALED(status)) { markGone(thread); return false; }
                    if (WIFSTOPPED(status)) { thread.Stopped = true; return true; }
                }
                else if (pending < 0 && errno != EINTR)
                {
                    if (errno == ESRCH || errno == ECHILD) markGone(thread);
                    return false;
                }

                if (!interruptPending)
                {
                    errno = 0;
                    if (::ptrace(PTRACE_INTERRUPT, thread.Tid, nullptr, nullptr) != 0)
                    {
                        if (errno == ESRCH || errno == ECHILD) markGone(thread);
                        return false;
                    }
                }

                for (;;)
                {
                    errno = 0;
                    const pid_t result = ::waitpid(thread.Tid, &status, __WALL);
                    if (result == thread.Tid)
                    {
                        if (WIFEXITED(status) || WIFSIGNALED(status)) { markGone(thread); return false; }
                        if (WIFSTOPPED(status)) { thread.Stopped = true; return true; }
                        continue;
                    }
                    if (result < 0 && errno == EINTR) continue;
                    if (result < 0 && (errno == ESRCH || errno == ECHILD)) markGone(thread);
                    return false;
                }
            };

            auto alreadyTracked = [&](const pid_t tid)
            {
                return std::ranges::any_of(threads, [tid](const TracedThread& thread) { return thread.Tid == tid && thread.Attached; });
            };

            auto armThread = [&](const pid_t tid)
            {
                if (tid <= 0 || alreadyTracked(tid) || stop.stop_requested()) return false;
                errno = 0;
                if (::ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) != 0)
                {
                    ++seizeFailures;
                    lastSeizeError = errno;
                    return false;
                }

                threads.push_back({});
                TracedThread& thread = threads.back();
                thread.Tid = tid;
                thread.Attached = true;

                errno = 0;
                if (::ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) != 0)
                {
                    ++setupFailures;
                    lastSetupError = errno;
                    if (errno == ESRCH || errno == ECHILD) markGone(thread);
                    return false;
                }

                int status = 0;
                if (!runtimeWaitForPtraceStop(tid, stop, 0.25, status))
                {
                    ++setupFailures;
                    lastSetupError = errno;
                    int cleanupStatus = 0;
                    if (stopForCleanup(thread, cleanupStatus, true))
                    {
                        const int signal = WIFSTOPPED(cleanupStatus) ? WSTOPSIG(cleanupStatus) : 0;
                        detachStopped(thread, signal == SIGTRAP || signal == SIGSTOP ? 0 : signal);
                    }
                    return false;
                }
                thread.Stopped = true;

                if (!runtimePtracePeekUser(tid, Dr0Offset, thread.Dr0) || !runtimePtracePeekUser(tid, Dr6Offset, thread.Dr6) || !runtimePtracePeekUser(tid, Dr7Offset, thread.Dr7))
                {
                    ++setupFailures;
                    lastSetupError = errno;
                    const int signal = WSTOPSIG(status);
                    detachStopped(thread, signal == SIGTRAP || signal == SIGSTOP ? 0 : signal);
                    return false;
                }

                std::uint64_t dr7 = thread.Dr7;
                dr7 &= ~0x3ULL;
                dr7 &= ~(0xFULL << 16);
                dr7 |= 0x1ULL;
                if (!runtimePtracePokeUser(tid, Dr0Offset, instruction) || !runtimePtracePokeUser(tid, Dr6Offset, 0) || !runtimePtracePokeUser(tid, Dr7Offset, dr7))
                {
                    ++setupFailures;
                    lastSetupError = errno;
                    detachStopped(thread);
                    return false;
                }

                thread.Armed = true;
                ++armedCount;
                const int signal = WSTOPSIG(status);
                if (!continueStopped(thread, signal == SIGTRAP || signal == SIGSTOP ? 0 : signal)) return false;
                return true;
            };

            auto refreshThreads = [&]
            {
                for (const pid_t tid : enumerateRuntimeThreads(pid)) armThread(tid);
            };

            auto updateStatus = [&]
            {
                std::lock_guard lock(capture->Mutex);
                std::ostringstream status;
                status << "Waiting for " << runtimeX64RegisterName(targetRegister) << " at 0x" << std::hex << instruction << std::dec
                       << " | armed " << armedCount << "/" << threads.size() << " | traps " << trapCount;
                if (lastTrapRip != 0) status << " | last RIP 0x" << std::hex << lastTrapRip << " DR6 0x" << lastTrapDr6 << std::dec;
                if (seizeFailures != 0) status << " | seize failures " << seizeFailures << " (" << std::strerror(lastSeizeError) << ')';
                if (setupFailures != 0) status << " | setup failures " << setupFailures << (lastSetupError ? std::string(" (") + std::strerror(lastSetupError) + ')' : std::string{});
                capture->Status = status.str();
            };

            refreshThreads();
            if (threads.empty())
            {
                finish(false, std::string("ptrace seize failed: ") + std::strerror(lastSeizeError ? lastSeizeError : errno));
                return;
            }

            bool success = false;
            std::uint64_t capturedRegister = 0;
            std::uintptr_t resolvedAddress = 0;
            std::string failure;
            if (armedCount == 0) failure = "could not arm a hardware execution breakpoint";
            const double deadline = runtimeSteadySeconds() + timeoutSeconds;
            double nextThreadRefresh = runtimeSteadySeconds() + 0.10;
            double nextStatusUpdate = 0.0;

            while (!stop.stop_requested() && runtimeSteadySeconds() < deadline && !success)
            {
                const double now = runtimeSteadySeconds();
                if (now >= nextThreadRefresh)
                {
                    refreshThreads();
                    nextThreadRefresh = now + 0.10;
                    if (armedCount > 0) failure.clear();
                }
                if (armedCount == 0)
                {
                    if (now >= nextStatusUpdate) { updateStatus(); nextStatusUpdate = now + 0.25; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }

                bool observed = false;
                for (auto& thread : threads)
                {
                    if (!thread.Attached || thread.Stopped) continue;
                    int status = 0;
                    errno = 0;
                    const pid_t result = ::waitpid(thread.Tid, &status, __WALL | WNOHANG);
                    if (result == 0) continue;
                    if (result < 0)
                    {
                        if (errno == ECHILD || errno == ESRCH) markGone(thread);
                        continue;
                    }
                    observed = true;
                    if (WIFEXITED(status) || WIFSIGNALED(status))
                    {
                        markGone(thread);
                        continue;
                    }
                    if (!WIFSTOPPED(status)) continue;
                    thread.Stopped = true;

                    if (!thread.Armed)
                    {
                        detachStopped(thread);
                        continue;
                    }

                    const int signal = WSTOPSIG(status);
                    if (signal == SIGTRAP)
                    {
                        ++trapCount;
                        std::uint64_t dr6 = 0;
                        user_regs_struct regs{};
                        const bool haveDr6 = runtimePtracePeekUser(thread.Tid, Dr6Offset, dr6);
                        const bool haveRegs = ::ptrace(PTRACE_GETREGS, thread.Tid, nullptr, &regs) == 0;
                        if (haveRegs) lastTrapRip = regs.rip;
                        if (haveDr6) lastTrapDr6 = dr6;
                        const bool slot0Hit = haveDr6 && (dr6 & 0x1ULL) != 0;
                        const bool targetRip = haveRegs && regs.rip == instruction;
                        if (haveRegs && (slot0Hit || targetRip))
                        {
                            capturedRegister = runtimeX64RegisterValue(regs, targetRegister);
                            resolvedAddress = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(capturedRegister) + displacement);
                            success = true;
                            detachStopped(thread);
                            break;
                        }
                    }

                    const int deliverSignal = signal == SIGTRAP || signal == SIGSTOP ? 0 : signal;
                    continueStopped(thread, deliverSignal);
                }

                if (now >= nextStatusUpdate) { updateStatus(); nextStatusUpdate = now + 0.25; }
                if (!observed) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            for (auto& thread : threads)
            {
                if (!thread.Attached) continue;
                int status = 0;
                if (!stopForCleanup(thread, status)) continue;
                const int signal = WIFSTOPPED(status) ? WSTOPSIG(status) : 0;
                if (!detachStopped(thread, signal == SIGTRAP || signal == SIGSTOP ? 0 : signal)) continueStopped(thread, signal == SIGTRAP || signal == SIGSTOP ? 0 : signal);
            }

            if (success)
            {
                std::ostringstream status;
                status << "Captured " << runtimeX64RegisterName(targetRegister) << "=0x" << std::hex << capturedRegister;
                if (displacement >= 0) status << " +0x" << static_cast<std::uint64_t>(displacement);
                else status << " -0x" << static_cast<std::uint64_t>(-displacement);
                status << " -> 0x" << resolvedAddress << std::dec << " | traps " << trapCount;
                finish(true, status.str(), capturedRegister, resolvedAddress);
            }
            else if (stop.stop_requested()) finish(false, "register capture cancelled");
            else if (armedCount == 0 && !failure.empty()) finish(false, failure);
            else
            {
                std::ostringstream status;
                status << "register capture timed out | armed " << armedCount << '/' << threads.size() << " | traps " << trapCount;
                if (lastTrapRip != 0) status << " | last RIP 0x" << std::hex << lastTrapRip << " DR6 0x" << lastTrapDr6;
                finish(false, status.str());
            }
        });
        binding.SignatureRegisterCapture = std::move(state);
    }

    static bool readRuntimeRegisterDisplacement(const RuntimeBinding& binding, const pid_t pid, const std::uintptr_t instruction, std::intptr_t& displacement, std::string& error)
    {
        if (binding.SignatureDisplacementType == RuntimeDisplacementType::Manual)
        {
            displacement = binding.SignatureManualDisplacement;
            error.clear();
            return true;
        }
        const std::uintptr_t address = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(instruction) + binding.SignatureRegisterDisplacementOffset);
        if (binding.SignatureDisplacementType == RuntimeDisplacementType::I8)
        {
            std::int8_t value = 0;
            if (!readProcessMemoryValue(pid, address, value, error)) return false;
            displacement = value;
        }
        else
        {
            std::int32_t value = 0;
            if (!readProcessMemoryValue(pid, address, value, error)) return false;
            displacement = value;
        }
        error.clear();
        return true;
    }

    static std::optional<std::uintptr_t> advanceRuntimeRegisterCapture(RuntimeBinding& binding, const pid_t pid, std::string& error)
    {
        const double now = runtimeSteadySeconds();
        if (binding.SignatureResolvedAddress != 0) { error.clear(); return binding.SignatureResolvedAddress; }
        if (binding.SignatureInstructionAddress == 0) { error = "register capture has no instruction address"; return std::nullopt; }
        if (binding.SignatureRegisterCapture)
        {
            bool finished = false, success = false;
            std::uint64_t registerValue = 0;
            std::intptr_t displacement = 0;
            std::uintptr_t resolved = 0;
            std::string status;
            {
                std::lock_guard lock(binding.SignatureRegisterCapture->Mutex);
                finished = binding.SignatureRegisterCapture->Finished;
                success = binding.SignatureRegisterCapture->Success;
                registerValue = binding.SignatureRegisterCapture->RegisterValue;
                displacement = binding.SignatureRegisterCapture->Displacement;
                resolved = binding.SignatureRegisterCapture->ResolvedAddress;
                status = binding.SignatureRegisterCapture->Status;
            }
            binding.SignatureStatus = status;
            if (!finished) { error = status; return std::nullopt; }
            binding.SignatureRegisterCapture.reset();
            if (success && resolved != 0)
            {
                binding.SignatureCapturedRegister = registerValue;
                binding.SignatureCapturedDisplacement = displacement;
                binding.SignatureResolvedAddress = resolved;
                error.clear();
                return resolved;
            }
            binding.NextRegisterCapture = now + std::max(static_cast<double>(binding.SignatureRetrySeconds), 0.1);
            error = status;
            return std::nullopt;
        }
        if (now < binding.NextRegisterCapture)
        {
            const double remaining = std::max(binding.NextRegisterCapture - now, 0.0);
            std::ostringstream status;
            status << "register capture retry in " << std::fixed << std::setprecision(1) << remaining << " s";
            binding.SignatureStatus = status.str();
            error = binding.SignatureStatus;
            return std::nullopt;
        }
        startRuntimeRegisterCapture(binding, pid, binding.SignatureInstructionAddress, binding.SignatureCapturedDisplacement);
        binding.SignatureStatus = "Waiting for instruction execution";
        error = binding.SignatureStatus;
        return std::nullopt;
    }

    static std::uint64_t runtimeSignatureConfigurationHash(const RuntimeBinding& binding)
    {
        std::size_t hash = std::hash<std::string_view>{}(binding.Signature);
        auto mix = [&](const std::size_t value) { hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2); };
        mix(std::hash<std::string_view>{}(binding.Module));
        mix(static_cast<std::size_t>(binding.SignaturePatternKind));
        mix(static_cast<std::size_t>(binding.SignatureExecutableOnly));
        mix(static_cast<std::size_t>(binding.SignatureResolve));
        mix(static_cast<std::size_t>(static_cast<std::uint32_t>(binding.SignatureResultOffset)));
        mix(static_cast<std::size_t>(static_cast<std::uint32_t>(binding.SignatureInstructionSize)));
        mix(static_cast<std::size_t>(binding.SignatureRegister));
        mix(static_cast<std::size_t>(static_cast<std::uint32_t>(binding.SignatureRegisterDisplacementOffset)));
        mix(static_cast<std::size_t>(binding.SignatureDisplacementType));
        mix(static_cast<std::size_t>(static_cast<std::uint32_t>(binding.SignatureManualDisplacement)));
        return static_cast<std::uint64_t>(hash);
    }

    static void resetRuntimeSignatureScan(RuntimeBinding& binding, const bool clearResolved = true)
    {
        binding.SignatureBytes.clear();
        binding.SignatureMasks.clear();
        binding.SignatureRegions.clear();
        binding.SignatureRegionIndex = 0;
        binding.SignatureCursor = 0;
        binding.SignatureScannedBytes = 0;
        binding.SignatureTotalBytes = 0;
        binding.SignatureProgress = 0.0f;
        binding.NextSignatureScan = 0.0;
        binding.NextRegisterCapture = 0.0;
        binding.SignatureRegisterCapture.reset();
        if (clearResolved)
        {
            binding.SignatureResolvedAddress = 0;
            binding.SignatureMatchAddress = 0;
            binding.SignatureInstructionAddress = 0;
            binding.SignatureCapturedRegister = 0;
            binding.SignatureCapturedDisplacement = 0;
        }
        binding.SignatureStatus.clear();
    }

    static bool prepareRuntimeSignatureScan(RuntimeBinding& binding, const pid_t pid, std::string& error)
    {
        if (binding.SignaturePatternKind == RuntimeSignaturePatternKind::HexadecimalPattern)
        {
            if (!parseRuntimeHexPattern(binding.Signature, binding.SignatureBytes, binding.SignatureMasks, error)) return false;
        }
        else
        {
#if QUARTZ_HAS_ZYDIS
            if (parseRuntimeOpcodePattern(binding.Signature).empty()) { error = "opcode pattern is empty"; return false; }
            binding.SignatureBytes.assign(1, 0); binding.SignatureMasks.assign(1, 0);
#else
            error = "opcode patterns require Zydis (<Zydis/Zydis.h>)"; return false;
#endif
        }
        binding.SignatureRegions.clear();
        binding.SignatureTotalBytes = 0;
        binding.SignatureScannedBytes = 0;
        for (auto& region : enumerateRuntimeRegions(pid))
        {
            if (!region.Readable || (binding.SignatureExecutableOnly && !region.Executable)) continue;
            if (binding.Module[0] != '\0')
            {
                const std::string module = binding.Module;
                const std::string name = region.Path.empty() ? std::string{} : std::filesystem::path(region.Path).filename().string();
                if (name != module && region.Path != module && region.Path.find(module) == std::string::npos) continue;
            }
            if (region.End - region.Base < binding.SignatureBytes.size()) continue;
            binding.SignatureTotalBytes += region.End - region.Base;
            binding.SignatureRegions.emplace_back(std::move(region));
        }
        if (binding.SignatureRegions.empty())
        {
            error = binding.Module[0] ? "no readable matching mappings for pattern scan" : "no readable mappings for pattern scan";
            return false;
        }
        binding.SignatureRegionIndex = 0;
        binding.SignatureCursor = binding.SignatureRegions.front().Base;
        binding.SignatureScannedBytes = 0;
        binding.SignatureProgress = 0.0f;
        binding.SignatureScanPid = pid;
        binding.SignatureConfigHash = runtimeSignatureConfigurationHash(binding);
        binding.SignatureStatus = "Scanning pattern";
        error.clear();
        return true;
    }

    static bool runtimeSignatureMatches(const std::span<const std::uint8_t> data, const std::size_t offset, const RuntimeBinding& binding) noexcept
    {
        for (std::size_t i = 0; i < binding.SignatureBytes.size(); ++i)
            if ((data[offset + i] & binding.SignatureMasks[i]) != (binding.SignatureBytes[i] & binding.SignatureMasks[i])) return false;
        return true;
    }

    template<typename T>
    static bool readProcessMemoryValue(const pid_t pid, const std::uintptr_t address, T& value, std::string& error)
    {
        iovec local{&value, sizeof(T)};
        iovec remote{reinterpret_cast<void*>(address), sizeof(T)};
        errno = 0;
        const ssize_t count = ::process_vm_readv(pid, &local, 1, &remote, 1, 0);
        if (count == static_cast<ssize_t>(sizeof(T))) return true;
        error = count < 0 ? std::string(std::strerror(errno)) : "short read (" + std::to_string(count) + "/" + std::to_string(sizeof(T)) + ")";
        return false;
    }

    static bool parseRuntimeInteger(const std::string_view text, std::intptr_t& value)
    {
        std::string token = trim(std::string(text));
        if (token.empty()) return false;
        bool negative = false;
        if (token.front() == '+' || token.front() == '-')
        {
            negative = token.front() == '-';
            token.erase(token.begin());
        }
        int base = 10;
        if (token.starts_with("0x") || token.starts_with("0X"))
        {
            base = 16;
            token.erase(0, 2);
        }
        if (token.empty()) return false;
        std::uintptr_t raw = 0;
        const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), raw, base);
        if (ec != std::errc{} || ptr != token.data() + token.size()) return false;
        value = negative ? -static_cast<std::intptr_t>(raw) : static_cast<std::intptr_t>(raw);
        return true;
    }

    static std::optional<std::uintptr_t> resolveRuntimeAddress(const RuntimeBinding& binding, const pid_t pid, std::string& error, const std::optional<std::uintptr_t> baseOverride = std::nullopt)
    {
        std::string expression = trim(binding.Address);
        if (expression.empty()) { error = "empty address expression"; return std::nullopt; }

        std::vector<std::string> terms;
        for (std::size_t start = 0;;)
        {
            const std::size_t arrow = expression.find("->", start);
            terms.emplace_back(trim(expression.substr(start, arrow == std::string::npos ? std::string::npos : arrow - start)));
            if (arrow == std::string::npos) break;
            start = arrow + 2;
        }
        if (terms.empty()) { error = "invalid address expression"; return std::nullopt; }

        auto modules = enumerateRuntimeModules(pid);
        std::uintptr_t moduleBase = baseOverride.value_or(0);
        if (!baseOverride && binding.Module[0] != '\0')
        {
            const std::string wanted = binding.Module;
            const auto it = std::ranges::find_if(modules, [&](const RuntimeProcessModule& module)
            {
                return module.Name == wanted || module.Path == wanted || module.Path.find(wanted) != std::string::npos;
            });
            if (it == modules.end()) { error = "module not found: " + wanted; return std::nullopt; }
            moduleBase = it->Base;
        }

        std::string first = terms.front();
        if (const std::size_t plus = first.find('+'); plus != std::string::npos && plus > 0 && !std::isdigit(static_cast<unsigned char>(first[0])))
        {
            const std::string moduleName = trim(first.substr(0, plus));
            const auto it = std::ranges::find_if(modules, [&](const RuntimeProcessModule& module)
            {
                return module.Name == moduleName || module.Path.find(moduleName) != std::string::npos;
            });
            if (it == modules.end()) { error = "module not found: " + moduleName; return std::nullopt; }
            moduleBase = it->Base;
            first = first.substr(plus);
        }

        std::intptr_t firstValue = 0;
        if (!parseRuntimeInteger(first, firstValue)) { error = "invalid address term: " + first; return std::nullopt; }
        std::uintptr_t address = first.front() == '+' || first.front() == '-' || moduleBase != 0 ? static_cast<std::uintptr_t>(static_cast<std::intptr_t>(moduleBase) + firstValue) : static_cast<std::uintptr_t>(firstValue);

        for (std::size_t i = 1; i < terms.size(); ++i)
        {
            std::uintptr_t pointer = 0;
            if (!readProcessMemoryValue(pid, address, pointer, error)) { error = "pointer read failed at 0x" + [&]{ std::ostringstream s; s << std::hex << address; return s.str(); }() + ": " + error; return std::nullopt; }
            std::intptr_t offset = 0;
            if (!parseRuntimeInteger(terms[i], offset)) { error = "invalid pointer offset: " + terms[i]; return std::nullopt; }
            address = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(pointer) + offset);
        }
        return address;
    }

    static std::optional<std::uintptr_t> advanceRuntimeSignatureScan(RuntimeBinding& binding, const pid_t pid, std::string& error)
    {
        const double now = runtimeSteadySeconds();
        const std::uint64_t configurationHash = runtimeSignatureConfigurationHash(binding);
        if (binding.SignatureScanPid != pid || binding.SignatureConfigHash != configurationHash)
        {
            resetRuntimeSignatureScan(binding);
            binding.SignatureScanPid = pid;
            binding.SignatureConfigHash = configurationHash;
        }
        if (binding.SignatureResolvedAddress != 0)
        {
            error.clear();
            return binding.SignatureResolvedAddress;
        }
        if (binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture && binding.SignatureInstructionAddress != 0)
            return advanceRuntimeRegisterCapture(binding, pid, error);
        if (now < binding.NextSignatureScan)
        {
            const double remaining = std::max(binding.NextSignatureScan - now, 0.0);
            std::ostringstream status;
            status << "pattern not found; retry in " << std::fixed << std::setprecision(1) << remaining << " s";
            binding.SignatureStatus = status.str();
            error = binding.SignatureStatus;
            return std::nullopt;
        }
        if (binding.SignatureRegions.empty() && !prepareRuntimeSignatureScan(binding, pid, error))
        {
            binding.SignatureStatus = error;
            binding.NextSignatureScan = now + std::max(static_cast<double>(binding.SignatureRetrySeconds), 0.1);
            return std::nullopt;
        }

        const auto opcodePatterns = binding.SignaturePatternKind == RuntimeSignaturePatternKind::OpcodePattern ? parseRuntimeOpcodePattern(binding.Signature) : std::vector<std::string>{};
        const std::size_t ScanBudget = binding.SignaturePatternKind == RuntimeSignaturePatternKind::OpcodePattern ? 32 * 1024 : 256 * 1024;
        const std::size_t ReadChunk = binding.SignaturePatternKind == RuntimeSignaturePatternKind::OpcodePattern ? 32 * 1024 : 128 * 1024;
        const std::size_t scanOverlap = binding.SignaturePatternKind == RuntimeSignaturePatternKind::OpcodePattern ? std::min<std::size_t>(std::max<std::size_t>(opcodePatterns.size() * 15, 15) - 1, 4095) : binding.SignatureBytes.size() > 1 ? binding.SignatureBytes.size() - 1 : 0;
        std::size_t budget = ScanBudget;
        std::vector<std::uint8_t> buffer;
        while (budget > 0 && binding.SignatureRegionIndex < binding.SignatureRegions.size())
        {
            const auto& region = binding.SignatureRegions[binding.SignatureRegionIndex];
            if (binding.SignatureCursor < region.Base) binding.SignatureCursor = region.Base;
            if (binding.SignatureCursor >= region.End)
            {
                ++binding.SignatureRegionIndex;
                if (binding.SignatureRegionIndex < binding.SignatureRegions.size()) binding.SignatureCursor = binding.SignatureRegions[binding.SignatureRegionIndex].Base;
                continue;
            }
            const std::size_t remaining = static_cast<std::size_t>(region.End - binding.SignatureCursor);
            const std::size_t readSize = std::min({remaining, ReadChunk + scanOverlap, budget + scanOverlap});
            if (readSize < binding.SignatureBytes.size())
            {
                binding.SignatureScannedBytes += remaining;
                binding.SignatureCursor = region.End;
                continue;
            }
            buffer.resize(readSize);
            std::string readError;
            if (!readProcessMemoryBlock(pid, binding.SignatureCursor, buffer, readError))
            {
                binding.SignatureScannedBytes += remaining;
                binding.SignatureCursor = region.End;
                continue;
            }
            const std::size_t last = buffer.size() - binding.SignatureBytes.size();
            for (std::size_t offset = 0; offset <= last; ++offset)
            {
                std::size_t opcodeLength = 0;
                const bool matched = binding.SignaturePatternKind == RuntimeSignaturePatternKind::HexadecimalPattern ? runtimeSignatureMatches(buffer, offset, binding) : runtimeOpcodePatternMatches(std::span<const std::uint8_t>(buffer).subspan(offset), binding.SignatureCursor + offset, opcodePatterns, opcodeLength);
                if (!matched) continue;
                const std::uintptr_t match = binding.SignatureCursor + offset;
                std::uintptr_t resolved = 0;
                if (binding.SignatureResolve == SignatureResultMode::MatchAddress)
                    resolved = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + binding.SignatureResultOffset);
                else if (binding.SignatureResolve == SignatureResultMode::PointerAtOffset)
                {
                    const std::uintptr_t pointerAddress = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + binding.SignatureResultOffset);
                    if (!readProcessMemoryValue(pid, pointerAddress, resolved, error))
                    {
                        binding.SignatureStatus = "signature matched, pointer resolve failed: " + error;
                        return std::nullopt;
                    }
                }
                else if (binding.SignatureResolve == SignatureResultMode::RipRelative32)
                {
                    std::int32_t displacement = 0;
                    const std::uintptr_t displacementAddress = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + binding.SignatureResultOffset);
                    if (!readProcessMemoryValue(pid, displacementAddress, displacement, error))
                    {
                        binding.SignatureStatus = "signature matched, RIP displacement read failed: " + error;
                        return std::nullopt;
                    }
                    const int instructionSize = std::max(binding.SignatureInstructionSize, 1);
                    resolved = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + instructionSize + displacement);
                }
                else if (binding.SignatureResolve == SignatureResultMode::Address32AtOffset)
                {
                    std::uint32_t address32 = 0;
                    const std::uintptr_t immediateAddress = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + binding.SignatureResultOffset);
                    if (!readProcessMemoryValue(pid, immediateAddress, address32, error))
                    {
                        binding.SignatureStatus = "signature matched, 32-bit address read failed: " + error;
                        return std::nullopt;
                    }
                    resolved = static_cast<std::uintptr_t>(address32);
                }
                else if (binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture)
                {
                    binding.SignatureMatchAddress = match;
                    binding.SignatureInstructionAddress = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + binding.SignatureResultOffset);
                    std::intptr_t displacement = 0;
                    if (!readRuntimeRegisterDisplacement(binding, pid, binding.SignatureInstructionAddress, displacement, error))
                    {
                        binding.SignatureStatus = "signature matched, displacement read failed: " + error;
                        return std::nullopt;
                    }
                    binding.SignatureCapturedDisplacement = displacement;
                    binding.SignatureProgress = 1.0f;
                    std::ostringstream status;
                    status << "Writer located 0x" << std::hex << binding.SignatureInstructionAddress << "; waiting for " << runtimeX64RegisterName(binding.SignatureRegister);
                    binding.SignatureStatus = status.str();
                    return advanceRuntimeRegisterCapture(binding, pid, error);
                }
                binding.SignatureResolvedAddress = resolved;
                binding.SignatureMatchAddress = match;
                binding.SignatureProgress = 1.0f;
                std::ostringstream status;
                status << "Pattern resolved 0x" << std::hex << resolved << " from match 0x" << match;
                binding.SignatureStatus = status.str();
                error.clear();
                return resolved;
            }
            const std::size_t overlap = scanOverlap;
            const std::size_t step = readSize > overlap ? readSize - overlap : readSize;
            binding.SignatureCursor += step;
            binding.SignatureScannedBytes += step;
            budget = step >= budget ? 0 : budget - step;
            binding.SignatureProgress = binding.SignatureTotalBytes ? std::clamp(static_cast<float>(static_cast<double>(binding.SignatureScannedBytes) / binding.SignatureTotalBytes), 0.0f, 1.0f) : 0.0f;
        }

        if (binding.SignatureRegionIndex >= binding.SignatureRegions.size())
        {
            binding.SignatureRegions.clear();
            binding.SignatureRegionIndex = 0;
            binding.SignatureCursor = 0;
            binding.NextSignatureScan = now + std::max(static_cast<double>(binding.SignatureRetrySeconds), 0.1);
            binding.SignatureProgress = 0.0f;
            binding.SignatureStatus = "Pattern not found; waiting for retry interval";
            error = binding.SignatureStatus;
            return std::nullopt;
        }
        std::ostringstream status;
        status << "Scanning pattern " << std::fixed << std::setprecision(0) << binding.SignatureProgress * 100.0f << "%";
        binding.SignatureStatus = status.str();
        error = binding.SignatureStatus;
        return std::nullopt;
    }

    static std::string runtimeHexAddress(std::uintptr_t value);

    static bool readNativeBinding(RuntimeBinding& binding, float& output)
    {
        binding.HasAddress = false;
        pid_t pid = static_cast<pid_t>(binding.ProcessId);
        if (!runtimeProcessIsAlive(pid) && !tryRuntimeProcessRebind(binding, pid, binding.Error)) return false;

        std::string error;
        std::optional<std::uintptr_t> signatureBase;
        if (binding.AddressMode == ProcessAddressMode::Signature)
        {
            signatureBase = advanceRuntimeSignatureScan(binding, pid, error);
            if (!signatureBase)
            {
                binding.Error = std::move(error);
                return false;
            }
        }
        const auto address = resolveRuntimeAddress(binding, pid, error, signatureBase);
        if (!address)
        {
            if (binding.AddressMode == ProcessAddressMode::Signature && binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture)
            {
                binding.SignatureResolvedAddress = 0;
                binding.NextRegisterCapture = 0.0;
                binding.SignatureRegisterCapture.reset();
            }
            binding.HasAddress = false;
            binding.Error = std::move(error);
            return false;
        }
        binding.AddressValue = *address;
        binding.HasAddress = true;
        binding.AddressProvenance.clear();
        binding.AddressProvenance.push_back(std::string("process ") + binding.ProcessName + " pid " + std::to_string(pid));
        if (binding.AddressMode == ProcessAddressMode::Signature)
        {
            if (binding.SignatureMatchAddress) binding.AddressProvenance.push_back("pattern match " + runtimeHexAddress(binding.SignatureMatchAddress));
            if (binding.SignatureInstructionAddress) binding.AddressProvenance.push_back("instruction " + runtimeHexAddress(binding.SignatureInstructionAddress));
            if (binding.SignatureCapturedRegister) binding.AddressProvenance.push_back(std::string(runtimeX64RegisterName(binding.SignatureRegister)) + " = " + runtimeHexAddress(static_cast<std::uintptr_t>(binding.SignatureCapturedRegister)));
        }
        binding.AddressProvenance.push_back("resolved " + runtimeHexAddress(binding.AddressValue));

#define QUARTZ_READ_NATIVE(type) do { type value{}; if (!readProcessMemoryValue(pid, *address, value, error)) { if (binding.AddressMode == ProcessAddressMode::Signature && binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture) { binding.SignatureResolvedAddress = 0; binding.NextRegisterCapture = 0.0; binding.SignatureRegisterCapture.reset(); } binding.Error = std::move(error); return false; } output = static_cast<float>(value); } while (false)
        switch (binding.ValueType)
        {
        case ProcessValueType::U8: QUARTZ_READ_NATIVE(std::uint8_t); break;
        case ProcessValueType::I8: QUARTZ_READ_NATIVE(std::int8_t); break;
        case ProcessValueType::U16: QUARTZ_READ_NATIVE(std::uint16_t); break;
        case ProcessValueType::I16: QUARTZ_READ_NATIVE(std::int16_t); break;
        case ProcessValueType::U32: QUARTZ_READ_NATIVE(std::uint32_t); break;
        case ProcessValueType::I32: QUARTZ_READ_NATIVE(std::int32_t); break;
        case ProcessValueType::U64: QUARTZ_READ_NATIVE(std::uint64_t); break;
        case ProcessValueType::I64: QUARTZ_READ_NATIVE(std::int64_t); break;
        case ProcessValueType::Float: QUARTZ_READ_NATIVE(float); break;
        case ProcessValueType::Double: { double value{}; if (!readProcessMemoryValue(pid, *address, value, error)) { if (binding.AddressMode == ProcessAddressMode::Signature && binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture) { binding.SignatureResolvedAddress = 0; binding.NextRegisterCapture = 0.0; binding.SignatureRegisterCapture.reset(); } binding.Error = std::move(error); return false; } output = static_cast<float>(value); break; }
        case ProcessValueType::Bool: { std::uint8_t value{}; if (!readProcessMemoryValue(pid, *address, value, error)) { if (binding.AddressMode == ProcessAddressMode::Signature && binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture) { binding.SignatureResolvedAddress = 0; binding.NextRegisterCapture = 0.0; binding.SignatureRegisterCapture.reset(); } binding.Error = std::move(error); return false; } output = value != 0 ? 1.0f : 0.0f; break; }
        }
#undef QUARTZ_READ_NATIVE
        binding.Error.clear();
        return true;
    }
