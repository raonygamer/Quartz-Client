#pragma once
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
    void setLanguage(Language language) noexcept;
    void loadLanguagePreference() noexcept;
    void saveLanguagePreference() noexcept;
    bool drawLanguageSelector();
}
