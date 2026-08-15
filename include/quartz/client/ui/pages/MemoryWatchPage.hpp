#pragma once
#include "quartz/client/ui/Page.hpp"
#include "quartz/client/native/MemoryWatch.hpp"

namespace quartz::client::ui
{
    class MemoryWatchPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "memory-watch"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Memory Watch"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Runtime; }
        void render(PageContext& context, PageManager& manager) override;
        void setTarget(const pid_t pid, const std::uintptr_t address, const std::size_t width) noexcept
        {
            _pid = pid;
            std::snprintf(_address.data(), _address.size(), "0x%llX", static_cast<unsigned long long>(address));
            _size = width <= 1 ? 0 : width <= 2 ? 1 : width <= 4 ? 2 : 3;
        }
    private:
        MemoryWatch _watch;
        std::vector<RuntimeProcessInfo> _processes;
        pid_t _pid = 0;
        std::array<char, 32> _address{};
        int _size = 2;
        int _access = static_cast<int>(MemoryWatchAccess::Write);
        int _maxHits = 64;
        std::string _status;
    };
}
