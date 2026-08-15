#pragma once
#include "quartz/client/ui/Page.hpp"
#include "quartz/client/native/MemoryScanner.hpp"
#include <cstdio>
#include <optional>
#include <unordered_map>

namespace quartz::client::ui
{
    class MemoryScannerPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "memory-scanner"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Memory Scanner"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Runtime; }
        void render(PageContext& context, PageManager& manager) override;
        bool addWatch(const pid_t pid, const std::uintptr_t address, const MemoryScanValueType type, std::size_t width = 0)
        {
            if (pid <= 0 || address == 0) return false;
            if (width == 0)
            {
                switch (type)
                {
                case MemoryScanValueType::U8: case MemoryScanValueType::I8: case MemoryScanValueType::Bool: width = 1; break;
                case MemoryScanValueType::U16: case MemoryScanValueType::I16: width = 2; break;
                case MemoryScanValueType::U32: case MemoryScanValueType::I32: case MemoryScanValueType::Float: width = 4; break;
                case MemoryScanValueType::U64: case MemoryScanValueType::I64: case MemoryScanValueType::Double: case MemoryScanValueType::Pointer: width = 8; break;
                case MemoryScanValueType::Utf8String: case MemoryScanValueType::Utf16String: case MemoryScanValueType::ByteArray: width = 16; break;
                }
            }
            for (const auto& watch : _watchList) if (watch.Pid == pid && watch.Address == address && watch.Type == type) return false;
            WatchedValue watch; watch.Id = _nextWatchId++; watch.Pid = pid; watch.Address = address; watch.Type = type; watch.Width = std::max<std::size_t>(width, 1); watch.Value = "<pending>"; watch.PreviousValue = "-"; std::snprintf(watch.AddressText.data(), watch.AddressText.size(), "0x%llX", static_cast<unsigned long long>(address)); _watchList.emplace_back(std::move(watch)); _status = "added manual watch"; return true;
        }
    private:
        struct LiveValue
        {
            std::string Value;
            std::optional<long double> Numeric;
            double LastRefresh = 0.0;
        };

        struct WatchedValue
        {
            std::uint64_t Id = 0;
            pid_t Pid = 0;
            std::uintptr_t Address = 0;
            MemoryScanValueType Type = MemoryScanValueType::I32;
            std::size_t Width = 4;
            std::string Value;
            std::string PreviousValue;
            std::optional<long double> Numeric;
            bool Frozen = false;
            std::string FrozenValue;
            double LastRefresh = 0.0;
            double LastFreeze = 0.0;
            std::array<char, 32> AddressText{};
            std::array<char, 256> ValueText{};
        };

        MemoryScanner _scanner;
        std::vector<RuntimeProcessInfo> _processes;
        std::unordered_map<std::uintptr_t, LiveValue> _liveValues;
        std::vector<WatchedValue> _watchList;
        std::uint64_t _nextWatchId = 1;
        pid_t _pid = 0;
        int _type = static_cast<int>(MemoryScanValueType::I32);
        int _comparison = static_cast<int>(MemoryScanComparison::Exact);
        std::array<char, 256> _valueA{"100"};
        std::array<char, 256> _valueB{};
        std::array<char, 32> _rangeStart{};
        std::array<char, 32> _rangeEnd{};
        std::array<char, 256> _scanWriteValue{};
        std::uintptr_t _scanWriteAddress = 0;
        bool _writableOnly = true;
        bool _executableOnly = false;
        bool _aligned = true;
        bool _caseSensitive = true;
        bool _wildcardRelocations = true;
        float _refreshHz = 1.0f;
        float _freezeHz = 30.0f;
        std::string _status;
        std::string _derivedPattern;
    };
}
