#pragma once
#include "quartz/client/Functions.hpp"

namespace quartz::client
{
    enum class ProcessValueType : int
    {
        U8, I8, U16, I16, U32, I32, U64, I64, Float, Double, Bool
    };

    enum class ProcessAddressMode : int
    {
        AddressChain,
        Signature
    };

    enum class RuntimeSignaturePatternKind : int
    {
        HexadecimalPattern,
        OpcodePattern
    };

    enum class SignatureResultMode : int
    {
        MatchAddress,
        RipRelative32,
        PointerAtOffset,
        RegisterRelativeCapture,
        Address32AtOffset
    };

    enum class RuntimeX64Register : int
    {
        Rax, Rbx, Rcx, Rdx, Rsi, Rdi, Rbp, Rsp, R8, R9, R10, R11, R12, R13, R14, R15
    };

    enum class RuntimeDisplacementType : int
    {
        I8, I32, Manual
    };

    enum class ProcessRebindMode : int
    {
        NameExact,
        ExecutableExact,
        TitleExact,
        CommandLineExact,
        NameRegex,
        ExecutableRegex,
        TitleRegex,
        CommandLineRegex,
        AnyRegex
    };

    struct RuntimeProcessInfo
    {
        pid_t Pid = 0;
        std::string Name;
        std::string Exe;
        std::string Title;
        std::string CommandLine;
        std::string SearchText;
    };

    struct RuntimeProcessModule
    {
        std::uintptr_t Base = 0;
        std::uintptr_t End = 0;
        std::string Name;
        std::string Path;
    };

    struct RuntimeProcessRegion
    {
        std::uintptr_t Base = 0;
        std::uintptr_t End = 0;
        bool Readable = false;
        bool Writable = false;
        bool Executable = false;
        std::string Path;
    };

    struct RuntimeRegisterCaptureState
    {
        std::mutex Mutex;
        bool Finished = false;
        bool Success = false;
        std::uint64_t RegisterValue = 0;
        std::intptr_t Displacement = 0;
        std::uintptr_t ResolvedAddress = 0;
        std::string Status;
        // Keep the worker last: members are destroyed in reverse order, so jthread requests stop
        // and joins while Mutex/Status/the result fields are still alive. The worker captures this state raw.
        std::jthread Worker;
    };

}
