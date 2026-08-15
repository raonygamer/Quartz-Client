#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class ValueBankPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "value-bank"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Value Bank (deprecated)"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Runtime; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
