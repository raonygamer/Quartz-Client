#pragma once
#include "quartz/client/async/ThreadPool.hpp"
#include "quartz/client/native/NativeTypes.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace quartz::client
{
    inline constexpr std::size_t DefaultSignatureScanChunkBytes = 4ULL * 1024ULL * 1024ULL;
    inline constexpr std::size_t MinimumSignatureScanChunkBytes = 64ULL * 1024ULL;
    inline constexpr std::size_t MaximumSignatureScanChunkBytes = 64ULL * 1024ULL * 1024ULL;
    inline constexpr std::size_t SignatureScanChunkAlignment = 8;

    struct SignatureScanResult
    {
        bool Found = false;
        bool Cancelled = false;
        std::uintptr_t MatchAddress = 0;
        std::uint64_t ScannedBytes = 0;
        double DurationSeconds = 0.0;
        std::string Error;
    };

    struct SignatureScanState
    {
        std::uint64_t Generation = 0;
        std::uint64_t TotalBytes = 0;
        std::size_t ChunkBytes = DefaultSignatureScanChunkBytes;
        std::atomic<std::uint64_t> ScannedBytes{0};
        std::atomic<std::int64_t> StartedNs{0};
        std::atomic<std::int64_t> FinishedNs{0};
        std::atomic<bool> CancelRequested{false};
        std::atomic<bool> Finished{false};
        std::mutex ResultMutex;
        SignatureScanResult Result;
    };

    std::size_t normalizeSignatureScanChunkBytes(std::size_t bytes) noexcept;
    std::size_t signatureScanChunkBytes() noexcept;
    void setSignatureScanChunkBytes(std::size_t bytes) noexcept;
    std::shared_ptr<SignatureScanState> startSignatureScan(pid_t pid, std::vector<RuntimeProcessRegion> regions, std::vector<std::uint8_t> bytes, std::vector<std::uint8_t> masks, bool executableOnly, std::uint64_t generation);
    void cancelSignatureScan(const std::shared_ptr<SignatureScanState>& state) noexcept;
    bool tryGetSignatureScanResult(const std::shared_ptr<SignatureScanState>& state, SignatureScanResult& result);
    double signatureScanAverageMiBs(const std::shared_ptr<SignatureScanState>& state) noexcept;
}
