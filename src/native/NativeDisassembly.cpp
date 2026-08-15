#include "quartz/client/native/NativeDisassembly.hpp"
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

    bool runtimeDecodeProcessInstructionText(const RuntimeX86Mode mode, const std::span<const std::uint8_t> bytes, const std::uintptr_t address, std::string& text, std::size_t& length)
    {
#if QUARTZ_HAS_ZYDIS
        ZydisDisassembledInstruction instruction{}; const ZydisMachineMode machine = mode == RuntimeX86Mode::X86 ? ZYDIS_MACHINE_MODE_LEGACY_32 : ZYDIS_MACHINE_MODE_LONG_64;
        if (!ZYAN_SUCCESS(ZydisDisassembleIntel(machine, address, bytes.data(), bytes.size(), &instruction))) return false;
        text = runtimeNormalizeOpcodeText(instruction.text); length = instruction.info.length; return length != 0;
#else
        (void)mode; (void)bytes; (void)address; (void)text; (void)length; return false;
#endif
    }

    bool runtimeDecodeProcessInstructionText(const pid_t pid, const std::span<const std::uint8_t> bytes, const std::uintptr_t address, std::string& text, std::size_t& length)
    {
        return runtimeDecodeProcessInstructionText(runtimeProcessX86Mode(pid), bytes, address, text, length);
    }
}
