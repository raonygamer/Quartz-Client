#include "quartz/client/native/NativeDisassembly.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/Model.hpp"
#include <elf.h>

namespace quartz::client
{
    namespace
    {
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
        std::ifstream file("/proc/" + std::to_string(pid) + "/exe", std::ios::binary);
        std::array<unsigned char, EI_NIDENT> ident{};
        if (!file.read(reinterpret_cast<char*>(ident.data()), static_cast<std::streamsize>(ident.size()))) return RuntimeX86Mode::X64;
        if (ident[EI_MAG0] != ELFMAG0 || ident[EI_MAG1] != ELFMAG1 || ident[EI_MAG2] != ELFMAG2 || ident[EI_MAG3] != ELFMAG3) return RuntimeX86Mode::X64;
        return ident[EI_CLASS] == ELFCLASS32 ? RuntimeX86Mode::X86 : RuntimeX86Mode::X64;
    }

    const char* runtimeX86ModeName(const RuntimeX86Mode mode) noexcept { return mode == RuntimeX86Mode::X86 ? "x86" : "x86-64"; }

    std::string runtimeFormatProcessAddress(const std::span<const RuntimeProcessModule> modules, const std::uintptr_t address)
    {
        for (const auto& module : modules)
        {
            if (address < module.Base || address >= module.End) continue;
            std::string name = module.Name;
            if (name.empty() && !module.Path.empty()) name = std::filesystem::path(module.Path).filename().string();
            if (name.empty()) break;
            std::ostringstream out; out << name << "+0x" << std::hex << std::uppercase << static_cast<unsigned long long>(address - module.Base); return out.str();
        }
        return runtimeHexAddress(address);
    }

    std::string runtimeFormatProcessAddress(const pid_t pid, const std::uintptr_t address)
    {
        if (pid <= 0 || address == 0) return runtimeHexAddress(address);
        return runtimeFormatProcessAddress(enumerateRuntimeModules(pid), address);
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
            ZyanU64 target = 0; if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction.info, &instruction.operands[0], address, &target)) && target <= std::numeric_limits<std::uintptr_t>::max()) result.Target = static_cast<std::uintptr_t>(target);
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
