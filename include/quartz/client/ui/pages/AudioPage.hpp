#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class AudioPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "audio"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Audio Lab"; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
