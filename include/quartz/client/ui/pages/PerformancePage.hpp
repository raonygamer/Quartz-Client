#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class PerformancePage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "performance"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Performance"; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
