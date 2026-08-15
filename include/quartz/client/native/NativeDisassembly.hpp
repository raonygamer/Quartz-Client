#pragma once
#include "quartz/client/native/NativeTypes.hpp"

namespace quartz::client
{
    enum class RuntimeX86Mode : std::uint8_t { X86, X64 };
    RuntimeX86Mode runtimeProcessX86Mode(pid_t pid) noexcept;
    const char* runtimeX86ModeName(RuntimeX86Mode mode) noexcept;
    bool runtimeDecodeProcessInstructionText(RuntimeX86Mode mode, std::span<const std::uint8_t> bytes, std::uintptr_t address, std::string& text, std::size_t& length);
    bool runtimeDecodeProcessInstructionText(pid_t pid, std::span<const std::uint8_t> bytes, std::uintptr_t address, std::string& text, std::size_t& length);
}
