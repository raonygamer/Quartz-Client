#pragma once
#include "quartz/client/native/NativeTypes.hpp"
#include <memory>

namespace quartz::client
{
    enum class MemoryScanValueType : int { U8, I8, U16, I16, U32, I32, U64, I64, Float, Double, Pointer, Bool, Utf8String, Utf16String, ByteArray };
    enum class MemoryScanComparison : int { Exact, NotEqual, UnknownInitial, Changed, Unchanged, Increased, Decreased, IncreasedBy, DecreasedBy, Greater, Less, Between, ChangedFromTo };

    struct MemoryScanRequest
    {
        pid_t Pid = 0;
        MemoryScanValueType Type = MemoryScanValueType::I32;
        MemoryScanComparison Comparison = MemoryScanComparison::Exact;
        std::string ValueA;
        std::string ValueB;
        bool WritableOnly = true;
        bool ExecutableOnly = false;
        bool Aligned = true;
        bool CaseSensitive = true;
    };

    struct MemoryScanStats
    {
        bool Running = false;
        std::uint64_t Bytes = 0;
        std::uint64_t TotalBytes = 0;
        std::uint64_t Candidates = 0;
        double Seconds = 0.0;
        double MiBs = 0.0;
        std::string Status;
    };

    struct MemoryScanResultRow { std::uintptr_t Address = 0; std::string Value; };
    struct MemoryScanSnapshot;
    struct MemoryScanJobState;

    class MemoryScanner
    {
    public:
        MemoryScanner();
        ~MemoryScanner();
        MemoryScanner(const MemoryScanner&) = delete;
        MemoryScanner& operator=(const MemoryScanner&) = delete;

        bool newScan(const MemoryScanRequest& request, std::string& error);
        bool nextScan(const MemoryScanRequest& request, std::string& error);
        void cancel() noexcept;
        void poll();
        [[nodiscard]] bool running() const noexcept;
        [[nodiscard]] bool hasSnapshot() const noexcept;
        [[nodiscard]] MemoryScanStats stats() const;
        [[nodiscard]] std::vector<MemoryScanResultRow> results(std::size_t limit = 256) const;
        [[nodiscard]] pid_t pid() const noexcept;
        [[nodiscard]] MemoryScanValueType valueType() const noexcept;

    private:
        void invalidateResultCache() const noexcept { _cachedResults.clear(); _cachedResultLimit = 0; }

        std::shared_ptr<MemoryScanJobState> _job;
        std::unique_ptr<MemoryScanSnapshot> _snapshot;
        MemoryScanStats _lastStats;
        mutable std::vector<MemoryScanResultRow> _cachedResults;
        mutable std::size_t _cachedResultLimit = 0;
    };

    const char* memoryScanValueTypeName(MemoryScanValueType type) noexcept;
    const char* memoryScanComparisonName(MemoryScanComparison comparison) noexcept;
    std::string deriveRuntimeBytePattern(pid_t pid, std::uintptr_t start, std::uintptr_t end, bool wildcardRelocations, std::string& error);
}
