#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace quartz::client
{
    class RuntimeBindingEngine;
    struct RuntimeBinding;
    struct RuntimeSignalContext;

    bool runtimeEvaluateScriptBinding(RuntimeBindingEngine& engine, RuntimeBinding& binding, const RuntimeSignalContext& context, float& output);
    void runtimeResetScriptBinding(std::uint64_t bindingId) noexcept;
    std::string_view runtimeQuickJSTypeDeclarations() noexcept;
    std::filesystem::path runtimeQuickJSTypeDeclarationsPath();
    bool runtimeSaveQuickJSTypeDeclarations(std::string& error);
}
