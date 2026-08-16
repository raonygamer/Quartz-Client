#pragma once
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include "quartz/client/ui/UIState.hpp"

namespace quartz::client
{
    void refreshEnhancedRuntimeMemoryInspector(RuntimeMemoryInspectorState& state);
    void drawEnhancedRuntimeMemoryInspector(RuntimeMemoryInspectorState& state);
}
