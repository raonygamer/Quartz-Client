#include "quartz/client/native/SignatureScanner.hpp"
#include "quartz/client/Functions.hpp"
#include <libhat/scanner.hpp>
#include <libhat/signature.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <span>

namespace quartz::client
{
    namespace
    {
        constexpr double BytesPerMiB = 1024.0 * 1024.0;
        std::atomic<std::size_t> ReadChunkBytes{DefaultSignatureScanChunkBytes};

        std::int64_t steadyNowNs() noexcept
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void finishSignatureScan(const std::shared_ptr<SignatureScanState>& state, SignatureScanResult result)
        {
            const std::int64_t finishedNs = steadyNowNs();
            const std::int64_t startedNs = state->StartedNs.load(std::memory_order_acquire);
            result.ScannedBytes = state->ScannedBytes.load(std::memory_order_relaxed);
            result.DurationSeconds = startedNs > 0 && finishedNs > startedNs ? static_cast<double>(finishedNs - startedNs) / 1'000'000'000.0 : 0.0;
            {
                std::lock_guard lock(state->ResultMutex);
                state->Result = std::move(result);
            }
            state->FinishedNs.store(finishedNs, std::memory_order_release);
            state->Finished.store(true, std::memory_order_release);
        }
    }

    std::size_t normalizeSignatureScanChunkBytes(const std::size_t bytes) noexcept
    {
        const std::size_t clamped = std::clamp(bytes, MinimumSignatureScanChunkBytes, MaximumSignatureScanChunkBytes);
        return clamped - clamped % SignatureScanChunkAlignment;
    }

    std::size_t signatureScanChunkBytes() noexcept { return ReadChunkBytes.load(std::memory_order_relaxed); }
    void setSignatureScanChunkBytes(const std::size_t bytes) noexcept { ReadChunkBytes.store(normalizeSignatureScanChunkBytes(bytes), std::memory_order_relaxed); }

    std::shared_ptr<SignatureScanState> startSignatureScan(const pid_t pid, std::vector<RuntimeProcessRegion> regions, std::vector<std::uint8_t> bytes, std::vector<std::uint8_t> masks, const bool executableOnly, const std::uint64_t generation)
    {
        auto state = std::make_shared<SignatureScanState>();
        state->Generation = generation; state->ChunkBytes = signatureScanChunkBytes();
        for (const auto& region : regions) state->TotalBytes += region.End - region.Base;
        async::globalThreadPool().submit([state, pid, regions = std::move(regions), bytes = std::move(bytes), masks = std::move(masks), executableOnly](std::stop_token stop) mutable
        {
            state->StartedNs.store(steadyNowNs(), std::memory_order_release);
            try
            {
                hat::signature signature;
                signature.reserve(bytes.size());
                for (std::size_t i = 0; i < bytes.size(); ++i) signature.emplace_back(static_cast<std::byte>(bytes[i]), static_cast<std::byte>(masks[i]));
                const hat::scan_hint hint = executableOnly ? hat::scan_hint::x86_64 : hat::scan_hint::none;
                const std::size_t overlap = signature.size() > 1 ? signature.size() - 1 : 0;
                std::vector<std::uint8_t> buffer;

                for (const auto& region : regions)
                {
                    std::uintptr_t cursor = region.Base;
                    while (cursor < region.End)
                    {
                        if (stop.stop_requested() || state->CancelRequested.load(std::memory_order_relaxed))
                        {
                            SignatureScanResult result; result.Cancelled = true; result.Error = "signature scan cancelled";
                            finishSignatureScan(state, std::move(result));
                            return;
                        }
                        const std::size_t remaining = static_cast<std::size_t>(region.End - cursor);
                        const std::size_t readSize = std::min(remaining, state->ChunkBytes + overlap);
                        if (readSize < signature.size()) break;
                        buffer.resize(readSize);
                        std::string readError;
                        if (!readProcessMemoryBlock(pid, cursor, buffer, readError)) break;
                        const std::span<const std::byte> data{reinterpret_cast<const std::byte*>(buffer.data()), buffer.size()};
                        const auto match = hat::find_pattern(data, signature, hat::scan_alignment::X1, hint);
                        state->ScannedBytes.fetch_add(readSize, std::memory_order_relaxed);
                        if (match.has_result())
                        {
                            SignatureScanResult result;
                            result.Found = true;
                            result.MatchAddress = cursor + static_cast<std::size_t>(match.get() - data.data());
                            finishSignatureScan(state, std::move(result));
                            return;
                        }
                        const std::size_t step = readSize > overlap ? readSize - overlap : readSize;
                        cursor += step;
                    }
                }
                finishSignatureScan(state, {});
            }
            catch (const std::exception& e)
            {
                SignatureScanResult result; result.Error = e.what();
                finishSignatureScan(state, std::move(result));
            }
            catch (...)
            {
                SignatureScanResult result; result.Error = "unknown signature scanner failure";
                finishSignatureScan(state, std::move(result));
            }
        }, async::TaskPriority::High);
        return state;
    }

    void cancelSignatureScan(const std::shared_ptr<SignatureScanState>& state) noexcept
    {
        if (state) state->CancelRequested.store(true, std::memory_order_relaxed);
    }

    bool tryGetSignatureScanResult(const std::shared_ptr<SignatureScanState>& state, SignatureScanResult& result)
    {
        if (!state || !state->Finished.load(std::memory_order_acquire)) return false;
        std::lock_guard lock(state->ResultMutex);
        result = state->Result;
        return true;
    }

    double signatureScanAverageMiBs(const std::shared_ptr<SignatureScanState>& state) noexcept
    {
        if (!state) return 0.0;
        const std::int64_t startedNs = state->StartedNs.load(std::memory_order_acquire);
        if (startedNs <= 0) return 0.0;
        const std::int64_t finishedNs = state->FinishedNs.load(std::memory_order_acquire);
        const std::int64_t endNs = finishedNs > 0 ? finishedNs : steadyNowNs();
        if (endNs <= startedNs) return 0.0;
        const double seconds = static_cast<double>(endNs - startedNs) / 1'000'000'000.0;
        return static_cast<double>(state->ScannedBytes.load(std::memory_order_relaxed)) / BytesPerMiB / seconds;
    }
}
