#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class NativePage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "native"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Workspace"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::ReverseEngineering; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
