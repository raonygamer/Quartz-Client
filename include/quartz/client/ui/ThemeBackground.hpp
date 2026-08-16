#pragma once
#include "quartz/client/ui/Theme.hpp"

struct ImDrawList;
struct ImVec2;

namespace quartz::client::ui
{
    void drawThemeBackground(Theme theme, ImDrawList* drawList, const ImVec2& min, const ImVec2& max) noexcept;
    void drawThemeBackground(int theme, ImDrawList* drawList, const ImVec2& min, const ImVec2& max) noexcept;
}
