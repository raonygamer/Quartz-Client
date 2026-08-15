#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class JavaScriptPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "javascript"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "JavaScript"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Runtime; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
