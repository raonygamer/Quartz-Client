#pragma once
#include <quickjs.h>
#include <chrono>

namespace quartz::client
{
    class RuntimeBindingEngine;
    struct RuntimeBinding;
    struct RuntimeSignalContext;

    struct RuntimeQuickJSDeadline
    {
        std::chrono::steady_clock::time_point Deadline{};
        bool Active = false;
        bool Interrupted = false;
    };

    struct RuntimeQuickJSContext
    {
        RuntimeBindingEngine* Engine = nullptr;
        RuntimeBinding* Binding = nullptr;
        const RuntimeSignalContext* SignalContext = nullptr;
        RuntimeQuickJSDeadline* Execution = nullptr;
    };

    inline bool runtimeQuickJSDeadlineExpired(RuntimeQuickJSContext& context) noexcept
    {
        if (!context.Execution || !context.Execution->Active || std::chrono::steady_clock::now() < context.Execution->Deadline) return false;
        context.Execution->Interrupted = true;
        return true;
    }

    void runtimeInstallQuickJSLowLevelApi(JSContext* ctx, JSValueConst api);
}
