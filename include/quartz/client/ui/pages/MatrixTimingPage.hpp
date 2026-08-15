#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class MatrixTimingPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "matrix-timing"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Matrix timing"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Diagnostics; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
