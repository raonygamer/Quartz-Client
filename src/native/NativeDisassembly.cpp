#include "quartz/client/native/NativeDisassembly.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/Model.hpp"
#include <elf.h>

namespace quartz::client
{
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
}
