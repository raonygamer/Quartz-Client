#include "quartz/client/native/NativeDisassembly.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/Model.hpp"
#include <elf.h>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace quartz::client
{
    namespace
    {
        struct ProcessDisassemblyMetadata
        {
            RuntimeX86Mode Mode = RuntimeX86Mode::X64;
            double ModeUpdated = 0.0;
            std::shared_ptr<const std::vector<RuntimeProcessModule>> Modules;
            double ModulesUpdated = 0.0;
        };

        struct LocalDisassemblyMetadata
        {
            pid_t ModePid = 0;
            RuntimeX86Mode Mode = RuntimeX86Mode::X64;
            double ModeUpdated = 0.0;
            pid_t ModulesPid = 0;
            std::shared_ptr<const std::vector<RuntimeProcessModule>> Modules;
            double ModulesUpdated = 0.0;
        };

        constexpr double ModeCacheSeconds = 2.0;
        constexpr double ModuleCacheSeconds = 0.75;
        std::mutex MetadataMutex;
        std::unordered_map<pid_t, ProcessDisassemblyMetadata> MetadataCache;
        thread_local LocalDisassemblyMetadata LocalMetadata;

        double metadataNow() noexcept { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        std::vector<RuntimeProcessModule> enumerateMappedProcessModules(const pid_t pid)
        {
            std::vector<RuntimeProcessModule> modules;
            std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
            if (!maps) return modules;
            std::string line;
            while (std::getline(maps,line))
            {
                std::istringstream stream(line); std::string range,permissions,offsetText,device,inode;
                if (!(stream>>range>>permissions>>offsetText>>device>>inode)) continue;
                std::string path; std::getline(stream,path); path=trim(std::move(path));
                if (path.empty()||path.front()=='[') continue;
                const std::size_t dash=range.find('-'); if (dash==std::string::npos) continue;
                const auto parseHex=[](const std::string_view text,std::uintptr_t& value)
                {
                    const auto [ptr,ec]=std::from_chars(text.data(),text.data()+text.size(),value,16); return ec==std::errc{}&&ptr==text.data()+text.size();
                };
                std::uintptr_t start=0,end=0,offset=0;
                if (!parseHex(std::string_view(range).substr(0,dash),start)||!parseHex(std::string_view(range).substr(dash+1),end)||!parseHex(offsetText,offset)||start>=end) continue;
                RuntimeProcessModule module; module.Base=start>=offset?start-offset:start; module.MappingBase=start; module.End=end; module.Path=path; module.Name=std::filesystem::path(path).filename().string(); modules.emplace_back(std::move(module));
            }
            std::ranges::sort(modules,[](const RuntimeProcessModule& a,const RuntimeProcessModule& b)
            {
                if (a.MappingBase!=b.MappingBase) return a.MappingBase<b.MappingBase;
                if (a.Base!=b.Base) return a.Base<b.Base;
                return a.Path<b.Path;
            });
            return modules;
        }

        std::shared_ptr<const std::vector<RuntimeProcessModule>> cachedProcessModules(const pid_t pid, const double now)
        {
            if (LocalMetadata.ModulesPid == pid && LocalMetadata.Modules && now - LocalMetadata.ModulesUpdated < ModuleCacheSeconds) return LocalMetadata.Modules;
            {
                std::lock_guard lock(MetadataMutex);
                if (const auto it = MetadataCache.find(pid); it != MetadataCache.end() && it->second.Modules && now - it->second.ModulesUpdated < ModuleCacheSeconds)
                {
                    LocalMetadata.ModulesPid = pid; LocalMetadata.Modules = it->second.Modules; LocalMetadata.ModulesUpdated = it->second.ModulesUpdated; return LocalMetadata.Modules;
                }
            }
            auto modules = std::make_shared<const std::vector<RuntimeProcessModule>>(enumerateMappedProcessModules(pid));
            {
                std::lock_guard lock(MetadataMutex); auto& cached = MetadataCache[pid]; cached.Modules = modules; cached.ModulesUpdated = now;
            }
            LocalMetadata.ModulesPid = pid; LocalMetadata.Modules = std::move(modules); LocalMetadata.ModulesUpdated = now; return LocalMetadata.Modules;
        }

        bool runAssemblerCommand(const std::string& command, std::string& output)
        {
            output.clear(); FILE* pipe = popen((command + " 2>&1").c_str(), "r"); if (!pipe) { output = "could not start assembler command"; return false; }
            std::array<char, 1024> buffer{}; while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) && output.size() < 16 * 1024) output += buffer.data();
            const int status = pclose(pipe); return status == 0;
        }

        void removeAssemblerFiles(const std::filesystem::path& base) noexcept
        {
            std::error_code ec; std::filesystem::remove(base.string() + ".s", ec); std::filesystem::remove(base.string() + ".o", ec); std::filesystem::remove(base.string() + ".elf", ec); std::filesystem::remove(base.string() + ".bin", ec);
        }
    }

    RuntimeX86Mode runtimeProcessX86Mode(const pid_t pid) noexcept
    {
        if (pid <= 0) return RuntimeX86Mode::X64;
        const double now = metadataNow(); if (LocalMetadata.ModePid == pid && now - LocalMetadata.ModeUpdated < ModeCacheSeconds) return LocalMetadata.Mode;
        {
            std::lock_guard lock(MetadataMutex);
            if (const auto it = MetadataCache.find(pid); it != MetadataCache.end() && now - it->second.ModeUpdated < ModeCacheSeconds) { LocalMetadata.ModePid = pid; LocalMetadata.Mode = it->second.Mode; LocalMetadata.ModeUpdated = it->second.ModeUpdated; return LocalMetadata.Mode; }
        }
        RuntimeX86Mode mode = RuntimeX86Mode::X64;
        std::ifstream file("/proc/" + std::to_string(pid) + "/exe", std::ios::binary);
        std::array<unsigned char, EI_NIDENT> ident{};
        if (file.read(reinterpret_cast<char*>(ident.data()), static_cast<std::streamsize>(ident.size())) && ident[EI_MAG0] == ELFMAG0 && ident[EI_MAG1] == ELFMAG1 && ident[EI_MAG2] == ELFMAG2 && ident[EI_MAG3] == ELFMAG3) mode = ident[EI_CLASS] == ELFCLASS32 ? RuntimeX86Mode::X86 : RuntimeX86Mode::X64;
        { std::lock_guard lock(MetadataMutex); auto& cached = MetadataCache[pid]; cached.Mode = mode; cached.ModeUpdated = now; }
        LocalMetadata.ModePid = pid; LocalMetadata.Mode = mode; LocalMetadata.ModeUpdated = now; return mode;
    }

    const char* runtimeX86ModeName(const RuntimeX86Mode mode) noexcept { return mode == RuntimeX86Mode::X86 ? "x86" : "x86-64"; }

    std::vector<RuntimeProcessModule> runtimeProcessModules(const pid_t pid)
    {
        if (pid<=0) return {}; const auto modules=cachedProcessModules(pid,metadataNow()); return modules?*modules:std::vector<RuntimeProcessModule>{};
    }

    std::string runtimeFormatAbsoluteAddress(const RuntimeX86Mode mode, const std::uintptr_t address)
    {
        const unsigned width=mode==RuntimeX86Mode::X86?8U:16U; const std::uint64_t value=mode==RuntimeX86Mode::X86?static_cast<std::uint32_t>(address):static_cast<std::uint64_t>(address); std::ostringstream out; out << "0x" << std::hex << std::uppercase << std::setw(static_cast<int>(width)) << std::setfill('0') << value; return out.str();
    }

    std::string runtimeFormatProcessAddress(const RuntimeX86Mode mode, const std::span<const RuntimeProcessModule> modules, const std::uintptr_t address)
    {
        for (const auto& module : modules)
        {
            if (!module.contains(address)) continue;
            std::string name = module.Name;
            if (name.empty() && !module.Path.empty()) name = std::filesystem::path(module.Path).filename().string();
            if (name.empty()) break;
            const std::uintptr_t base=address>=module.Base?module.Base:module.MappingBase;
            std::ostringstream out; out << name << "+0x" << std::hex << std::uppercase << static_cast<unsigned long long>(address - base); return out.str();
        }
        return runtimeFormatAbsoluteAddress(mode,address);
    }

    std::string runtimeFormatProcessAddress(const std::span<const RuntimeProcessModule> modules, const std::uintptr_t address)
    {
        for (const auto& module : modules)
        {
            if (!module.contains(address)) continue;
            std::string name = module.Name;
            if (name.empty() && !module.Path.empty()) name = std::filesystem::path(module.Path).filename().string();
            if (name.empty()) break;
            const std::uintptr_t base=address>=module.Base?module.Base:module.MappingBase;
            std::ostringstream out; out << name << "+0x" << std::hex << std::uppercase << static_cast<unsigned long long>(address - base); return out.str();
        }
        return runtimeHexAddress(address);
    }

    std::string runtimeFormatProcessAddress(const pid_t pid, const std::uintptr_t address)
    {
        if (pid <= 0) return runtimeHexAddress(address); const RuntimeX86Mode mode=runtimeProcessX86Mode(pid); const auto modules = cachedProcessModules(pid, metadataNow()); return modules ? runtimeFormatProcessAddress(mode,*modules,address) : runtimeFormatAbsoluteAddress(mode,address);
    }

    bool runtimeDecodeProcessInstruction(const RuntimeX86Mode mode, const std::span<const std::uint8_t> bytes, const std::uintptr_t address, RuntimeDecodedInstruction& result)
    {
        result = {};
#if QUARTZ_HAS_ZYDIS
        ZydisDisassembledInstruction instruction{}; const ZydisMachineMode machine = mode == RuntimeX86Mode::X86 ? ZYDIS_MACHINE_MODE_LEGACY_32 : ZYDIS_MACHINE_MODE_LONG_64;
        if (!ZYAN_SUCCESS(ZydisDisassembleIntel(machine, address, bytes.data(), bytes.size(), &instruction))) return false;
        result.Text = runtimeNormalizeOpcodeText(instruction.text); result.Length = instruction.info.length;
        std::string mnemonic = result.Text.substr(0, result.Text.find_first_of(" \t")); std::ranges::transform(mnemonic, mnemonic.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (mnemonic == "call") result.Branch = RuntimeBranchKind::Call;
        else if (mnemonic == "jmp") result.Branch = RuntimeBranchKind::Unconditional;
        else if (!mnemonic.empty() && mnemonic.front() == 'j') result.Branch = RuntimeBranchKind::Conditional;
        else if (mnemonic == "ret" || mnemonic == "retf") result.Branch = RuntimeBranchKind::Return;
        if ((result.Branch == RuntimeBranchKind::Conditional || result.Branch == RuntimeBranchKind::Unconditional || result.Branch == RuntimeBranchKind::Call) && instruction.info.operand_count_visible)
        {
            const auto& operand=instruction.operands[0]; ZyanU64 target=0; bool resolved=ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction.info,&operand,address,&target));
            if (!resolved && operand.type==ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative)
            {
                const std::int64_t displacement=operand.imm.value.s; target=static_cast<ZyanU64>(static_cast<std::int64_t>(address+instruction.info.length)+displacement); resolved=true;
            }
            if (resolved)
            {
                if (mode==RuntimeX86Mode::X86) target&=0xFFFFFFFFULL;
                if (target<=std::numeric_limits<std::uintptr_t>::max()) result.Target=static_cast<std::uintptr_t>(target);
            }
        }
        return result.Length != 0;
#else
        (void)mode; (void)bytes; (void)address; return false;
#endif
    }

    bool runtimeDecodeProcessInstructionText(const RuntimeX86Mode mode, const std::span<const std::uint8_t> bytes, const std::uintptr_t address, std::string& text, std::size_t& length)
    {
        RuntimeDecodedInstruction instruction; if (!runtimeDecodeProcessInstruction(mode, bytes, address, instruction)) return false; text = std::move(instruction.Text); length = instruction.Length; return true;
    }

    bool runtimeDecodeProcessInstructionText(const pid_t pid, const std::span<const std::uint8_t> bytes, const std::uintptr_t address, std::string& text, std::size_t& length)
    {
        return runtimeDecodeProcessInstructionText(runtimeProcessX86Mode(pid), bytes, address, text, length);
    }

    bool runtimeAssembleInstructionText(const RuntimeX86Mode mode, const std::uintptr_t address, const std::string_view source, std::vector<std::uint8_t>& bytes, std::string& error)
    {
        bytes.clear(); error.clear(); if (source.empty()) { error = "assembly source is empty"; return false; }
        if (!commandExists("as") || !commandExists("ld") || !commandExists("objcopy")) { error = "GNU binutils (as, ld and objcopy) are required for assembler mode"; return false; }
        const auto id = std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())) + "-" + std::to_string(static_cast<long long>(getpid()));
        const std::filesystem::path base = std::filesystem::temp_directory_path() / ("quartz-asm-" + id); const auto sourcePath = base.string() + ".s", objectPath = base.string() + ".o", elfPath = base.string() + ".elf", binaryPath = base.string() + ".bin";
        {
            std::ofstream file(sourcePath, std::ios::binary | std::ios::trunc); if (!file) { error = "could not create temporary assembler source"; return false; }
            file << ".intel_syntax noprefix\n.text\n.global _start\n_start:\n"; file.write(source.data(), static_cast<std::streamsize>(source.size())); file << '\n';
            if (!file) { error = "could not write temporary assembler source"; removeAssemblerFiles(base); return false; }
        }
        std::string output; const std::string asMode = mode == RuntimeX86Mode::X86 ? "--32" : "--64"; const std::string ldMode = mode == RuntimeX86Mode::X86 ? "elf_i386" : "elf_x86_64";
        if (!runAssemblerCommand("as " + asMode + " -o " + shellQuote(objectPath) + " " + shellQuote(sourcePath), output)) { error = output.empty() ? "assembler failed" : output; removeAssemblerFiles(base); return false; }
        std::ostringstream baseAddress; baseAddress << "0x" << std::hex << std::uppercase << static_cast<unsigned long long>(address);
        if (!runAssemblerCommand("ld -m " + ldMode + " -Ttext=" + baseAddress.str() + " -e _start -o " + shellQuote(elfPath) + " " + shellQuote(objectPath), output)) { error = output.empty() ? "linker failed" : output; removeAssemblerFiles(base); return false; }
        if (!runAssemblerCommand("objcopy -O binary -j .text " + shellQuote(elfPath) + " " + shellQuote(binaryPath), output)) { error = output.empty() ? "objcopy failed" : output; removeAssemblerFiles(base); return false; }
        std::error_code ec; const auto size = std::filesystem::file_size(binaryPath, ec); if (ec || size == 0 || size > 4096) { error = ec ? "could not read assembled output" : size == 0 ? "assembler produced no bytes" : "assembled patch exceeds 4096 bytes"; removeAssemblerFiles(base); return false; }
        std::ifstream file(binaryPath, std::ios::binary); bytes.resize(static_cast<std::size_t>(size)); if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) { bytes.clear(); error = "could not read assembled bytes"; removeAssemblerFiles(base); return false; }
        removeAssemblerFiles(base); return true;
    }
}
