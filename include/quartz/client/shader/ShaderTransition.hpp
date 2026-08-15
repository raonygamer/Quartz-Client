#pragma once
#include "quartz/client/shader/ShaderFramebuffer.hpp"

namespace quartz::client
{
    struct ShaderTransitionState
    {
        ShaderFramebuffer Previous;
        std::array<Color32, MatrixSize> Frame{};
        bool Active = false;
        double StartedAt = 0.0;
        float Duration = 0.0f;

        void cancel() noexcept
        {
            Active = false;
            StartedAt = 0.0;
            Duration = 0.0f;
            Previous.shutdown();
        }
    };

}
