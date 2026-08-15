#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace quartz::client
{
    class RuntimeBindingEngine;
    struct RuntimeBinding;
    struct RuntimeScript;
    struct RuntimeSignalContext;
    struct RuntimeControlOutput;
    class ShaderFramebuffer;

    bool runtimeEvaluateScriptBinding(RuntimeBindingEngine& engine, RuntimeBinding& binding, const RuntimeSignalContext& context, float& output);
    void runtimeResetScriptBinding(std::uint64_t bindingId) noexcept;
    std::string_view runtimeQuickJSTypeDeclarations() noexcept;
    std::filesystem::path runtimeQuickJSTypeDeclarationsPath();
    bool runtimeSaveQuickJSTypeDeclarations(std::string& error);
    RuntimeControlOutput runtimeEvaluateWorkspaceScripts(RuntimeBindingEngine& engine, const RuntimeSignalContext& context, ShaderFramebuffer& shader);
    void runtimeResetWorkspaceScript(std::uint64_t scriptId) noexcept;
    void runtimeReloadAllWorkspaceScripts() noexcept;
    std::filesystem::path runtimeQuickJSScriptDirectory();
}
