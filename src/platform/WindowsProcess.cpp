#ifdef _WIN32
#include "quartz/client/Model.hpp"
#include <tlhelp32.h>
#include <psapi.h>
#include <winternl.h>

namespace quartz::client
{
    namespace
    {
        win::Handle snapshot(DWORD flags, DWORD pid = 0)
        {
            for (int retry = 0; retry < 8; ++retry)
            {
                win::Handle result(CreateToolhelp32Snapshot(flags, pid));
                if (result || GetLastError() != ERROR_BAD_LENGTH) return result;
            }
            return win::Handle{};
        }

        std::string commandLine(HANDLE process)
        {
            using Query = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, void*, ULONG, ULONG*);
            static auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
            if (!query) return {};
            // ProcessCommandLineInformation is available on all supported Windows versions.
            ULONG size = 0;
            query(process, static_cast<PROCESSINFOCLASS>(60), nullptr, 0, &size);
            if (size < sizeof(UNICODE_STRING) || size > 1024 * 1024) return {};
            std::vector<std::uint8_t> buffer(size);
            if (query(process, static_cast<PROCESSINFOCLASS>(60), buffer.data(), size, &size) < 0) return {};
            const auto& value = *reinterpret_cast<const UNICODE_STRING*>(buffer.data());
            const auto begin = reinterpret_cast<std::uintptr_t>(buffer.data());
            const auto text = reinterpret_cast<std::uintptr_t>(value.Buffer);
            if (text < begin || text > begin + buffer.size() || value.Length > begin + buffer.size() - text) return {};
            return win::utf8({value.Buffer, value.Length / sizeof(wchar_t)});
        }
    }

    std::vector<RuntimeProcessInfo> enumerateRuntimeProcesses()
    {
        std::unordered_map<DWORD, std::string> titles;
        EnumWindows([](HWND window, LPARAM context) -> BOOL
        {
            if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
            wchar_t title[1024]{};
            if (GetWindowTextW(window, title, 1024) <= 0) return TRUE;
            DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
            reinterpret_cast<std::unordered_map<DWORD, std::string>*>(context)->try_emplace(pid, win::utf8(title));
            return TRUE;
        }, reinterpret_cast<LPARAM>(&titles));
        std::vector<RuntimeProcessInfo> result;
        auto processes = snapshot(TH32CS_SNAPPROCESS);
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (!processes || !Process32FirstW(processes.get(), &entry)) return result;
        do
        {
            if (!entry.th32ProcessID) continue;
            RuntimeProcessInfo info; info.Pid = static_cast<pid_t>(entry.th32ProcessID);
            info.Name = win::utf8(entry.szExeFile);
            win::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
            if (process)
            {
                std::wstring path(32768, L'\0'); DWORD length = static_cast<DWORD>(path.size());
                if (QueryFullProcessImageNameW(process.get(), 0, path.data(), &length)) info.Exe = win::utf8(std::wstring_view(path).substr(0, length));
                info.CommandLine = commandLine(process.get());
            }
            if (const auto title = titles.find(entry.th32ProcessID); title != titles.end()) info.Title = title->second;
            if (info.Title.empty()) info.Title = info.Name;
            if (info.CommandLine.empty()) info.CommandLine = info.Exe;
            info.SearchText = runtimeLower(std::to_string(info.Pid) + "\n" + info.Name + "\n" + info.Exe + "\n" + info.Title + "\n" + info.CommandLine);
            result.emplace_back(std::move(info));
        } while (Process32NextW(processes.get(), &entry));
        std::ranges::sort(result, [](const auto& a, const auto& b) { return a.Name == b.Name ? a.Pid < b.Pid : a.Name < b.Name; });
        return result;
    }

    bool runtimeProcessIsAlive(pid_t pid) noexcept
    {
        if (pid <= 0) return false;
        win::Handle process(OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid)));
        if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
        return WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;
    }

    std::vector<RuntimeProcessModule> enumerateRuntimeModules(pid_t pid)
    {
        std::vector<RuntimeProcessModule> result;
        auto modules = snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, static_cast<DWORD>(pid));
        MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (!modules || !Module32FirstW(modules.get(), &entry)) return result;
        do
        {
            RuntimeProcessModule module;
            module.Base = module.MappingBase = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
            module.End = module.Base + entry.modBaseSize;
            module.Name = win::utf8(entry.szModule); module.Path = win::utf8(entry.szExePath);
            result.emplace_back(std::move(module));
        } while (Module32NextW(modules.get(), &entry));
        std::ranges::sort(result, {}, &RuntimeProcessModule::Base);
        return result;
    }

    std::vector<RuntimeProcessRegion> enumerateRuntimeRegions(pid_t pid)
    {
        std::vector<RuntimeProcessRegion> result;
        win::Handle process(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid)));
        if (!process) return result;
        const auto modules = enumerateRuntimeModules(pid);
        MEMORY_BASIC_INFORMATION info{};
        for (std::uintptr_t address = 0; VirtualQueryEx(process.get(), reinterpret_cast<void*>(address), &info, sizeof(info));)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
            if (info.RegionSize > std::numeric_limits<std::uintptr_t>::max() - base || base + info.RegionSize <= address) break;
            address = base + info.RegionSize;
            if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) continue;
            const DWORD protection = info.Protect & 0xFF;
            RuntimeProcessRegion region;
            region.Base = base; region.End = address;
            region.Readable = protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
            region.Writable = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
            region.Executable = protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
            for (const auto& module : modules) if (module.contains(base)) { region.Path = module.Path; break; }
            result.emplace_back(std::move(region));
        }
        return result;
    }

    std::vector<pid_t> enumerateRuntimeThreads(pid_t pid)
    {
        std::vector<pid_t> result;
        auto threads = snapshot(TH32CS_SNAPTHREAD);
        THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
        if (!threads || !Thread32First(threads.get(), &entry)) return result;
        do { if (entry.th32OwnerProcessID == static_cast<DWORD>(pid)) result.push_back(static_cast<pid_t>(entry.th32ThreadID)); }
        while (Thread32Next(threads.get(), &entry));
        return result;
    }

    std::size_t readProcessMemoryPartial(pid_t pid, std::uintptr_t address, std::span<std::uint8_t> buffer, std::string& error)
    {
        error.clear();
        if (buffer.empty()) return 0;
        win::Handle process(OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid)));
        if (!process) { error = win::error("OpenProcess for reading"); return 0; }
        SIZE_T total = 0;
        while (total < buffer.size())
        {
            if (total > std::numeric_limits<std::uintptr_t>::max() - address) break;
            MEMORY_BASIC_INFORMATION region{};
            const auto current = address + total;
            if (!VirtualQueryEx(process.get(), reinterpret_cast<void*>(current), &region, sizeof(region)) || region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) break;
            const auto remaining = region.RegionSize - (current - reinterpret_cast<std::uintptr_t>(region.BaseAddress));
            const auto count = std::min<std::size_t>(buffer.size() - total, remaining);
            SIZE_T read = 0;
            const BOOL success = ReadProcessMemory(process.get(), reinterpret_cast<void*>(current), buffer.data() + total, count, &read);
            total += read;
            if (!success || !read) { error = win::error("ReadProcessMemory"); break; }
        }
        if (!total && error.empty()) error = "address is not readable";
        return total;
    }

    bool runtimeWriteProcessMemory(pid_t pid, std::uintptr_t address, std::span<const std::uint8_t> bytes, std::string& error)
    {
        error.clear();
        if (bytes.empty() || address > std::numeric_limits<std::uintptr_t>::max() - bytes.size()) { error = "invalid write range"; return false; }
        win::Handle process(OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid)));
        if (!process) { error = win::error("OpenProcess for writing"); return false; }
        struct Protection { void* Address; SIZE_T Size; DWORD Original; };
        std::vector<Protection> changes;
        const auto restore = [&]
        {
            bool success = true;
            for (auto it = changes.rbegin(); it != changes.rend(); ++it)
            {
                DWORD ignored;
                if (!VirtualProtectEx(process.get(), it->Address, it->Size, it->Original, &ignored))
                { error += "; " + win::error("restoring memory protection"); success = false; }
            }
            return success;
        };
        for (auto current = address; current < address + bytes.size();)
        {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQueryEx(process.get(), reinterpret_cast<void*>(current), &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            { error = "write range includes uncommitted, guarded, or inaccessible memory"; restore(); return false; }
            const auto count = std::min<std::size_t>(address + bytes.size() - current, info.RegionSize - (current - reinterpret_cast<std::uintptr_t>(info.BaseAddress)));
            const DWORD protection = info.Protect & 0xFF;
            if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
            {
                DWORD old;
                const DWORD writable = (protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
                if (!VirtualProtectEx(process.get(), reinterpret_cast<void*>(current), count, writable, &old))
                { error = win::error("VirtualProtectEx"); restore(); return false; }
                changes.push_back({reinterpret_cast<void*>(current), count, old});
            }
            current += count;
        }
        SIZE_T written = 0;
        const bool success = WriteProcessMemory(process.get(), reinterpret_cast<void*>(address), bytes.data(), bytes.size(), &written) && written == bytes.size();
        if (!success) error = win::error("WriteProcessMemory");
        const bool flushed = FlushInstructionCache(process.get(), reinterpret_cast<void*>(address), bytes.size()) != FALSE;
        if (!flushed) error += "; " + win::error("FlushInstructionCache");
        return restore() && success && flushed;
    }
}
#endif
