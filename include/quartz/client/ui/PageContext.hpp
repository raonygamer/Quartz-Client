#pragma once
#include "quartz/client/Model.hpp"

namespace quartz::client::ui
{
    struct PageContext
    {
        RawUSB& usb;
        AudioSpectrum& audio;
        MediaColorProvider& mediaColor;
        const EvdevKeyboard& keyboardInput;
        ShaderFramebuffer& shaderFramebuffer;
        ShaderTransitionState& shaderTransition;
        ShaderEditorState& shaderEditor;
        std::array<char, ShaderSourceCapacity>& vertexShaderSource;
        std::array<char, ShaderSourceCapacity>& fragmentShaderSource;
        std::array<char, ShaderPathCapacity>& vertexLoadPath;
        std::array<char, ShaderPathCapacity>& fragmentLoadPath;
        VisualizerSettings& settings;
        const std::array<float, FFTSize>& analysisBands;
        const std::array<float, Columns>& mappedBands;
        const std::array<float, Columns>& smoothedBands;
        const std::array<Color32, MatrixSize>& framebuffer;
        SharedDeviceState& deviceState;
        RuntimeBindingEngine& runtimeBindings;
        RuntimeTelemetry& runtimeTelemetry;
        const AutoGainState& autoGain;
        const AudioLevelSnapshot& audioLevel;
        const ReactiveKeyState& reactiveKeys;
        const RuntimeInputAnalytics& inputAnalytics;
        const RuntimeRGBAnalytics& rgbAnalytics;
        std::uint64_t sentFrames = 0;
        std::uint64_t droppedFrames = 0;
        float appCpuUsage = 0.0f;
        bool scrollLockActive = false;
        bool capsLockActive = false;
    };
}
