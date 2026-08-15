#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class ControlsPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "controls"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Controls (deprecated)"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Runtime; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
