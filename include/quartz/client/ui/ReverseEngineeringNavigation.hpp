#pragma once
#include <cstdint>
#include <sys/types.h>

namespace quartz::client::ui
{
    void requestMemoryInspector(pid_t pid, std::uintptr_t address) noexcept;
    bool consumeMemoryInspectorFocus() noexcept;
}
