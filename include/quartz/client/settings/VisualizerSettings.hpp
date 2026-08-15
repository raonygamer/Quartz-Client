#pragma once
#include "quartz/client/Functions.hpp"

namespace quartz::client
{
    struct VisualizerSettings
    {
        bool Enabled = true;
        bool SendFramebuffer = true;
        bool MediaArtworkColor = true;
        bool ForceFullRow = true;
        bool ShowFramebuffer = true;
        bool ShowAnalysisSpectrum = true;
        bool ShowMappedSpectrum = true;
        bool LimitMainLoop = true;
        bool AutoReconnect = true;
        bool ShaderRecompileOnChange = false;
        bool ShaderKeyStateUniforms = true;
        bool ShaderCapsLockColorEnabled = true;
        bool ShaderScrollLockColorEnabled = true;
        int ShaderFramebufferWidth = DefaultShaderWidth;
        int ShaderFramebufferHeight = DefaultShaderHeight;
        int FrameRate = 240;
        int AnalysisBandCount = 512;
        int BassColumns = 3;
        int BassEndBand = 16;
        int FullRow = 5;
        float OverallGain = 1.62f;
        bool AutomaticOverallGain = false;
        float AutoGainBaseline = 1.62f;
        float AutoGainTargetRms = 0.10f;
        float AutoGainAdaptation = 0.35f;
        float AutoGainMinCorrection = 0.45f;
        float AutoGainMaxCorrection = 3.00f;
        float AutoGainSilenceGate = 0.0025f;
        float GlobalBrightness = 1.0f;
        float LiveOutputInterpolation = 0.35f;
        float WaveSpeed = 0.40f;
        float FeatherRows = 2.5f;
        float Saturation = 2.0f;
        float AttackSpeed = 3.5f;
        float ReleaseSpeed = 40.5f;
        float BassActivationThreshold = 0.65f;
        float BassMaxBoost = 1.68f;
        float ColorTransitionSpeed = 1.5f;
        float MediaColorBlend = 1.0f;
        float MinFrequency = 50.0f;
        float MaxFrequency = 16000.0f;
        float MinDb = -72.0f;
        float MaxDb = -15.0f;
        float StatisticsInterval = 0.20f;
        float MediaPollInterval = 0.50f;
        float ShaderEditorZoom = 1.0f;
        float ShaderTransitionSeconds = 0.35f;
        std::array<float, 3> SolidColor{1.0f, 0.0f, 0.0f};
        std::array<float, 3> ShaderCapsLockColor{0.10f, 0.80f, 1.00f};
        std::array<float, 3> ShaderScrollLockColor{1.00f, 0.20f, 0.55f};
        std::array<float, Columns> ColumnGain{0.55f, 0.58f, 0.56f, 0.72f, 0.78f, 0.72f, 0.81f, 0.74f, 0.77f, 0.84f, 0.84f, 0.86f, 0.99f, 0.99f, 0.99f, 0.92f};
        char AudioSource[128] = "easyeffects_sink.monitor";
        int BaseColorMode = 0;
        int ShaderDownsampleMode = 0;
        int ShaderPresetIndex = 1;
        std::string ShaderId = "builtin.rainbow_equalizer";
    };

}
