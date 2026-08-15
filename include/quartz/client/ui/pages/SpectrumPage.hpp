#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class SpectrumPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "spectrum"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Spectrum"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Visual; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
