#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace quartz::client
{
    class RuntimeBindingEngine;
    class JavaScriptRuntime;
    class EvdevKeyboard;
    struct RuntimeBinding;
    struct RuntimeSignalContext;
    struct RuntimeControlOutput;
    class ShaderFramebuffer;

    bool runtimeEvaluateScriptBinding(RuntimeBindingEngine& engine, RuntimeBinding& binding, const RuntimeSignalContext& context, float& output);
    void runtimeResetScriptBinding(std::uint64_t bindingId) noexcept;

    std::string_view runtimeQuickJSTypeDeclarations() noexcept;
    std::filesystem::path runtimeQuickJSTypeDeclarationsPath();
    bool runtimeSaveQuickJSTypeDeclarations(std::string& error);

    const RuntimeControlOutput& runtimeEvaluateWorkspaceScripts(JavaScriptRuntime& javascript, const RuntimeSignalContext& context, ShaderFramebuffer& shader, const EvdevKeyboard& keyboard);
    void runtimeResetWorkspaceScript(std::uint64_t scriptId, std::string_view reason = "reload") noexcept;
    void runtimeReloadAllWorkspaceScripts(std::string_view reason = "reload") noexcept;
    std::filesystem::path runtimeQuickJSScriptDirectory();
}
