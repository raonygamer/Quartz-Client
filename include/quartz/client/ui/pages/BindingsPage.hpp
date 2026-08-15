#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class BindingsPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "bindings"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "RE / Bindings"; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
