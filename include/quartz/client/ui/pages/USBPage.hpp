#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class USBPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "usb"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "USB"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Device; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
