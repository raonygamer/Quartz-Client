#pragma once
#include "quartz/client/native/NativeTypes.hpp"
#include <memory>

namespace quartz::client
{
    enum class MemoryWatchAccess : int { Write, ReadWrite };
    struct MemoryWatchHit { double Time = 0.0; pid_t Tid = 0; std::uintptr_t Rip = 0; std::uintptr_t InstructionAddress = 0; std::uint64_t Count = 0; std::string Instruction; };
    struct MemoryWatchState;

    class MemoryWatch
    {
    public:
        MemoryWatch() = default;
        ~MemoryWatch();
        MemoryWatch(const MemoryWatch&) = delete;
        MemoryWatch& operator=(const MemoryWatch&) = delete;
        bool start(pid_t pid, std::uintptr_t address, std::size_t size, MemoryWatchAccess access, std::size_t maxHits, std::string& error);
        void stop() noexcept;
        void clearHits();
        [[nodiscard]] bool running() const noexcept;
        [[nodiscard]] std::string status() const;
        [[nodiscard]] std::vector<MemoryWatchHit> hits() const;
    private:
        std::shared_ptr<MemoryWatchState> _state;
    };
}
