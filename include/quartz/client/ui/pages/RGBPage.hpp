#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class RGBPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "rgb"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "RGB Profiler"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Visual; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
