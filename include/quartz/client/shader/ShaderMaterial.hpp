#pragma once
#include "quartz/client/Functions.hpp"

namespace quartz::client
{
    struct HSV
    {
        float H = 0.0f;
        float S = 0.0f;
        float V = 0.0f;
    };

    struct ShaderUniformMetadata
    {
        bool Explicit = false;
        bool Hidden = false;
        bool Color = false;
        bool HasDefault = false;
        bool HasMin = false;
        bool HasMax = false;
        bool HasStep = false;
        std::string Label;
        std::string Id;
        float Min = 0.0f;
        float Max = 1.0f;
        float Step = 0.01f;
        std::array<float, 4> Default{};
    };

    struct ShaderMaterialParameter
    {
        std::string Name;
        std::string Label;
        std::string PersistenceKey;
        GLenum Type = 0;
        GLint Location = -1;
        int Components = 1;
        bool Integer = false;
        bool Boolean = false;
        bool Color = false;
        bool HasMin = false;
        bool HasMax = false;
        float Min = 0.0f;
        float Max = 1.0f;
        float Step = 0.01f;
        std::array<float, 4> FloatValue{};
        std::array<float, 4> FloatDefault{};
        std::array<int, 4> IntValue{};
        std::array<int, 4> IntDefault{};
    };

}
