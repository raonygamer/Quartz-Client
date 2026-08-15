#pragma once
#include "quartz/client/shader/ShaderEditor.hpp"
#include "quartz/client/shader/ShaderFramebuffer.hpp"
#include "quartz/client/settings/VisualizerSettings.hpp"

namespace quartz::client
{
    bool loadExternalShaderFile(ShaderEditorState& editor, ShaderFramebuffer& framebuffer, std::array<char, ShaderSourceCapacity>& vertexSource, std::array<char, ShaderSourceCapacity>& fragmentSource, VisualizerSettings& settings, const std::filesystem::path& path, bool fragment);
    void clearExternalShaderFile(ShaderEditorState& editor, bool fragment) noexcept;
    bool pollExternalShaderHotReload(ShaderEditorState& editor, ShaderFramebuffer& framebuffer, std::array<char, ShaderSourceCapacity>& vertexSource, std::array<char, ShaderSourceCapacity>& fragmentSource, VisualizerSettings& settings, double now);
}
