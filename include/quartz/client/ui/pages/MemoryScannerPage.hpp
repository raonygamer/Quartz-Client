#pragma once
#include "quartz/client/ui/Page.hpp"
#include "quartz/client/native/MemoryScanner.hpp"
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
