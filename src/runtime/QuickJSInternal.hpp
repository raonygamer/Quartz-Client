#pragma once
#include <quickjs.h>
#include <chrono>

namespace quartz::client
{
    class RuntimeBindingEngine;
    class JavaScriptRuntime;
    class EvdevKeyboard;
    struct RuntimeBinding;
    struct RuntimeScript;
    struct RuntimeSignalContext;
    struct RuntimeControlOutput;
    class ShaderFramebuffer;

    struct RuntimeQuickJSDeadline
    {
        std::chrono::steady_clock::time_point Deadline{};
        bool Active = false;
        bool Interrupted = false;
    };

    struct RuntimeQuickJSContext
    {
        RuntimeBindingEngine* Engine = nullptr;
        JavaScriptRuntime* JavaScript = nullptr;
        RuntimeBinding* Binding = nullptr;
        RuntimeScript* Script = nullptr;
        const RuntimeSignalContext* SignalContext = nullptr;
        const EvdevKeyboard* Keyboard = nullptr;
        RuntimeQuickJSDeadline* Execution = nullptr;
        ShaderFramebuffer* Shader = nullptr;
        RuntimeControlOutput* Output = nullptr;
        bool AllowGraphMutation = false;
        bool LegacyBridge = false;
    };

    inline bool runtimeQuickJSDeadlineExpired(RuntimeQuickJSContext& context) noexcept
    {
        if (!context.Execution || !context.Execution->Active || std::chrono::steady_clock::now() < context.Execution->Deadline) return false;
        context.Execution->Interrupted = true;
        return true;
    }

    void runtimeInstallQuickJSLowLevelApi(JSContext* ctx, JSValueConst api);
    void runtimeInstallQuickJSGraphApi(JSContext* ctx, JSValueConst api);
    void runtimeInstallQuickJSAsyncSignatureApi(JSContext* ctx, JSValueConst api);
    void runtimeRefreshQuickJSSignatureScans(RuntimeScript& script) noexcept;
    void runtimeCancelQuickJSSignatureScans(RuntimeScript& script) noexcept;
}
