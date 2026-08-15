#pragma once
#include <cstdint>

namespace quartz::client
{
    class RuntimeBindingEngine;
    struct RuntimeBinding;
    struct RuntimeSignalContext;

    bool runtimeEvaluateScriptBinding(RuntimeBindingEngine& engine, RuntimeBinding& binding, const RuntimeSignalContext& context, float& output);
    void runtimeResetScriptBinding(std::uint64_t bindingId) noexcept;
}
