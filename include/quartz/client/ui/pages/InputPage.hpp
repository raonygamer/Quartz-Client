#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class InputPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "input"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Input Analyzer"; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
