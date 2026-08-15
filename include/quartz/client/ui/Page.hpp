#pragma once
#include <string_view>

namespace quartz::client::ui
{
    struct PageContext;
    class PageManager;

    enum class PagePresentation
    {
        Tab,
        Standalone
    };

    class Page
    {
    public:
        virtual ~Page() = default;
        [[nodiscard]] virtual std::string_view id() const noexcept = 0;
        [[nodiscard]] virtual std::string_view title() const noexcept = 0;
        [[nodiscard]] virtual PagePresentation presentation() const noexcept { return PagePresentation::Tab; }
        virtual void render(PageContext& context, PageManager& manager) = 0;
    };
}
