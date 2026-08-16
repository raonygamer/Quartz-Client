#include "quartz/client/ui/ThemeBackground.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <glad/gl.h>
#include <imgui.h>
#include <stb_image.h>

extern "C"
{
    extern const unsigned char quartz_theme_deviluke_start[];
    extern const unsigned char quartz_theme_deviluke_end[];
    extern const unsigned char quartz_theme_kurosaki_start[];
    extern const unsigned char quartz_theme_kurosaki_end[];
    extern const unsigned char quartz_theme_yami_start[];
    extern const unsigned char quartz_theme_yami_end[];
    extern const unsigned char quartz_theme_kirisaki_start[];
    extern const unsigned char quartz_theme_kirisaki_end[];
}

namespace quartz::client::ui
{
    namespace
    {
        struct EmbeddedImage
        {
            const unsigned char* Begin = nullptr;
            const unsigned char* End = nullptr;
        };

        struct ThemeTexture
        {
            GLuint Id = 0;
            int Width = 0;
            int Height = 0;
            bool Attempted = false;
        };

        std::array<ThemeTexture, static_cast<std::size_t>(Theme::Count)> Textures{};

        float silhouetteAlpha(const Theme theme) noexcept
        {
            switch (theme)
            {
            case Theme::KurosakiPink: return 0.128f;
            case Theme::YamiGolden: return 0.105f;
            case Theme::DevilukePink: case Theme::KirisakiPurple: return 0.115f;
            default: return 0.0f;
            }
        }

        EmbeddedImage imageForTheme(const Theme theme) noexcept
        {
            switch (theme)
            {
            case Theme::DevilukePink: return {quartz_theme_deviluke_start, quartz_theme_deviluke_end};
            case Theme::KurosakiPink: return {quartz_theme_kurosaki_start, quartz_theme_kurosaki_end};
            case Theme::YamiGolden: return {quartz_theme_yami_start, quartz_theme_yami_end};
            case Theme::KirisakiPurple: return {quartz_theme_kirisaki_start, quartz_theme_kirisaki_end};
            default: return {};
            }
        }

        ThemeTexture* textureForTheme(const Theme theme) noexcept
        {
            if (!suspiciousTheme(theme)) return nullptr;
            const int themeIndex = static_cast<int>(theme);
            if (themeIndex < 0 || themeIndex >= static_cast<int>(Theme::Count)) return nullptr;
            ThemeTexture& texture = Textures[static_cast<std::size_t>(themeIndex)];
            if (texture.Attempted) return texture.Id != 0 ? &texture : nullptr;
            texture.Attempted = true;

            const EmbeddedImage image = imageForTheme(theme);
            if (!image.Begin || !image.End || image.End <= image.Begin) return nullptr;
            const std::ptrdiff_t byteCount = image.End - image.Begin;
            if (byteCount <= 0 || byteCount > std::numeric_limits<int>::max()) return nullptr;

            int width = 0, height = 0, channels = 0;
            stbi_uc* pixels = stbi_load_from_memory(image.Begin, static_cast<int>(byteCount), &width, &height, &channels, STBI_rgb_alpha);
            if (!pixels || width <= 0 || height <= 0) { if (pixels) stbi_image_free(pixels); return nullptr; }

            GLint previousTexture = 0, previousUnpackAlignment = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpackAlignment);
            glGenTextures(1, &texture.Id);
            glBindTexture(GL_TEXTURE_2D, texture.Id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            glPixelStorei(GL_UNPACK_ALIGNMENT, previousUnpackAlignment);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
            stbi_image_free(pixels);

            texture.Width = width;
            texture.Height = height;
            return texture.Id != 0 ? &texture : nullptr;
        }
    }

    void drawThemeBackground(const Theme theme, ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const float opacity) noexcept
    {
        if (!drawList) return;
        ThemeTexture* texture = textureForTheme(theme);
        if (!texture || texture->Width <= 0 || texture->Height <= 0) return;
        const float areaWidth = std::max(0.0f, max.x - min.x), areaHeight = std::max(0.0f, max.y - min.y);
        if (areaWidth <= 1.0f || areaHeight <= 1.0f) return;

        const float aspect = static_cast<float>(texture->Width) / static_cast<float>(texture->Height);
        float drawHeight = areaHeight * 0.94f;
        float drawWidth = drawHeight * aspect;
        const float maxWidth = areaWidth * 0.72f;
        if (drawWidth > maxWidth) { drawWidth = maxWidth; drawHeight = drawWidth / aspect; }

        const float rightBleed = drawWidth * 0.035f;
        const float bottomBleed = drawHeight * 0.025f;
        const ImVec2 imageMax{max.x + rightBleed, max.y + bottomBleed};
        const ImVec2 imageMin{imageMax.x - drawWidth, imageMax.y - drawHeight};
        const float alpha = std::clamp(silhouetteAlpha(theme) * opacity, 0.0f, 1.0f);
        const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(alpha * 255.0f));
        drawList->AddImage(ImTextureRef(static_cast<ImTextureID>(texture->Id)), imageMin, imageMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
    }

    void drawThemeBackground(const int theme, ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const float opacity) noexcept
    {
        drawThemeBackground(static_cast<Theme>(std::clamp(theme, 0, static_cast<int>(Theme::Count) - 1)), drawList, min, max, opacity);
    }
}
