#pragma once
#include "quartz/client/ui/Page.hpp"
#include "quartz/client/native/MemoryScanner.hpp"

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
        MemoryScanner _scanner;
        std::vector<RuntimeProcessInfo> _processes;
        pid_t _pid = 0;
        int _type = static_cast<int>(MemoryScanValueType::I32);
        int _comparison = static_cast<int>(MemoryScanComparison::Exact);
        std::array<char, 256> _valueA{"100"};
        std::array<char, 256> _valueB{};
        std::array<char, 32> _rangeStart{};
        std::array<char, 32> _rangeEnd{};
        bool _writableOnly = true;
        bool _executableOnly = false;
        bool _aligned = true;
        bool _caseSensitive = true;
        bool _wildcardRelocations = true;
        std::string _status;
        std::string _derivedPattern;
    };
}
