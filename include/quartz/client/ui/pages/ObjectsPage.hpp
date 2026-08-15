#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class ObjectsPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "objects"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Objects & Pointers"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Runtime; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
