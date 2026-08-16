#pragma once
#include <cstddef>
#include <string_view>

namespace quartz::client::ui::i18n
{
    enum class Language : int
    {
        English,
        PortugueseBrazil,
        Count
    };

    [[nodiscard]] Language language() noexcept;
    [[nodiscard]] const char* languageName(Language language) noexcept;
    [[nodiscard]] const char* tr(std::string_view key) noexcept;
    [[nodiscard]] const char* trExtension(std::string_view key) noexcept;
    template<std::size_t N> [[nodiscard]] const char* tr(const char (&key)[N]) noexcept
    {
        const std::string_view view(key,N-1); if (const char* translated=trExtension(view)) return translated; return tr(view);
    }
    void setLanguage(Language language) noexcept;
    void loadLanguagePreference() noexcept;
    void saveLanguagePreference() noexcept;
    bool drawLanguageSelector();
}
