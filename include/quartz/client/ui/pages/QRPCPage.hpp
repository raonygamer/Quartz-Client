#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class QRPCPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "qrpc"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "QRPC Inspector"; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
