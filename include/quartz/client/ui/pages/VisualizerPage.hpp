#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class VisualizerPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "visualizer"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Visualizer"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Visual; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
