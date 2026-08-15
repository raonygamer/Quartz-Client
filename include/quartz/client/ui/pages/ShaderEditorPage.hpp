#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class ShaderEditorPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "shader-editor"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Shader Editor"; }
        [[nodiscard]] PagePresentation presentation() const noexcept override { return PagePresentation::Standalone; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
