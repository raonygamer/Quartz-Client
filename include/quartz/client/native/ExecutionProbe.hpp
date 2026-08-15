#pragma once
#include "quartz/client/native/NativeTypes.hpp"
#include <memory>

namespace quartz::client
{
    struct ExecutionProbeHit
    {
        double Time = 0.0;
        pid_t Pid = 0;
        pid_t Tid = 0;
        std::uintptr_t Address = 0;
        user_regs_struct Registers{};
        bool HasRegisters = false;
    };

    struct ExecutionProbeState;

    class ExecutionProbe
    {
    public:
        ExecutionProbe() = default;
        ~ExecutionProbe();
        ExecutionProbe(const ExecutionProbe&) = delete;
        ExecutionProbe& operator=(const ExecutionProbe&) = delete;
        bool start(pid_t pid, std::uintptr_t address, std::string& error);
        void stop() noexcept;
        [[nodiscard]] bool running() const noexcept;
        [[nodiscard]] std::string status() const;
        [[nodiscard]] std::optional<ExecutionProbeHit> hit() const;
        [[nodiscard]] pid_t pid() const noexcept;
        [[nodiscard]] std::uintptr_t address() const noexcept;
    private:
        std::shared_ptr<ExecutionProbeState> _state;
    };

    ExecutionProbe& executionProbe();
}
