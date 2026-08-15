#pragma once
#include "quartz/client/Functions.hpp"
#include "quartz/client/native/NativeTypes.hpp"
#include "quartz/client/runtime/Profile.hpp"
#include "quartz/client/device/DeviceState.hpp"
#include "quartz/client/input/Input.hpp"
#include "quartz/client/audio/Audio.hpp"
#include "quartz/client/usb/USB.hpp"

namespace quartz::client
{
    enum class RuntimeSourceKind : int
    {
        Constant,
        Time,
        Audio,
        Media,
        Keyboard,
        RPC,
        Host,
        USB,
        RGB,
        NativeProcess,
        BindingStatus,
        Unbound,
        BindingValue,
        ShaderState,
        ControlStatus,
        Aggregate,
        MassCompare,
        NativeAddress,
        ObjectField,
        ObjectStatus,
        ValueBank,
        StringConstant,
        ProfileState
    };

    enum class RuntimeParameterSlot : int
    {
        Normalize,
        InputMin,
        InputMax,
        Invert,
        Scale,
        Offset,
        Clamp,
        OutputMin,
        OutputMax,
        SmoothingHz,
        UpdateHz,
        Count
    };

    enum class RuntimeControlCondition : int
    {
        Equal,
        NotEqual,
        Less,
        LessEqual,
        Greater,
        GreaterEqual,
        Between,
        Outside,
        RisingEdge,
        FallingEdge,
        OnChange,
        ChangedTo,
        ChangedFrom,
        BecomesTrue,
        BecomesFalse,
        StringEqual,
        StringNotEqual,
        StringContains
    };

    enum class RuntimeControlTarget : int
    {
        ActiveShader,
        BindingEnabled,
        GlobalBrightness,
        SendFramebuffer,
        BaseColorMode,
        MaterialParameter,
        BindingValue,
        ValueBank,
        ControlEnabled,
        BindingRefresh,
        BindingForceUpdate,
        BindingInvalidate,
        BindingResetState,
        BindingRetryRegisterCapture,
        BindingRescanPattern,
        BindingRebindProcess,
        BindingClearError
    };

    enum class RuntimeReferenceKind : int
    {
        Binding,
        Control
    };

    enum class RuntimeAggregateOperation : int
    {
        Sum,
        Average,
        Minimum,
        Maximum,
        Product,
        Count,
        CountTruthy,
        FractionTruthy,
        Any,
        All
    };

    enum class RuntimeCompareCondition : int
    {
        Equal,
        NotEqual,
        Less,
        LessEqual,
        Greater,
        GreaterEqual,
        Between,
        Outside
    };

    enum class RuntimeMassCompareResult : int
    {
        Any,
        All,
        None,
        Count,
        Fraction,
        FirstMatchIndex
    };

    enum class RuntimeObjectFieldType : int
    {
        U8, I8, U16, I16, U32, I32, U64, I64, Float, Double, Bool, Pointer,
        Filler1, Filler2, Filler4, Filler8, Filler16, Filler32, FillerCustom,
        CStringPointer, WStringPointer, FixedCString, FixedWString
    };

    enum class RuntimeObjectPacking : int
    {
        Natural,
        Pack1,
        Pack2,
        Pack4,
        Pack8,
        Pack16
    };

    enum class RuntimeObjectAlignment : int
    {
        Auto,
        Align1,
        Align2,
        Align4,
        Align8,
        Align16
    };

    enum class RuntimeBankValueType : int
    {
        Number, Integer, Boolean, String, Address
    };

    enum class RuntimeActionTarget : int
    {
        ActiveShader, BindingEnabled, GlobalBrightness, SendFramebuffer, BaseColorMode, MaterialParameter, BindingValue, ValueBank, ControlEnabled,
        BindingRefresh, BindingForceUpdate, BindingInvalidate, BindingResetState, BindingRetryRegisterCapture, BindingRescanPattern, BindingRebindProcess, BindingClearError
    };

    enum class RuntimeBindingOperation : int
    {
        Refresh, ForceUpdate, Invalidate, ResetState, RetryRegisterCapture, RescanPattern, RebindProcess, ClearError
    };

    enum class RuntimeActionValueMode : int
    {
        Constant, SourceValue, BindingValue, BankValue
    };

    enum class RuntimeActionWhen : int
    {
        WhileActive, OnTrigger, OnUpdate, OnChange, OnTruthy, OnFalsy
    };

    struct RuntimeAction
    {
        bool Enabled = true;
        RuntimeActionTarget Target = RuntimeActionTarget::ActiveShader;
        RuntimeActionValueMode ValueMode = RuntimeActionValueMode::Constant;
        RuntimeActionWhen When = RuntimeActionWhen::WhileActive;
        int ShaderPresetIndex = 1;
        char ShaderId[128]{};
        std::uint64_t TargetBindingId = 0;
        std::uint64_t TargetControlId = 0;
        std::uint64_t ValueBindingId = 0;
        std::uint64_t BankValueId = 0;
        std::uint64_t TargetBankValueId = 0;
        float Value = 1.0f;
        bool BoolValue = true;
        int TargetComponent = 0;
        char TargetId[128]{};
        char StringValue[256]{};
        float TransitionSeconds = 0.35f;
    };

    struct RuntimeValueBankEntry
    {
        std::uint64_t Id = 0;
        bool Enabled = true;
        char Name[64] = "Value";
        char Description[256]{};
        RuntimeBankValueType Type = RuntimeBankValueType::Number;
        float Number = 0.0f;
        std::int64_t Integer = 0;
        bool Boolean = false;
        char String[256]{};
        std::uintptr_t Address = 0;
        bool HasValue = true;
        bool ChangedThisFrame = false;
    };

    struct RuntimeParameterLink
    {
        bool Enabled = false;
        std::uint64_t BindingId = 0;
    };

    struct RuntimeSourceReference
    {
        RuntimeReferenceKind Kind = RuntimeReferenceKind::Binding;
        std::uint64_t Id = 0;
        int Signal = 0;
        float Weight = 1.0f;
        bool Enabled = true;
        bool UseOwnComparison = false;
        RuntimeCompareCondition CompareCondition = RuntimeCompareCondition::Greater;
        float CompareA = 0.5f;
        float CompareB = 1.0f;
        float CompareTolerance = 0.001f;
    };

    struct RuntimeObjectField
    {
        std::uint64_t Id = 0;
        bool Enabled = true;
        char Name[64] = "Field";
        RuntimeObjectFieldType Type = RuntimeObjectFieldType::Float;
        RuntimeObjectAlignment Alignment = RuntimeObjectAlignment::Auto;
        bool ManualOffset = false;
        int Offset = 0;
        int CustomFillerBytes = 1;
        int StringMaxLength = 256;
        int FixedElementCount = 32;
    };

    struct RuntimeObjectDescriptor
    {
        std::uint64_t Id = 0;
        bool Enabled = true;
        char Name[64] = "Object";
        char Description[256]{};
        std::uint64_t BaseBindingId = 0; // v9 migration only; runtime assignment lives in RuntimeObjectPointer
        std::uint64_t ProcessBindingId = 0; // v9 migration only
        int BaseOffset = 0; // v9 migration only
        int Order = 0;
        char Group[64]{};
        RuntimeObjectPacking Packing = RuntimeObjectPacking::Natural;
        std::vector<RuntimeObjectField> Fields;
        std::size_t Size = 0; // derived model size only; runtime addresses live in RuntimeObjectPointer
    };

    struct RuntimeObjectPointer
    {
        std::uint64_t Id = 0;
        bool Enabled = true;
        int Order = 0;
        char Group[64]{};
        char Name[64] = "Pointer";
        std::uint64_t DescriptorId = 0;
        std::uint64_t BaseBindingId = 0;
        std::uint64_t ProcessBindingId = 0;
        int BaseOffset = 0;
        std::uintptr_t Address = 0;
        pid_t ProcessId = 0;
        bool Resolved = false;
        std::string Status;
        std::vector<std::string> Provenance;
    };

    struct RuntimeBinding
    {
        std::uint64_t Id = 0;
        bool Enabled = true;
        int Priority = 0;
        int Order = 0;
        char Group[64]{};
        RuntimeSourceKind Source = RuntimeSourceKind::Constant;
        int Signal = 0;
        float Constant = 0.5f;
        char Name[64] = "Binding";
        char TargetId[128] = "source.health";
        int TargetComponent = 0;

        int ProcessId = 0;
        bool AutoReattach = true;
        ProcessRebindMode RebindMode = ProcessRebindMode::NameExact;
        ProcessValueType ValueType = ProcessValueType::Float;
        char ProcessName[128]{};
        char ProcessRebindPattern[512]{};
        char ProcessSearch[256]{};
        char Module[192]{};
        ProcessAddressMode AddressMode = ProcessAddressMode::AddressChain;
        char Address[256] = "+0x0";
        char Signature[2048]{};
        RuntimeSignaturePatternKind SignaturePatternKind = RuntimeSignaturePatternKind::HexadecimalPattern;
        bool SignatureExecutableOnly = true;
        SignatureResultMode SignatureResolve = SignatureResultMode::MatchAddress;
        int SignatureResultOffset = 0;
        int SignatureInstructionSize = 7;
        RuntimeX64Register SignatureRegister = RuntimeX64Register::R15;
        int SignatureRegisterDisplacementOffset = 3;
        RuntimeDisplacementType SignatureDisplacementType = RuntimeDisplacementType::I32;
        int SignatureManualDisplacement = 0;
        float SignatureCaptureTimeoutSeconds = 10.0f;
        float SignatureRetrySeconds = 1.0f;
        double NextProcessSearch = 0.0;

        std::vector<std::uint8_t> SignatureBytes;
        std::vector<std::uint8_t> SignatureMasks;
        std::vector<RuntimeProcessRegion> SignatureRegions;
        std::size_t SignatureRegionIndex = 0;
        std::uintptr_t SignatureCursor = 0;
        std::uintptr_t SignatureResolvedAddress = 0;
        std::uintptr_t SignatureMatchAddress = 0;
        std::uintptr_t SignatureInstructionAddress = 0;
        std::uint64_t SignatureCapturedRegister = 0;
        std::intptr_t SignatureCapturedDisplacement = 0;
        std::shared_ptr<RuntimeRegisterCaptureState> SignatureRegisterCapture;
        std::uint64_t SignatureConfigHash = 0;
        pid_t SignatureScanPid = 0;
        double NextSignatureScan = 0.0;
        double NextRegisterCapture = 0.0;
        std::uint64_t SignatureScannedBytes = 0;
        std::uint64_t SignatureTotalBytes = 0;
        float SignatureProgress = 0.0f;
        std::string SignatureStatus;

        std::uint64_t StatusBindingId = 0;
        std::uint64_t ValueBindingId = 0;
        std::uint64_t ControlStatusId = 0;
        std::uint64_t BankValueId = 0;
        std::uint64_t ProfileId = 0;
        std::uint64_t StoreBankValueId = 0;
        bool StoreToBank = false;
        float UnboundValue = 0.0f;
        char StringConstant[256]{};
        RuntimeAggregateOperation AggregateOperation = RuntimeAggregateOperation::Average;
        RuntimeCompareCondition CompareCondition = RuntimeCompareCondition::Greater;
        RuntimeMassCompareResult CompareResult = RuntimeMassCompareResult::Any;
        float CompareA = 0.5f;
        float CompareB = 1.0f;
        float CompareTolerance = 0.001f;
        std::vector<RuntimeSourceReference> References;
        std::uint64_t ObjectId = 0;
        std::uint64_t ObjectPointerId = 0;
        std::uint64_t ObjectFieldId = 0;
        bool WriteMaterial = true;
        std::vector<RuntimeAction> Actions;

        bool Normalize = false;
        float InputMin = 0.0f;
        float InputMax = 1.0f;
        bool Invert = false;
        float Scale = 1.0f;
        float Offset = 0.0f;
        bool Clamp = true;
        float OutputMin = 0.0f;
        float OutputMax = 1.0f;
        float SmoothingHz = 8.0f;
        float UpdateHz = 60.0f;
        std::array<RuntimeParameterLink, static_cast<std::size_t>(RuntimeParameterSlot::Count)> ParameterLinks{};

        float RawValue = 0.0f;
        float Value = 0.0f;
        bool HasValue = false;
        std::string StringValue;
        bool HasString = false;
        float PreviousActionValue = 0.0f;
        std::string PreviousActionString;
        bool ActionPreviousInitialized = false;
        std::uintptr_t AddressValue = 0;
        bool HasAddress = false;
        std::vector<std::string> AddressProvenance;
        bool LastReadSucceeded = false;
        bool RuntimeEnabled = true;
        double LastSuccessTime = 0.0;
        double NextUpdate = 0.0;
        double LastUpdate = 0.0;
        std::string Error;
    };

    struct RuntimeControlRule
    {
        std::uint64_t Id = 0;
        bool Enabled = true;
        int Priority = 0;
        int Order = 0;
        char Group[64]{};
        char Name[64] = "Control";
        std::uint64_t SourceBindingId = 0;
        RuntimeControlCondition Condition = RuntimeControlCondition::Greater;
        float ValueA = 0.5f;
        float ValueB = 1.0f;
        float Tolerance = 0.001f;
        float Hysteresis = 0.0f;
        char StringCompare[256]{};
        bool FireOnFirstSample = true;
        RuntimeControlTarget Target = RuntimeControlTarget::ActiveShader;
        int ShaderPresetIndex = 1;
        char ShaderId[128]{};
        std::uint64_t TargetBindingId = 0;
        std::uint64_t TargetBankValueId = 0;
        std::uint64_t TargetControlId = 0;
        float TargetValue = 1.0f;
        bool TargetUseSourceValue = false;
        bool TargetBool = true;
        int TargetComponent = 0;
        char TargetId[128]{};
        float TransitionSeconds = 0.35f;
        std::vector<RuntimeAction> Actions;

        bool ConditionActive = false;
        bool PreviousInitialized = false;
        float PreviousValue = 0.0f;
        std::string PreviousString;
        bool PreviousStringInitialized = false;
        bool RuntimeEnabled = true;
        bool TriggeredThisFrame = false;
        double LastTriggerTime = 0.0;
        std::uint64_t TriggerCount = 0;
    };

    struct RuntimeControlOutput
    {
        std::optional<int> ShaderPresetIndex;
        std::optional<std::string> ShaderId;
        float ShaderTransitionSeconds = 0.35f;
        std::optional<float> GlobalBrightness;
        std::optional<bool> SendFramebuffer;
        std::optional<int> BaseColorMode;
    };

    struct RuntimeTimelineEvent
    {
        double Time = 0.0;
        std::string Category;
        std::string Text;
    };

    struct RuntimePacketRecord
    {
        static constexpr std::size_t CaptureBytes = 128;
        double Time = 0.0;
        bool Tx = false;
        std::uint16_t Type = 0;
        std::uint8_t Version = 0;
        std::uint32_t PacketId = 0;
        std::uint32_t ResponseFor = 0;
        std::uint32_t PayloadLength = 0;
        std::size_t Bytes = 0;
        std::size_t CapturedBytes = 0;
        std::array<std::byte, CaptureBytes> Data{};
    };

    struct RuntimeUSBRates
    {
        double TxKiB = 0.0;
        double RxKiB = 0.0;
        double TxTransfers = 0.0;
        double RxTransfers = 0.0;
    };

    struct RuntimeSignalContext
    {
        double Time = 0.0;
        float DeltaTime = 0.0f;
        AudioLevelSnapshot Audio{};
        const std::array<float, Columns>* MappedBands = nullptr;
        const std::array<float, Columns>* SmoothedBands = nullptr;
        std::optional<Color32> MediaColor;
        float MediaAmount = 0.0f;
        bool MediaPlaying = false;
        std::string MediaTitle;
        ReactiveKeyState Keys{};
        PerformanceSnapshot Performance{};
        bool HasPerformance = false;
        float AppCpu = 0.0f;
        bool USBConnected = false;
        USBStatsSnapshot USB{};
        RuntimeUSBRates USBRates{};
        const std::array<Color32, MatrixSize>* Framebuffer = nullptr;
        float EffectiveGain = 1.0f;
        float GainCorrection = 1.0f;
        int CurrentShaderPreset = 0;
        int PreviousShaderPreset = 0;
        std::string CurrentShaderId;
        std::string PreviousShaderId;
        bool ShaderTransitionActive = false;
        float ShaderTransitionProgress = 0.0f;
        int BaseColorMode = 0;
        float GlobalBrightness = 1.0f;
        bool SendFramebuffer = true;
        int ShaderFramebufferWidth = 0;
        int ShaderFramebufferHeight = 0;
    };

    struct RuntimeInputAnalytics
    {
        std::array<std::uint64_t, MatrixSize> PressCount{};
        std::array<double, MatrixSize> DownSince{};
        std::array<double, MatrixSize> LastDuration{};
        std::array<float, MatrixSize> Previous{};
        std::uint64_t TotalPresses = 0;
        double LongestPress = 0.0;

        void update(const ReactiveKeyState& keys, const double now) noexcept
        {
            for (std::size_t i = 0; i < MatrixSize; ++i)
            {
                const bool down = keys.Down[i] > 0.5f;
                const bool wasDown = Previous[i] > 0.5f;
                if (down && !wasDown)
                {
                    ++PressCount[i];
                    ++TotalPresses;
                    DownSince[i] = now;
                }
                else if (!down && wasDown && DownSince[i] > 0.0)
                {
                    LastDuration[i] = std::max(now - DownSince[i], 0.0);
                    LongestPress = std::max(LongestPress, LastDuration[i]);
                    DownSince[i] = 0.0;
                }
                Previous[i] = keys.Down[i];
            }
        }

        double heldDuration(const std::size_t index, const double now) const noexcept
        {
            return index < MatrixSize && DownSince[index] > 0.0 ? std::max(now - DownSince[index], 0.0) : 0.0;
        }
    };

    struct RuntimeRGBAnalytics
    {
        std::array<Color32, MatrixSize> Previous{};
        std::array<float, 16> LumaHistogram{};
        std::uint64_t Frames = 0;
        std::uint64_t ChangedCellsTotal = 0;
        std::size_t ChangedCells = 0;
        float AverageChangedCells = 0.0f;
        float FrameRate = 0.0f;
        double LastFrameTime = 0.0;

        void update(const std::array<Color32, MatrixSize>& framebuffer, const double now) noexcept
        {
            ChangedCells = 0;
            LumaHistogram.fill(0.0f);
            for (std::size_t row = 0; row < ActiveProbeRows; ++row)
                for (std::size_t column = 0; column < Columns; ++column)
                {
                    const std::size_t index = row * Columns + column;
                    const auto& current = framebuffer[index];
                    const auto& previous = Previous[index];
                    if (current.R != previous.R || current.G != previous.G || current.B != previous.B) ++ChangedCells;
                    const float luma = (current.R / 255.0f) * 0.2126f + (current.G / 255.0f) * 0.7152f + (current.B / 255.0f) * 0.0722f;
                    const std::size_t bucket = std::min<std::size_t>(static_cast<std::size_t>(luma * LumaHistogram.size()), LumaHistogram.size() - 1);
                    LumaHistogram[bucket] += 1.0f;
                }
            constexpr float ActiveCells = static_cast<float>(ActiveProbeRows * Columns);
            for (auto& bucket : LumaHistogram) bucket /= ActiveCells;
            Previous = framebuffer;
            ++Frames;
            ChangedCellsTotal += ChangedCells;
            AverageChangedCells += (static_cast<float>(ChangedCells) - AverageChangedCells) * 0.05f;
            if (LastFrameTime > 0.0)
            {
                const double dt = now - LastFrameTime;
                if (dt > 0.000001)
                {
                    const float instantaneous = static_cast<float>(1.0 / dt);
                    FrameRate += (instantaneous - FrameRate) * 0.08f;
                }
            }
            LastFrameTime = now;
        }
    };

}
