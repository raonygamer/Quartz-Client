#include "quartz/client/native/MemoryScanner.hpp"
#include "quartz/client/Model.hpp"
#include "quartz/client/async/ThreadPool.hpp"
#include <atomic>
#include <bit>
#include <cstring>
#if QUARTZ_HAS_ZYDIS
#include <Zydis/Zydis.h>
#endif

namespace quartz::client
{
    namespace
    {
        constexpr std::size_t ScanChunk = 4 * 1024 * 1024;

        struct MemoryScanRegionSnapshot
        {
            std::uintptr_t Base = 0;
            std::size_t ScanLength = 0;
            std::size_t FirstOffset = 0;
            std::size_t Step = 1;
            std::vector<std::uint8_t> Bytes;
            std::vector<std::uint64_t> Candidates;
            std::uint64_t CandidateCount = 0;
        };

        struct ParsedScanValue
        {
            std::size_t Width = 0;
            long double NumericA = 0.0L;
            long double NumericB = 0.0L;
            bool HasA = false;
            bool HasB = false;
            std::vector<std::uint8_t> A;
            std::vector<std::uint8_t> B;
            std::vector<std::uint8_t> MaskA;
            std::vector<std::uint8_t> MaskB;
        };

        std::size_t numericWidth(const MemoryScanValueType type) noexcept
        {
            switch (type)
            {
            case MemoryScanValueType::U8: case MemoryScanValueType::I8: case MemoryScanValueType::Bool: return 1;
            case MemoryScanValueType::U16: case MemoryScanValueType::I16: return 2;
            case MemoryScanValueType::U32: case MemoryScanValueType::I32: case MemoryScanValueType::Float: return 4;
            case MemoryScanValueType::U64: case MemoryScanValueType::I64: case MemoryScanValueType::Double: case MemoryScanValueType::Pointer: return 8;
            default: return 0;
            }
        }

        bool isNumeric(const MemoryScanValueType type) noexcept { return numericWidth(type) != 0; }
        bool comparisonNeedsPrevious(const MemoryScanComparison c) noexcept { return c == MemoryScanComparison::Changed || c == MemoryScanComparison::Unchanged || c == MemoryScanComparison::Increased || c == MemoryScanComparison::Decreased || c == MemoryScanComparison::IncreasedBy || c == MemoryScanComparison::DecreasedBy || c == MemoryScanComparison::ChangedFromTo; }
        bool comparisonNeedsA(const MemoryScanComparison c) noexcept { return c == MemoryScanComparison::Exact || c == MemoryScanComparison::NotEqual || c == MemoryScanComparison::IncreasedBy || c == MemoryScanComparison::DecreasedBy || c == MemoryScanComparison::Greater || c == MemoryScanComparison::Less || c == MemoryScanComparison::Between || c == MemoryScanComparison::ChangedFromTo; }
        bool comparisonNeedsB(const MemoryScanComparison c) noexcept { return c == MemoryScanComparison::Between || c == MemoryScanComparison::ChangedFromTo; }

        bool parseNumeric(const std::string& text, long double& value)
        {
            if (text.empty()) return false;
            char* end = nullptr;
            errno = 0;
            value = std::strtold(text.c_str(), &end);
            return errno == 0 && end == text.c_str() + text.size() && std::isfinite(value);
        }

        std::vector<std::uint8_t> utf16Bytes(const std::string_view text)
        {
            std::vector<std::uint8_t> result;
            result.reserve(text.size() * 2);
            const unsigned char* p = reinterpret_cast<const unsigned char*>(text.data());
            const unsigned char* end = p + text.size();
            while (p < end)
            {
                std::uint32_t cp = *p++;
                if (cp >= 0xC2 && cp <= 0xDF && p < end) cp = ((cp & 0x1F) << 6) | (*p++ & 0x3F);
                else if (cp >= 0xE0 && cp <= 0xEF && p + 1 < end) { cp = ((cp & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
                else if (cp >= 0xF0 && cp <= 0xF4 && p + 2 < end) { cp = ((cp & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
                if (cp <= 0xFFFF)
                {
                    result.push_back(static_cast<std::uint8_t>(cp & 0xFF)); result.push_back(static_cast<std::uint8_t>((cp >> 8) & 0xFF));
                }
                else
                {
                    cp -= 0x10000; const std::uint16_t high = static_cast<std::uint16_t>(0xD800 + (cp >> 10)), low = static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF));
                    result.push_back(static_cast<std::uint8_t>(high & 0xFF)); result.push_back(static_cast<std::uint8_t>(high >> 8)); result.push_back(static_cast<std::uint8_t>(low & 0xFF)); result.push_back(static_cast<std::uint8_t>(low >> 8));
                }
            }
            return result;
        }

        bool parseBytes(const MemoryScanValueType type, const std::string& text, std::vector<std::uint8_t>& bytes, std::vector<std::uint8_t>& masks, std::string& error)
        {
            bytes.clear(); masks.clear();
            if (type == MemoryScanValueType::Utf8String)
            {
                bytes.assign(text.begin(), text.end()); masks.assign(bytes.size(), 0xFF); if (bytes.empty()) { error = "string cannot be empty"; return false; } return true;
            }
            if (type == MemoryScanValueType::Utf16String)
            {
                bytes = utf16Bytes(text); masks.assign(bytes.size(), 0xFF); if (bytes.empty()) { error = "string cannot be empty"; return false; } return true;
            }
            if (type == MemoryScanValueType::ByteArray) return parseRuntimeHexPattern(text, bytes, masks, error);
            return false;
        }

        bool parseRequestValues(const MemoryScanRequest& request, const std::size_t existingWidth, ParsedScanValue& parsed, std::string& error)
        {
            parsed = {};
            if (isNumeric(request.Type))
            {
                parsed.Width = numericWidth(request.Type);
                if (comparisonNeedsA(request.Comparison) && !parseNumeric(request.ValueA, parsed.NumericA)) { error = "invalid first numeric value"; return false; }
                if (comparisonNeedsB(request.Comparison) && !parseNumeric(request.ValueB, parsed.NumericB)) { error = "invalid second numeric value"; return false; }
                parsed.HasA = comparisonNeedsA(request.Comparison); parsed.HasB = comparisonNeedsB(request.Comparison);
                return true;
            }
            std::string localError;
            if (comparisonNeedsA(request.Comparison) || existingWidth == 0)
            {
                if (!parseBytes(request.Type, request.ValueA, parsed.A, parsed.MaskA, localError)) { error = localError.empty() ? "a value is required to determine scan width" : localError; return false; }
                parsed.HasA = true; parsed.Width = parsed.A.size();
            }
            else parsed.Width = existingWidth;
            if (comparisonNeedsB(request.Comparison))
            {
                if (!parseBytes(request.Type, request.ValueB, parsed.B, parsed.MaskB, localError)) { error = localError; return false; }
                if (parsed.B.size() != parsed.Width) { error = "from/to values must have the same byte length"; return false; }
                parsed.HasB = true;
            }
            if (existingWidth != 0 && parsed.Width != existingWidth) { error = "value width differs from the current scan"; return false; }
            if ((request.Comparison == MemoryScanComparison::Increased || request.Comparison == MemoryScanComparison::Decreased || request.Comparison == MemoryScanComparison::IncreasedBy || request.Comparison == MemoryScanComparison::DecreasedBy || request.Comparison == MemoryScanComparison::Greater || request.Comparison == MemoryScanComparison::Less || request.Comparison == MemoryScanComparison::Between) && !isNumeric(request.Type)) { error = "ordered comparisons require a numeric value type"; return false; }
            return parsed.Width != 0;
        }

        long double numericAt(const MemoryScanValueType type, const std::uint8_t* data)
        {
            switch (type)
            {
            case MemoryScanValueType::U8: { std::uint8_t v; std::memcpy(&v, data, 1); return v; }
            case MemoryScanValueType::I8: { std::int8_t v; std::memcpy(&v, data, 1); return v; }
            case MemoryScanValueType::U16: { std::uint16_t v; std::memcpy(&v, data, 2); return v; }
            case MemoryScanValueType::I16: { std::int16_t v; std::memcpy(&v, data, 2); return v; }
            case MemoryScanValueType::U32: { std::uint32_t v; std::memcpy(&v, data, 4); return v; }
            case MemoryScanValueType::I32: { std::int32_t v; std::memcpy(&v, data, 4); return v; }
            case MemoryScanValueType::U64: case MemoryScanValueType::Pointer: { std::uint64_t v; std::memcpy(&v, data, 8); return static_cast<long double>(v); }
            case MemoryScanValueType::I64: { std::int64_t v; std::memcpy(&v, data, 8); return static_cast<long double>(v); }
            case MemoryScanValueType::Float: { float v; std::memcpy(&v, data, 4); return static_cast<long double>(v); }
            case MemoryScanValueType::Double: { double v; std::memcpy(&v, data, 8); return static_cast<long double>(v); }
            case MemoryScanValueType::Bool: { std::uint8_t v; std::memcpy(&v, data, 1); return v != 0 ? 1.0L : 0.0L; }
            default: return 0.0L;
            }
        }

        bool bytesEqual(const std::uint8_t* value, const std::vector<std::uint8_t>& expected, const std::vector<std::uint8_t>& masks, const MemoryScanValueType type, const bool caseSensitive)
        {
            for (std::size_t i = 0; i < expected.size(); ++i)
            {
                std::uint8_t actual = value[i], wanted = expected[i];
                if (!caseSensitive && type == MemoryScanValueType::Utf8String) { actual = static_cast<std::uint8_t>(std::tolower(actual)); wanted = static_cast<std::uint8_t>(std::tolower(wanted)); }
                if (!caseSensitive && type == MemoryScanValueType::Utf16String && (i & 1) == 0 && value[i + 1] == 0 && expected[i + 1] == 0) { actual = static_cast<std::uint8_t>(std::tolower(actual)); wanted = static_cast<std::uint8_t>(std::tolower(wanted)); }
                const std::uint8_t mask = masks.empty() ? 0xFF : masks[i];
                if ((actual & mask) != (wanted & mask)) return false;
            }
            return true;
        }

        bool valuesEqual(const MemoryScanValueType type, const std::uint8_t* a, const std::uint8_t* b, const std::size_t width, const bool caseSensitive)
        {
            if (isNumeric(type)) return numericAt(type, a) == numericAt(type, b);
            if (caseSensitive || type == MemoryScanValueType::ByteArray) return std::memcmp(a, b, width) == 0;
            if (type == MemoryScanValueType::Utf8String) for (std::size_t i = 0; i < width; ++i) if (std::tolower(a[i]) != std::tolower(b[i])) return false; else { }
            else if (type == MemoryScanValueType::Utf16String) for (std::size_t i = 0; i + 1 < width; i += 2) { const std::uint16_t av = static_cast<std::uint16_t>(a[i] | (a[i + 1] << 8)), bv = static_cast<std::uint16_t>(b[i] | (b[i + 1] << 8)); if (av < 128 && bv < 128 ? std::tolower(av) != std::tolower(bv) : av != bv) return false; }
            return true;
        }

        bool matches(const MemoryScanRequest& request, const ParsedScanValue& parsed, const std::uint8_t* current, const std::uint8_t* previous)
        {
            if (request.Comparison == MemoryScanComparison::UnknownInitial) return true;
            if (isNumeric(request.Type))
            {
                const long double now = numericAt(request.Type, current), before = previous ? numericAt(request.Type, previous) : 0.0L;
                switch (request.Comparison)
                {
                case MemoryScanComparison::Exact: return now == parsed.NumericA;
                case MemoryScanComparison::NotEqual: return now != parsed.NumericA;
                case MemoryScanComparison::Changed: return previous && now != before;
                case MemoryScanComparison::Unchanged: return previous && now == before;
                case MemoryScanComparison::Increased: return previous && now > before;
                case MemoryScanComparison::Decreased: return previous && now < before;
                case MemoryScanComparison::IncreasedBy: return previous && now == before + parsed.NumericA;
                case MemoryScanComparison::DecreasedBy: return previous && now == before - parsed.NumericA;
                case MemoryScanComparison::Greater: return now > parsed.NumericA;
                case MemoryScanComparison::Less: return now < parsed.NumericA;
                case MemoryScanComparison::Between: return now >= std::min(parsed.NumericA, parsed.NumericB) && now <= std::max(parsed.NumericA, parsed.NumericB);
                case MemoryScanComparison::ChangedFromTo: return previous && before == parsed.NumericA && now == parsed.NumericB;
                default: return true;
                }
            }
            switch (request.Comparison)
            {
            case MemoryScanComparison::Exact: return bytesEqual(current, parsed.A, parsed.MaskA, request.Type, request.CaseSensitive);
            case MemoryScanComparison::NotEqual: return !bytesEqual(current, parsed.A, parsed.MaskA, request.Type, request.CaseSensitive);
            case MemoryScanComparison::Changed: return previous && !valuesEqual(request.Type, current, previous, parsed.Width, request.CaseSensitive);
            case MemoryScanComparison::Unchanged: return previous && valuesEqual(request.Type, current, previous, parsed.Width, request.CaseSensitive);
            case MemoryScanComparison::ChangedFromTo: return previous && bytesEqual(previous, parsed.A, parsed.MaskA, request.Type, request.CaseSensitive) && bytesEqual(current, parsed.B, parsed.MaskB, request.Type, request.CaseSensitive);
            default: return false;
            }
        }

        bool bit(const std::vector<std::uint64_t>& bits, const std::size_t index) noexcept { return (bits[index / 64] & (1ULL << (index & 63))) != 0; }
        void setBit(std::vector<std::uint64_t>& bits, const std::size_t index) noexcept { bits[index / 64] |= 1ULL << (index & 63); }
        void clearBit(std::vector<std::uint64_t>& bits, const std::size_t index) noexcept { bits[index / 64] &= ~(1ULL << (index & 63)); }

        std::string formatValue(const MemoryScanValueType type, const std::uint8_t* data, const std::size_t width)
        {
            std::ostringstream out;
            if (isNumeric(type))
            {
                if (type == MemoryScanValueType::Pointer) { std::uint64_t v; std::memcpy(&v, data, 8); out << "0x" << std::hex << std::uppercase << v; }
                else if (type == MemoryScanValueType::Float || type == MemoryScanValueType::Double) out << std::setprecision(12) << static_cast<double>(numericAt(type, data));
                else out << std::fixed << std::setprecision(0) << numericAt(type, data);
                return out.str();
            }
            if (type == MemoryScanValueType::Utf8String) return std::string(reinterpret_cast<const char*>(data), width);
            if (type == MemoryScanValueType::Utf16String)
            {
                std::string result; result.reserve(width / 2);
                for (std::size_t i = 0; i + 1 < width; i += 2) { const std::uint16_t cp = static_cast<std::uint16_t>(data[i] | (data[i + 1] << 8)); result.push_back(cp >= 32 && cp < 127 ? static_cast<char>(cp) : '.'); }
                return result;
            }
            for (std::size_t i = 0; i < width; ++i) { if (i) out << ' '; out << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<unsigned>(data[i]); }
            return out.str();
        }
    }

    struct MemoryScanSnapshot
    {
        pid_t Pid = 0;
        MemoryScanValueType Type = MemoryScanValueType::I32;
        std::size_t Width = 0;
        bool Aligned = true;
        bool CaseSensitive = true;
        std::vector<MemoryScanRegionSnapshot> Regions;
        std::uint64_t Candidates = 0;
    };

    struct MemoryScanJobState
    {
        std::mutex Mutex;
        std::atomic_bool Finished{false};
        std::atomic_bool Success{false};
        std::atomic_bool Cancelled{false};
        std::atomic<std::uint64_t> Bytes{0};
        std::atomic<std::uint64_t> TotalBytes{0};
        std::atomic<std::uint64_t> Candidates{0};
        double Started = runtimeSteadySeconds();
        double Seconds = 0.0;
        std::string Status = "queued";
        std::unique_ptr<MemoryScanSnapshot> Result;
    };

    const char* memoryScanValueTypeName(const MemoryScanValueType type) noexcept
    {
        static constexpr const char* Names[] = {"u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "float", "double", "pointer", "bool", "UTF-8 string", "UTF-16 string", "byte array"};
        return Names[std::clamp(static_cast<int>(type), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    const char* memoryScanComparisonName(const MemoryScanComparison c) noexcept
    {
        static constexpr const char* Names[] = {"Exact value", "Not equal", "Unknown initial value", "Changed", "Unchanged", "Increased", "Decreased", "Increased by", "Decreased by", "Greater than", "Less than", "Between", "Changed from -> to"};
        return Names[std::clamp(static_cast<int>(c), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    MemoryScanner::MemoryScanner() = default;
    MemoryScanner::~MemoryScanner() { cancel(); _job.reset(); }
    bool MemoryScanner::running() const noexcept { return _job && !_job->Finished.load(std::memory_order_acquire); }
    bool MemoryScanner::hasSnapshot() const noexcept { return _snapshot != nullptr; }
    pid_t MemoryScanner::pid() const noexcept { return _snapshot ? _snapshot->Pid : 0; }
    MemoryScanValueType MemoryScanner::valueType() const noexcept { return _snapshot ? _snapshot->Type : MemoryScanValueType::I32; }
    void MemoryScanner::cancel() noexcept { if (_job) _job->Cancelled.store(true, std::memory_order_release); }

    MemoryScanStats MemoryScanner::stats() const
    {
        if (!_job) return _lastStats;
        MemoryScanStats stats;
        stats.Running = !_job->Finished.load(std::memory_order_acquire);
        stats.Bytes = _job->Bytes.load(std::memory_order_relaxed); stats.TotalBytes = _job->TotalBytes.load(std::memory_order_relaxed); stats.Candidates = _job->Candidates.load(std::memory_order_relaxed);
        stats.Seconds = stats.Running ? std::max(runtimeSteadySeconds() - _job->Started, 0.000001) : _job->Seconds;
        stats.MiBs = stats.Bytes / (1024.0 * 1024.0) / std::max(stats.Seconds, 0.000001);
        { std::lock_guard lock(_job->Mutex); stats.Status = _job->Status; }
        return stats;
    }

    void MemoryScanner::poll()
    {
        if (!_job || !_job->Finished.load(std::memory_order_acquire)) return;
        MemoryScanStats done = stats(); done.Running = false;
        {
            std::lock_guard lock(_job->Mutex);
            if (_job->Result) { _snapshot = std::move(_job->Result); invalidateResultCache(); }
            done.Status = _job->Status;
        }
        _lastStats = std::move(done); _job.reset();
    }

    bool MemoryScanner::newScan(const MemoryScanRequest& request, std::string& error)
    {
        if (running()) { error = "a scan is already running"; return false; }
        if (request.Pid <= 0) { error = "select a process first"; return false; }
        if (comparisonNeedsPrevious(request.Comparison)) { error = "this comparison requires Next Scan"; return false; }
        ParsedScanValue parsed;
        if (!parseRequestValues(request, 0, parsed, error)) return false;
        _snapshot.reset(); invalidateResultCache(); _lastStats = {};
        auto job = std::make_shared<MemoryScanJobState>(); _job = job;
        async::globalThreadPool().submit([job, request, parsed](std::stop_token stop)
        {
            auto snapshot = std::make_unique<MemoryScanSnapshot>(); snapshot->Pid = request.Pid; snapshot->Type = request.Type; snapshot->Width = parsed.Width; snapshot->Aligned = request.Aligned; snapshot->CaseSensitive = request.CaseSensitive;
            const auto regions = enumerateRuntimeRegions(request.Pid);
            std::uint64_t total = 0; for (const auto& r : regions) if (r.Readable && (!request.WritableOnly || r.Writable) && (!request.ExecutableOnly || r.Executable)) total += r.End - r.Base;
            job->TotalBytes.store(total, std::memory_order_relaxed);
            { std::lock_guard lock(job->Mutex); job->Status = "scanning process memory"; }
            std::string readError;
            for (const auto& region : regions)
            {
                if (stop.stop_requested() || job->Cancelled.load(std::memory_order_acquire)) break;
                if (!region.Readable || (request.WritableOnly && !region.Writable) || (request.ExecutableOnly && !region.Executable)) continue;
                for (std::uintptr_t cursor = region.Base; cursor < region.End;)
                {
                    if (stop.stop_requested() || job->Cancelled.load(std::memory_order_acquire)) break;
                    const std::size_t logical = static_cast<std::size_t>(std::min<std::uintptr_t>(ScanChunk, region.End - cursor));
                    const std::size_t readSize = static_cast<std::size_t>(std::min<std::uintptr_t>(logical + parsed.Width - 1, region.End - cursor));
                    std::vector<std::uint8_t> current(readSize);
                    if (readProcessMemoryBlock(request.Pid, cursor, current, readError))
                    {
                        MemoryScanRegionSnapshot part; part.Base = cursor; part.ScanLength = logical; part.Bytes = std::move(current); part.Step = request.Aligned ? parsed.Width : 1;
                        part.FirstOffset = request.Aligned ? (parsed.Width - (cursor % parsed.Width)) % parsed.Width : 0;
                        const std::size_t slots = part.FirstOffset < logical ? ((logical - 1 - part.FirstOffset) / part.Step + 1) : 0;
                        part.Candidates.assign((slots + 63) / 64, 0);
                        for (std::size_t i = 0; i < slots; ++i)
                        {
                            const std::size_t offset = part.FirstOffset + i * part.Step;
                            if (offset + parsed.Width > part.Bytes.size()) break;
                            if (matches(request, parsed, part.Bytes.data() + offset, nullptr)) { setBit(part.Candidates, i); ++part.CandidateCount; }
                        }
                        snapshot->Candidates += part.CandidateCount; snapshot->Regions.emplace_back(std::move(part));
                        job->Candidates.store(snapshot->Candidates, std::memory_order_relaxed);
                    }
                    cursor += logical; job->Bytes.fetch_add(logical, std::memory_order_relaxed);
                }
            }
            const bool cancelled = stop.stop_requested() || job->Cancelled.load(std::memory_order_acquire);
            job->Seconds = std::max(runtimeSteadySeconds() - job->Started, 0.000001);
            { std::lock_guard lock(job->Mutex); job->Result = std::move(snapshot); job->Status = cancelled ? "scan cancelled" : "scan complete"; }
            job->Success.store(!cancelled, std::memory_order_release); job->Finished.store(true, std::memory_order_release);
        }, async::TaskPriority::Background);
        error.clear(); return true;
    }

    bool MemoryScanner::nextScan(const MemoryScanRequest& request, std::string& error)
    {
        poll();
        if (running()) { error = "a scan is already running"; return false; }
        if (!_snapshot) { error = "run New Scan first"; return false; }
        if (request.Pid != _snapshot->Pid) { error = "process changed; start a New Scan"; return false; }
        if (request.Type != _snapshot->Type) { error = "value type changed; start a New Scan"; return false; }
        if (request.Comparison == MemoryScanComparison::UnknownInitial) { error = "Unknown initial value is only valid for New Scan"; return false; }
        ParsedScanValue parsed;
        if (!parseRequestValues(request, _snapshot->Width, parsed, error)) return false;
        auto job = std::make_shared<MemoryScanJobState>(); job->Result = std::move(_snapshot); invalidateResultCache(); job->Candidates.store(job->Result->Candidates, std::memory_order_relaxed);
        std::uint64_t total = 0; for (const auto& part : job->Result->Regions) total += part.ScanLength; job->TotalBytes.store(total, std::memory_order_relaxed); _job = job;
        async::globalThreadPool().submit([job, request, parsed](std::stop_token stop)
        {
            { std::lock_guard lock(job->Mutex); job->Status = "filtering previous candidates"; }
            auto* snapshot = job->Result.get(); std::uint64_t remaining = snapshot->Candidates; std::string readError;
            for (auto& part : snapshot->Regions)
            {
                if (stop.stop_requested() || job->Cancelled.load(std::memory_order_acquire)) break;
                std::vector<std::uint8_t> current(part.Bytes.size());
                if (!readProcessMemoryBlock(request.Pid, part.Base, current, readError))
                {
                    remaining -= part.CandidateCount; part.CandidateCount = 0; std::fill(part.Candidates.begin(), part.Candidates.end(), 0); job->Bytes.fetch_add(part.ScanLength, std::memory_order_relaxed); continue;
                }
                const std::size_t slots = part.Candidates.size() * 64;
                for (std::size_t i = 0; i < slots; ++i)
                {
                    if (!bit(part.Candidates, i)) continue;
                    const std::size_t offset = part.FirstOffset + i * part.Step;
                    if (offset + parsed.Width > part.Bytes.size() || offset + parsed.Width > current.size() || !matches(request, parsed, current.data() + offset, part.Bytes.data() + offset)) { clearBit(part.Candidates, i); --part.CandidateCount; --remaining; }
                }
                part.Bytes.swap(current); job->Candidates.store(remaining, std::memory_order_relaxed); job->Bytes.fetch_add(part.ScanLength, std::memory_order_relaxed);
            }
            snapshot->Candidates = remaining;
            const bool cancelled = stop.stop_requested() || job->Cancelled.load(std::memory_order_acquire);
            job->Seconds = std::max(runtimeSteadySeconds() - job->Started, 0.000001);
            { std::lock_guard lock(job->Mutex); job->Status = cancelled ? "next scan cancelled" : "next scan complete"; }
            job->Success.store(!cancelled, std::memory_order_release); job->Finished.store(true, std::memory_order_release);
        }, async::TaskPriority::Background);
        error.clear(); return true;
    }

    std::vector<MemoryScanResultRow> MemoryScanner::results(const std::size_t limit) const
    {
        if (!_snapshot || limit == 0) return {};
        if (_cachedResultLimit >= limit)
        {
            const std::size_t count = std::min(limit, _cachedResults.size());
            return {_cachedResults.begin(), _cachedResults.begin() + static_cast<std::ptrdiff_t>(count)};
        }

        _cachedResults.clear();
        _cachedResultLimit = limit;
        _cachedResults.reserve(std::min<std::uint64_t>(_snapshot->Candidates, limit));
        for (const auto& part : _snapshot->Regions)
        {
            for (std::size_t wordIndex = 0; wordIndex < part.Candidates.size() && _cachedResults.size() < limit; ++wordIndex)
            {
                std::uint64_t word = part.Candidates[wordIndex];
                while (word != 0 && _cachedResults.size() < limit)
                {
                    const unsigned bitIndex = std::countr_zero(word);
                    const std::size_t i = wordIndex * 64 + bitIndex;
                    const std::size_t offset = part.FirstOffset + i * part.Step;
                    if (offset + _snapshot->Width <= part.Bytes.size()) _cachedResults.push_back({part.Base + offset, formatValue(_snapshot->Type, part.Bytes.data() + offset, _snapshot->Width)});
                    word &= word - 1;
                }
            }
            if (_cachedResults.size() >= limit) break;
        }
        return _cachedResults;
    }

    std::string deriveRuntimeBytePattern(const pid_t pid, const std::uintptr_t start, const std::uintptr_t end, const bool wildcardRelocations, std::string& error)
    {
        if (pid <= 0 || start == 0 || end <= start) { error = "enter a valid PID and address range"; return {}; }
        const std::size_t size = static_cast<std::size_t>(end - start);
        if (size > 4096) { error = "pattern derivation is limited to 4096 bytes"; return {}; }
        std::vector<std::uint8_t> bytes(size); if (!readProcessMemoryBlock(pid, start, bytes, error)) return {};
        std::vector<bool> wildcard(size, false);
#if QUARTZ_HAS_ZYDIS
        if (wildcardRelocations)
        {
            ZydisDecoder decoder{}; ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
            std::size_t offset = 0;
            while (offset < bytes.size())
            {
                ZydisDecodedInstruction instruction{}; ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes.data() + offset, bytes.size() - offset, &instruction, operands)) || instruction.length == 0) { ++offset; continue; }
                for (std::uint8_t i = 0; i < instruction.operand_count; ++i)
                {
                    const auto& operand = operands[i];
                    if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && (operand.imm.is_relative || operand.imm.is_address) && operand.imm.size)
                    {
                        const std::size_t begin = offset + operand.imm.offset, count = operand.imm.size / 8; for (std::size_t j = 0; j < count && begin + j < wildcard.size(); ++j) wildcard[begin + j] = true;
                    }
                    else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_RIP && operand.mem.disp.size)
                    {
                        const std::size_t begin = offset + operand.mem.disp.offset, count = operand.mem.disp.size / 8; for (std::size_t j = 0; j < count && begin + j < wildcard.size(); ++j) wildcard[begin + j] = true;
                    }
                }
                offset += instruction.length;
            }
        }
#else
        (void)wildcardRelocations;
#endif
        std::ostringstream out;
        for (std::size_t i = 0; i < bytes.size(); ++i) { if (i) out << ' '; if (wildcard[i]) out << "??"; else out << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[i]); }
        error.clear(); return out.str();
    }
}
