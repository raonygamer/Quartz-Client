#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class DevicePage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "device"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Device"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Device; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
