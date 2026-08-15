#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class OpcodeEditorPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "opcode-editor"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Opcode Pattern Editor"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Runtime; }
        [[nodiscard]] PagePresentation presentation() const noexcept override { return PagePresentation::Standalone; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
