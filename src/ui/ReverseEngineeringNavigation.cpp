#include "quartz/client/ui/ReverseEngineeringNavigation.hpp"
#include "quartz/client/ui/MemoryInspector.hpp"

namespace quartz::client::ui
{
    namespace { bool InspectorFocusRequested = false; }

    void requestMemoryInspector(const pid_t pid, const std::uintptr_t address) noexcept
    {
        auto& state = runtimeMemoryInspectorState();
        state.Pid = pid;
        state.Address = address;
        InspectorFocusRequested = true;
        refreshEnhancedRuntimeMemoryInspector(state);
    }

    bool consumeMemoryInspectorFocus() noexcept
    {
        const bool requested = InspectorFocusRequested;
        InspectorFocusRequested = false;
        return requested;
    }
}
