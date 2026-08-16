#pragma once
#include "quartz/client/native/NativeTypes.hpp"
#include <optional>

namespace quartz::client
{
    enum class RuntimeX86Mode : std::uint8_t { X86, X64 };
    enum class RuntimeBranchKind : std::uint8_t { None, Conditional, Unconditional, Call, Return };

    struct RuntimeDecodedInstruction
    {
        std::string Text;
        std::size_t Length = 0;
        RuntimeBranchKind Branch = RuntimeBranchKind::None;
        std::optional<std::uintptr_t> Target;
    };

    RuntimeX86Mode runtimeProcessX86Mode(pid_t pid) noexcept;
    const char* runtimeX86ModeName(RuntimeX86Mode mode) noexcept;
    std::vector<RuntimeProcessModule> runtimeProcessModules(pid_t pid);
    std::string runtimeFormatProcessAddress(std::span<const RuntimeProcessModule> modules, std::uintptr_t address);
    std::string runtimeFormatProcessAddress(pid_t pid, std::uintptr_t address);
    bool runtimeDecodeProcessInstruction(RuntimeX86Mode mode, std::span<const std::uint8_t> bytes, std::uintptr_t address, RuntimeDecodedInstruction& instruction);
    bool runtimeDecodeProcessInstructionText(RuntimeX86Mode mode, std::span<const std::uint8_t> bytes, std::uintptr_t address, std::string& text, std::size_t& length);
    bool runtimeDecodeProcessInstructionText(pid_t pid, std::span<const std::uint8_t> bytes, std::uintptr_t address, std::string& text, std::size_t& length);
    bool runtimeAssembleInstructionText(RuntimeX86Mode mode, std::uintptr_t address, std::string_view source, std::vector<std::uint8_t>& bytes, std::string& error);
}
