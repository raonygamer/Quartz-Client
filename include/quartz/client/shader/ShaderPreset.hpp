#pragma once
#include "quartz/client/Common.hpp"

namespace quartz::client
{
    struct ShaderPreset
    {
        std::string Name;
        std::string FragmentSource;
        std::string Id;
        std::filesystem::path SourcePath;
        bool BuiltIn = true;
    };
}
