#pragma once
#include "quartz/client/Forward.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace quartz::client
{
    enum class RuntimeFunctionCandidateSource : std::uint8_t
    {
        Prologue,
        EndBranchTarget,
        CallTarget
    };

    struct RuntimeFunctionCandidate
    {
        std::uintptr_t Address = 0;
        RuntimeFunctionCandidateSource Source = RuntimeFunctionCandidateSource::Prologue;
        float Confidence = 0.0f;
    };

    struct RuntimeFunctionAnalysisSnapshot
    {
        pid_t Pid = 0;
        std::uintptr_t RangeStart = 0;
        std::uintptr_t RangeEnd = 0;
        bool Running = false;
        float Progress = 0.0f;
        std::uint64_t Revision = 0;
        std::vector<RuntimeFunctionCandidate> Candidates;
        std::string Status;
    };

    void requestRuntimeFunctionAnalysis(pid_t pid, std::uintptr_t around, bool force = false);
    RuntimeFunctionAnalysisSnapshot runtimeFunctionAnalysisSnapshot(pid_t pid, std::uintptr_t around);
    void runtimeObserveFunctionTarget(pid_t pid, std::uintptr_t address, RuntimeFunctionCandidateSource source = RuntimeFunctionCandidateSource::CallTarget);
    void invalidateRuntimeFunctionAnalysis(pid_t pid = 0);
    const char* runtimeFunctionCandidateSourceName(RuntimeFunctionCandidateSource source) noexcept;
}
