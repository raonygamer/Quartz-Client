#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class ProfilesPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "profiles"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Profiles"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Scripting; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
