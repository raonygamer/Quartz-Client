#pragma once
#include "quartz/client/Functions.hpp"

namespace quartz::client
{
    enum class ViewPage : std::uint8_t
    {
        Main,
        ShaderEditor
    };

    struct ShaderEditorState
    {
        TextEditor Vertex;
        TextEditor Fragment;
        int ActiveStage = 0;
        bool Initialized = false;
        bool ZoomInWasDown = false;
        bool ZoomOutWasDown = false;
        bool ZoomResetWasDown = false;
        std::filesystem::path ExternalVertexPath;
        std::filesystem::path ExternalFragmentPath;
        std::filesystem::file_time_type ExternalVertexWriteTime{};
        std::filesystem::file_time_type ExternalFragmentWriteTime{};
        bool HotReloadExternal = false;
        double NextExternalPoll = 0.0;
        std::string ExternalStatus;
    };

}
