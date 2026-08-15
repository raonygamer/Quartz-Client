#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class TimelinePage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "timeline"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Timeline"; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
