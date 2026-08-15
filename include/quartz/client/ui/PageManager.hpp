#pragma once
#include "Page.hpp"
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace quartz::client::ui
{
    class PageManager
    {
    public:
        PageManager() = default;
        PageManager(PageManager&&) noexcept = default;
        PageManager& operator=(PageManager&&) noexcept = default;
        PageManager(const PageManager&) = delete;
        PageManager& operator=(const PageManager&) = delete;

        template<typename T, typename... Args>
        T& add(Args&&... args)
        {
            static_assert(std::is_base_of_v<Page, T>);
            auto page = std::make_unique<T>(std::forward<Args>(args)...);
            T& result = *page;
            _pages.emplace_back(std::move(page));
            return result;
        }

        [[nodiscard]] Page* find(std::string_view id) noexcept;
        [[nodiscard]] const Page* find(std::string_view id) const noexcept;
        [[nodiscard]] bool hasStandalonePage() const noexcept { return !_standaloneId.empty(); }
        [[nodiscard]] std::string_view standalonePageId() const noexcept { return _standaloneId; }

        bool open(std::string_view id);
        void closeStandalone() noexcept { _standaloneId.clear(); }
        void render(PageContext& context);

    private:
        std::vector<std::unique_ptr<Page>> _pages;
        std::string _standaloneId;
        std::string _activePageId;
    };

    [[nodiscard]] PageManager createDefaultPageManager();
}
