#include "quartz/client/Model.hpp"
#include "quartz/client/native/SignatureScanner.hpp"

namespace quartz::client
{
    std::string runtimeLower(std::string_view value)
    {
        std::string result(value);
        std::ranges::transform(result, result.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }

    std::string runtimeRegexEscape(const std::string_view value)
    {
        static constexpr std::string_view Special = R"(\.^$|()[]*+?{})";
        std::string result;
        result.reserve(value.size() + 8);
        for (const char c : value)
        {
            if (Special.find(c) != std::string_view::npos) result.push_back('\\');
            result.push_back(c);
        }
        return result;
    }

    bool runtimeRebindModeIsRegex(const ProcessRebindMode mode) noexcept
    {
        return mode >= ProcessRebindMode::NameRegex;
    }

    const char* runtimeRebindModeName(const ProcessRebindMode mode) noexcept
    {
        switch (mode)
        {
        case ProcessRebindMode::NameExact: return "Process name (exact)";
        case ProcessRebindMode::ExecutableExact: return "Executable path (exact)";
        case ProcessRebindMode::TitleExact: return "Process title / argv[0] (exact)";
        case ProcessRebindMode::CommandLineExact: return "Command line (exact)";
        case ProcessRebindMode::NameRegex: return "Process name (regex)";
        case ProcessRebindMode::ExecutableRegex: return "Executable path (regex)";
        case ProcessRebindMode::TitleRegex: return "Process title / argv[0] (regex)";
        case ProcessRebindMode::CommandLineRegex: return "Command line (regex)";
        case ProcessRebindMode::AnyRegex: return "Any field (regex)";
        }
        return "Process name (exact)";
    }

    std::vector<RuntimeProcessInfo> enumerateRuntimeProcesses()
    {
        std::vector<RuntimeProcessInfo> processes;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator("/proc", error))
        {
            if (error || !entry.is_directory(error)) continue;
            const std::string name = entry.path().filename().string();
            if (name.empty() || !std::ranges::all_of(name, [](const unsigned char c) { return std::isdigit(c) != 0; })) continue;
            int pid = 0;
            const auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), pid);
            if (ec != std::errc{} || ptr != name.data() + name.size() || pid <= 0) continue;

            RuntimeProcessInfo process;
            process.Pid = static_cast<pid_t>(pid);
            std::ifstream comm(entry.path() / "comm");
            std::getline(comm, process.Name);
            std::array<char, 4096> exe{};
            const ssize_t count = ::readlink((entry.path() / "exe").c_str(), exe.data(), exe.size() - 1);
            if (count > 0) process.Exe.assign(exe.data(), static_cast<std::size_t>(count));

            std::ifstream cmdline(entry.path() / "cmdline", std::ios::binary);
            if (cmdline)
            {
                std::string raw((std::istreambuf_iterator<char>(cmdline)), std::istreambuf_iterator<char>());
                if (!raw.empty())
                {
                    const std::size_t firstEnd = raw.find('\0');
                    process.Title = raw.substr(0, firstEnd == std::string::npos ? raw.size() : firstEnd);
                    for (char& c : raw) if (c == '\0') c = ' ';
                    process.CommandLine = trim(std::move(raw));
                }
            }
            if (process.Title.empty()) process.Title = process.Name;
            if (process.CommandLine.empty()) process.CommandLine = !process.Exe.empty() ? process.Exe : process.Name;
            if (process.Name.empty() && process.Exe.empty() && process.Title.empty()) continue;
            process.SearchText = runtimeLower(std::to_string(process.Pid) + "\n" + process.Name + "\n" + process.Exe + "\n" + process.Title + "\n" + process.CommandLine);
            processes.emplace_back(std::move(process));
        }
        std::ranges::sort(processes, [](const auto& a, const auto& b)
        {
            if (a.Name != b.Name) return a.Name < b.Name;
            return a.Pid < b.Pid;
        });
        return processes;
    }

    std::string runtimeProcessDisplayTitle(const RuntimeProcessInfo& process)
    {
        std::string title = process.Name.empty() ? "<unnamed>" : process.Name;
        if (!process.Title.empty() && process.Title != process.Name && process.Title != process.Exe)
        {
            std::string decorative = process.Title;
            if (decorative.size() > 72) decorative.resize(69), decorative += "...";
            title += "  -  " + decorative;
        }
        return title;
    }

    bool runtimeProcessMatchesSearch(const RuntimeProcessInfo& process, const std::string_view loweredQuery)
    {
        return loweredQuery.empty() || process.SearchText.find(loweredQuery) != std::string::npos;
    }

    std::string runtimeProcessRebindValue(const RuntimeProcessInfo& process, const ProcessRebindMode mode)
    {
        switch (mode)
        {
        case ProcessRebindMode::NameExact:
        case ProcessRebindMode::NameRegex: return process.Name;
        case ProcessRebindMode::ExecutableExact:
        case ProcessRebindMode::ExecutableRegex: return process.Exe;
        case ProcessRebindMode::TitleExact:
        case ProcessRebindMode::TitleRegex: return process.Title;
        case ProcessRebindMode::CommandLineExact:
        case ProcessRebindMode::CommandLineRegex: return process.CommandLine;
        case ProcessRebindMode::AnyRegex: return process.Name;
        }
        return process.Name;
    }

    void captureRuntimeRebindPattern(RuntimeBinding& binding, const RuntimeProcessInfo& process)
    {
        std::string value = runtimeProcessRebindValue(process, binding.RebindMode);
        if (value.empty()) value = process.Name;
        if (runtimeRebindModeIsRegex(binding.RebindMode)) value = "^" + runtimeRegexEscape(value) + "$";
        std::snprintf(binding.ProcessRebindPattern, sizeof(binding.ProcessRebindPattern), "%s", value.c_str());
    }

    bool runtimeProcessMatchesRebind(const RuntimeProcessInfo& process, const RuntimeBinding& binding, const std::regex* regex)
    {
        if (runtimeRebindModeIsRegex(binding.RebindMode))
        {
            if (!regex) return false;
            const auto matches = [&](const std::string& value) { return !value.empty() && std::regex_search(value, *regex); };
            switch (binding.RebindMode)
            {
            case ProcessRebindMode::NameRegex: return matches(process.Name);
            case ProcessRebindMode::ExecutableRegex: return matches(process.Exe);
            case ProcessRebindMode::TitleRegex: return matches(process.Title);
            case ProcessRebindMode::CommandLineRegex: return matches(process.CommandLine);
            case ProcessRebindMode::AnyRegex: return matches(process.Name) || matches(process.Exe) || matches(process.Title) || matches(process.CommandLine);
            default: return false;
            }
        }
        return runtimeProcessRebindValue(process, binding.RebindMode) == binding.ProcessRebindPattern;
    }

    std::optional<RuntimeProcessInfo> findRuntimeRebindProcess(const RuntimeBinding& binding, std::string& error)
    {
        if (!binding.AutoReattach) { error = "automatic process rebind is disabled"; return std::nullopt; }
        if (binding.ProcessRebindPattern[0] == '\0') { error = "rebind pattern is empty"; return std::nullopt; }
        std::optional<std::regex> regex;
        if (runtimeRebindModeIsRegex(binding.RebindMode))
        {
            try { regex.emplace(binding.ProcessRebindPattern, std::regex::ECMAScript | std::regex::icase); }
            catch (const std::regex_error& e) { error = std::string("invalid rebind regex: ") + e.what(); return std::nullopt; }
        }
        std::optional<RuntimeProcessInfo> best;
        for (const auto& process : enumerateRuntimeProcesses())
            if (runtimeProcessMatchesRebind(process, binding, regex ? &*regex : nullptr) && (!best || process.Pid > best->Pid)) best = process;
        if (!best) error = "waiting for process match: " + std::string(binding.ProcessRebindPattern);
        return best;
    }

    bool runtimeProcessIsAlive(const pid_t pid) noexcept
    {
        if (pid <= 0) return false;
        errno = 0;
        return ::kill(pid, 0) == 0 || errno != ESRCH;
    }

    double runtimeSteadySeconds() noexcept
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool tryRuntimeProcessRebind(RuntimeBinding& binding, pid_t& pid, std::string& error)
    {
        if (!binding.AutoReattach) { error = "process is not running"; return false; }
        const double now = runtimeSteadySeconds();
        if (now < binding.NextProcessSearch) { error = "waiting for process match: " + std::string(binding.ProcessRebindPattern); return false; }
        binding.NextProcessSearch = now + 1.0;
        const auto process = findRuntimeRebindProcess(binding, error);
        if (!process) return false;
        pid = process->Pid;
        binding.ProcessId = static_cast<int>(pid);
        std::snprintf(binding.ProcessName, sizeof(binding.ProcessName), "%s", process->Name.c_str());
        binding.NextProcessSearch = 0.0;
        error.clear();
        return true;
    }

    std::vector<RuntimeProcessModule> enumerateRuntimeModules(const pid_t pid)
    {
        std::vector<RuntimeProcessModule> modules;
        std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
        if (!maps) return modules;
        std::unordered_map<std::string, std::size_t> indices;
        std::string line;
        while (std::getline(maps, line))
        {
            std::istringstream stream(line);
            std::string range, permissions, offsetText, device, inode;
            if (!(stream >> range >> permissions >> offsetText >> device >> inode)) continue;
            std::string path;
            std::getline(stream, path);
            path = trim(std::move(path));
            if (path.empty() || path.front() == '[') continue;
            const std::size_t dash = range.find('-');
            if (dash == std::string::npos) continue;
            std::uintptr_t start = 0, end = 0, offset = 0;
            auto parseHex = [](const std::string_view value, std::uintptr_t& result)
            {
                const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result, 16);
                return ec == std::errc{} && ptr == value.data() + value.size();
            };
            if (!parseHex(std::string_view(range).substr(0, dash), start) || !parseHex(std::string_view(range).substr(dash + 1), end) || !parseHex(offsetText, offset)) continue;
            const std::uintptr_t base = start >= offset ? start - offset : start;
            const auto existing = indices.find(path);
            if (existing == indices.end())
            {
                RuntimeProcessModule module;
                module.Base = base;
                module.End = end;
                module.Path = path;
                module.Name = std::filesystem::path(path).filename().string();
                indices[path] = modules.size();
                modules.emplace_back(std::move(module));
            }
            else
            {
                auto& module = modules[existing->second];
                module.Base = std::min(module.Base, base);
                module.End = std::max(module.End, end);
            }
        }
        std::ranges::sort(modules, std::ranges::less{}, &RuntimeProcessModule::Base);
        return modules;
    }

    std::vector<RuntimeProcessRegion> enumerateRuntimeRegions(const pid_t pid)
    {
        std::vector<RuntimeProcessRegion> regions;
        std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
        if (!maps) return regions;
        std::string line;
        while (std::getline(maps, line))
        {
            std::istringstream stream(line);
            std::string range, permissions, offset, device, inode;
            if (!(stream >> range >> permissions >> offset >> device >> inode)) continue;
            const std::size_t dash = range.find('-');
            if (dash == std::string::npos) continue;
            std::uintptr_t base = 0, end = 0;
            const auto [basePtr, baseEc] = std::from_chars(range.data(), range.data() + dash, base, 16);
            const auto [endPtr, endEc] = std::from_chars(range.data() + dash + 1, range.data() + range.size(), end, 16);
            if (baseEc != std::errc{} || endEc != std::errc{} || base >= end) continue;
            std::string path;
            std::getline(stream, path);
            RuntimeProcessRegion region;
            region.Base = base;
            region.End = end;
            region.Readable = !permissions.empty() && permissions[0] == 'r';
            region.Writable = permissions.size() > 1 && permissions[1] == 'w';
            region.Executable = permissions.size() > 2 && permissions[2] == 'x';
            region.Path = trim(std::move(path));
            regions.emplace_back(std::move(region));
        }
        return regions;
    }

    int runtimeHexNibble(const char c) noexcept
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool parseRuntimeHexPattern(const std::string_view signature, std::vector<std::uint8_t>& bytes, std::vector<std::uint8_t>& masks, std::string& error)
    {
        bytes.clear();
        masks.clear();
        std::istringstream stream{std::string(signature)};
        std::string token;
        while (stream >> token)
        {
            if (token == "?" || token == "??")
            {
                bytes.push_back(0);
                masks.push_back(0);
                continue;
            }
            if (token.size() != 2)
            {
                error = "hexadecimal pattern token must be two hex nibbles or ?: " + token;
                return false;
            }
            const bool highWildcard = token[0] == '?';
            const bool lowWildcard = token[1] == '?';
            const int high = highWildcard ? 0 : runtimeHexNibble(token[0]);
            const int low = lowWildcard ? 0 : runtimeHexNibble(token[1]);
            if (high < 0 || low < 0)
            {
                error = "invalid hexadecimal pattern byte: " + token;
                return false;
            }
            bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
            masks.push_back(static_cast<std::uint8_t>((highWildcard ? 0 : 0xF0) | (lowWildcard ? 0 : 0x0F)));
        }
        if (bytes.empty())
        {
            error = "hexadecimal pattern is empty";
            return false;
        }
        if (std::ranges::all_of(masks, [](const std::uint8_t mask) { return mask == 0; }))
        {
            error = "hexadecimal pattern cannot contain only wildcards";
            return false;
        }
        error.clear();
        return true;
    }

    std::string runtimeNormalizeOpcodeText(std::string value)
    {
        value = trim(std::move(value));
        std::string result; result.reserve(value.size());
        bool space = false;
        for (const unsigned char c : value)
        {
            if (std::isspace(c)) { space = !result.empty(); continue; }
            if (space) { result.push_back(' '); space = false; }
            result.push_back(static_cast<char>(std::tolower(c)));
        }
        return result;
    }

    bool runtimeWildcardTextMatch(const std::string_view pattern, const std::string_view value) noexcept
    {
        std::size_t p = 0, v = 0, star = std::string_view::npos, retry = 0;
        while (v < value.size())
        {
            if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) { ++p; ++v; continue; }
            if (p < pattern.size() && pattern[p] == '*') { star = p++; retry = v; continue; }
            if (star != std::string_view::npos) { p = star + 1; v = ++retry; continue; }
            return false;
        }
        while (p < pattern.size() && pattern[p] == '*') ++p;
        return p == pattern.size();
    }

    std::vector<std::string> parseRuntimeOpcodePattern(const std::string_view specification)
    {
        std::vector<std::string> lines; std::istringstream stream{std::string(specification)}; std::string line;
        while (std::getline(stream, line))
        {
            line = runtimeNormalizeOpcodeText(line);
            if (line.empty() || line.starts_with('#') || line.starts_with("//")) continue;
            lines.emplace_back(std::move(line));
        }
        return lines;
    }

#if QUARTZ_HAS_ZYDIS
    bool runtimeDecodeInstructionText(const std::span<const std::uint8_t> bytes, const std::uintptr_t address, std::string& text, std::size_t& length)
    {
        ZydisDisassembledInstruction instruction{};
        if (!ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64, address, bytes.data(), bytes.size(), &instruction))) return false;
        text = runtimeNormalizeOpcodeText(instruction.text);
        length = instruction.info.length;
        return length != 0;
    }
#else
    bool runtimeDecodeInstructionText(const std::span<const std::uint8_t>, const std::uintptr_t, std::string&, std::size_t&) { return false; }
#endif

    bool runtimeOpcodePatternMatches(const std::span<const std::uint8_t> bytes, const std::uintptr_t address, const std::vector<std::string>& patterns, std::size_t& matchedLength)
    {
        matchedLength = 0;
        if (patterns.empty()) return false;
        std::size_t offset = 0;
        for (const auto& pattern : patterns)
        {
            if (offset >= bytes.size()) return false;
            std::string text; std::size_t instructionLength = 0;
            if (!runtimeDecodeInstructionText(bytes.subspan(offset), address + offset, text, instructionLength)) return false;
            if (!runtimeWildcardTextMatch(pattern, text)) return false;
            offset += instructionLength;
        }
        matchedLength = offset;
        return true;
    }

    bool readProcessMemoryBlock(const pid_t pid, const std::uintptr_t address, std::span<std::uint8_t> buffer, std::string& error)
    {
        if (buffer.empty()) return true;
        iovec local{buffer.data(), buffer.size()};
        iovec remote{reinterpret_cast<void*>(address), buffer.size()};
        errno = 0;
        const ssize_t count = ::process_vm_readv(pid, &local, 1, &remote, 1, 0);
        if (count == static_cast<ssize_t>(buffer.size())) return true;
        error = count < 0 ? std::string(std::strerror(errno)) : "short read (" + std::to_string(count) + "/" + std::to_string(buffer.size()) + ")";
        return false;
    }

    const char* runtimeX64RegisterName(const RuntimeX64Register reg) noexcept
    {
        static constexpr const char* Names[] = {"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};
        return Names[std::clamp(static_cast<int>(reg), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    std::uint64_t runtimeX64RegisterValue(const user_regs_struct& regs, const RuntimeX64Register reg) noexcept
    {
        switch (reg)
        {
        case RuntimeX64Register::Rax: return regs.rax;
        case RuntimeX64Register::Rbx: return regs.rbx;
        case RuntimeX64Register::Rcx: return regs.rcx;
        case RuntimeX64Register::Rdx: return regs.rdx;
        case RuntimeX64Register::Rsi: return regs.rsi;
        case RuntimeX64Register::Rdi: return regs.rdi;
        case RuntimeX64Register::Rbp: return regs.rbp;
        case RuntimeX64Register::Rsp: return regs.rsp;
        case RuntimeX64Register::R8: return regs.r8;
        case RuntimeX64Register::R9: return regs.r9;
        case RuntimeX64Register::R10: return regs.r10;
        case RuntimeX64Register::R11: return regs.r11;
        case RuntimeX64Register::R12: return regs.r12;
        case RuntimeX64Register::R13: return regs.r13;
        case RuntimeX64Register::R14: return regs.r14;
        case RuntimeX64Register::R15: return regs.r15;
        }
        return 0;
    }
    std::vector<pid_t> enumerateRuntimeThreads(const pid_t pid)
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

    bool runtimePtracePeekUser(const pid_t tid, const std::size_t offset, std::uint64_t& value) noexcept
    {
        errno = 0;
        const long result = ::ptrace(PTRACE_PEEKUSER, tid, reinterpret_cast<void*>(offset), nullptr);
        if (result == -1 && errno != 0) return false;
        value = static_cast<std::uint64_t>(static_cast<unsigned long>(result));
        return true;
    }

    bool runtimePtracePokeUser(const pid_t tid, const std::size_t offset, const std::uint64_t value) noexcept
    {
        errno = 0;
        return ::ptrace(PTRACE_POKEUSER, tid, reinterpret_cast<void*>(offset), reinterpret_cast<void*>(static_cast<std::uintptr_t>(value))) == 0;
    }

    bool runtimeWaitForPtraceStop(const pid_t tid, std::stop_token stop, const double timeoutSeconds, int& status) noexcept
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

    void startRuntimeRegisterCapture(RuntimeBinding& binding, const pid_t pid, const std::uintptr_t instruction, const std::intptr_t displacement)
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

    bool readRuntimeRegisterDisplacement(const RuntimeBinding& binding, const pid_t pid, const std::uintptr_t instruction, std::intptr_t& displacement, std::string& error)
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

    std::optional<std::uintptr_t> advanceRuntimeRegisterCapture(RuntimeBinding& binding, const pid_t pid, std::string& error)
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

    std::uint64_t runtimeSignatureConfigurationHash(const RuntimeBinding& binding)
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

    void resetRuntimeSignatureScan(RuntimeBinding& binding, const bool clearResolved)
    {
        cancelSignatureScan(binding.SignatureScan);
        binding.SignatureScan.reset();
        ++binding.SignatureScanGeneration;
        binding.SignatureScanRunning = false;
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

    bool prepareRuntimeSignatureScan(RuntimeBinding& binding, const pid_t pid, std::string& error)
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

    bool runtimeSignatureMatches(const std::span<const std::uint8_t> data, const std::size_t offset, const RuntimeBinding& binding) noexcept
    {
        for (std::size_t i = 0; i < binding.SignatureBytes.size(); ++i)
            if ((data[offset + i] & binding.SignatureMasks[i]) != (binding.SignatureBytes[i] & binding.SignatureMasks[i])) return false;
        return true;
    }

    bool parseRuntimeInteger(const std::string_view text, std::intptr_t& value)
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

    std::optional<std::uintptr_t> resolveRuntimeAddress(const RuntimeBinding& binding, const pid_t pid, std::string& error, const std::optional<std::uintptr_t> baseOverride)
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

    std::optional<std::uintptr_t> resolveRuntimeSignatureMatch(RuntimeBinding& binding, const pid_t pid, const std::uintptr_t match, std::string& error)
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


    bool readNativeBinding(RuntimeBinding& binding, float& output)
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

}
