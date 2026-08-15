

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

    enum class ProcessValueType : int
    {
        U8, I8, U16, I16, U32, I32, U64, I64, Float, Double, Bool
    };

    enum class ProcessAddressMode : int
    {
        AddressChain,
        Signature
    };

    enum class RuntimeSignaturePatternKind : int
    {
        HexadecimalPattern,
        OpcodePattern
    };

    enum class SignatureResultMode : int
    {
        MatchAddress,
        RipRelative32,
        PointerAtOffset,
        RegisterRelativeCapture,
        Address32AtOffset
    };

    enum class RuntimeX64Register : int
    {
        Rax, Rbx, Rcx, Rdx, Rsi, Rdi, Rbp, Rsp, R8, R9, R10, R11, R12, R13, R14, R15
    };

    enum class RuntimeDisplacementType : int
    {
        I8, I32, Manual
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

    struct RuntimeBindingProfile
    {
        std::uint64_t Id = 0;
        bool Enabled = true;
        char Name[64] = "Profile";
        bool Exclusive = true;
        std::vector<std::uint64_t> BindingIds;
        std::vector<std::uint64_t> ControlIds;
        bool HotkeyCtrl = false;
        bool HotkeyAlt = false;
        bool HotkeyShift = false;
        int HotkeyKey = 0;
        bool HotkeyDown = false;
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

    enum class ProcessRebindMode : int
    {
        NameExact,
        ExecutableExact,
        TitleExact,
        CommandLineExact,
        NameRegex,
        ExecutableRegex,
        TitleRegex,
        CommandLineRegex,
        AnyRegex
    };

    struct RuntimeProcessInfo
    {
        pid_t Pid = 0;
        std::string Name;
        std::string Exe;
        std::string Title;
        std::string CommandLine;
        std::string SearchText;
    };

    struct RuntimeProcessModule
    {
        std::uintptr_t Base = 0;
        std::uintptr_t End = 0;
        std::string Name;
        std::string Path;
    };

    struct RuntimeProcessRegion
    {
        std::uintptr_t Base = 0;
        std::uintptr_t End = 0;
        bool Readable = false;
        bool Executable = false;
        std::string Path;
    };

    struct RuntimeRegisterCaptureState
    {
        std::mutex Mutex;
        bool Finished = false;
        bool Success = false;
        std::uint64_t RegisterValue = 0;
        std::intptr_t Displacement = 0;
        std::uintptr_t ResolvedAddress = 0;
        std::string Status;
        // Keep the worker last: members are destroyed in reverse order, so jthread requests stop
        // and joins while Mutex/Status/the result fields are still alive. The worker captures this state raw.
        std::jthread Worker;
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

    static std::string runtimeLower(std::string_view value)
    {
        std::string result(value);
        std::ranges::transform(result, result.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }

    static std::string runtimeRegexEscape(const std::string_view value)
    {
        static constexpr std::string_view Special = R"(\.^$|()[]*+?{})";
        std::string result;
        result.reserve(value.size() + 8);
        for (const char c : value)
        {
            if (Special.find(c) != std::string_view::npos) result.push_back('\\');
            result.push_back(c);
        }
        return result;
    }

    static bool runtimeRebindModeIsRegex(const ProcessRebindMode mode) noexcept
    {
        return mode >= ProcessRebindMode::NameRegex;
    }

    static const char* runtimeRebindModeName(const ProcessRebindMode mode) noexcept
    {
        switch (mode)
        {
        case ProcessRebindMode::NameExact: return "Process name (exact)";
        case ProcessRebindMode::ExecutableExact: return "Executable path (exact)";
        case ProcessRebindMode::TitleExact: return "Process title / argv[0] (exact)";
        case ProcessRebindMode::CommandLineExact: return "Command line (exact)";
        case ProcessRebindMode::NameRegex: return "Process name (regex)";
        case ProcessRebindMode::ExecutableRegex: return "Executable path (regex)";
        case ProcessRebindMode::TitleRegex: return "Process title / argv[0] (regex)";
        case ProcessRebindMode::CommandLineRegex: return "Command line (regex)";
        case ProcessRebindMode::AnyRegex: return "Any field (regex)";
        }
        return "Process name (exact)";
    }

    static std::vector<RuntimeProcessInfo> enumerateRuntimeProcesses()
    {
        std::vector<RuntimeProcessInfo> processes;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator("/proc", error))
        {
            if (error || !entry.is_directory(error)) continue;
            const std::string name = entry.path().filename().string();
            if (name.empty() || !std::ranges::all_of(name, [](const unsigned char c) { return std::isdigit(c) != 0; })) continue;
            int pid = 0;
            const auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), pid);
            if (ec != std::errc{} || ptr != name.data() + name.size() || pid <= 0) continue;

            RuntimeProcessInfo process;
            process.Pid = static_cast<pid_t>(pid);
            std::ifstream comm(entry.path() / "comm");
            std::getline(comm, process.Name);
            std::array<char, 4096> exe{};
            const ssize_t count = ::readlink((entry.path() / "exe").c_str(), exe.data(), exe.size() - 1);
            if (count > 0) process.Exe.assign(exe.data(), static_cast<std::size_t>(count));

            std::ifstream cmdline(entry.path() / "cmdline", std::ios::binary);
            if (cmdline)
            {
                std::string raw((std::istreambuf_iterator<char>(cmdline)), std::istreambuf_iterator<char>());
                if (!raw.empty())
                {
                    const std::size_t firstEnd = raw.find('\0');
                    process.Title = raw.substr(0, firstEnd == std::string::npos ? raw.size() : firstEnd);
                    for (char& c : raw) if (c == '\0') c = ' ';
                    process.CommandLine = trim(std::move(raw));
                }
            }
            if (process.Title.empty()) process.Title = process.Name;
            if (process.CommandLine.empty()) process.CommandLine = !process.Exe.empty() ? process.Exe : process.Name;
            if (process.Name.empty() && process.Exe.empty() && process.Title.empty()) continue;
            process.SearchText = runtimeLower(std::to_string(process.Pid) + "\n" + process.Name + "\n" + process.Exe + "\n" + process.Title + "\n" + process.CommandLine);
            processes.emplace_back(std::move(process));
        }
        std::ranges::sort(processes, [](const auto& a, const auto& b)
        {
            if (a.Name != b.Name) return a.Name < b.Name;
            return a.Pid < b.Pid;
        });
        return processes;
    }

    static std::string runtimeProcessDisplayTitle(const RuntimeProcessInfo& process)
    {
        std::string title = process.Name.empty() ? "<unnamed>" : process.Name;
        if (!process.Title.empty() && process.Title != process.Name && process.Title != process.Exe)
        {
            std::string decorative = process.Title;
            if (decorative.size() > 72) decorative.resize(69), decorative += "...";
            title += "  -  " + decorative;
        }
        return title;
    }

    static bool runtimeProcessMatchesSearch(const RuntimeProcessInfo& process, const std::string_view loweredQuery)
    {
        return loweredQuery.empty() || process.SearchText.find(loweredQuery) != std::string::npos;
    }

    static std::string runtimeProcessRebindValue(const RuntimeProcessInfo& process, const ProcessRebindMode mode)
    {
        switch (mode)
        {
        case ProcessRebindMode::NameExact:
        case ProcessRebindMode::NameRegex: return process.Name;
        case ProcessRebindMode::ExecutableExact:
        case ProcessRebindMode::ExecutableRegex: return process.Exe;
        case ProcessRebindMode::TitleExact:
        case ProcessRebindMode::TitleRegex: return process.Title;
        case ProcessRebindMode::CommandLineExact:
        case ProcessRebindMode::CommandLineRegex: return process.CommandLine;
        case ProcessRebindMode::AnyRegex: return process.Name;
        }
        return process.Name;
    }

    static void captureRuntimeRebindPattern(RuntimeBinding& binding, const RuntimeProcessInfo& process)
    {
        std::string value = runtimeProcessRebindValue(process, binding.RebindMode);
        if (value.empty()) value = process.Name;
        if (runtimeRebindModeIsRegex(binding.RebindMode)) value = "^" + runtimeRegexEscape(value) + "$";
        std::snprintf(binding.ProcessRebindPattern, sizeof(binding.ProcessRebindPattern), "%s", value.c_str());
    }

    static bool runtimeProcessMatchesRebind(const RuntimeProcessInfo& process, const RuntimeBinding& binding, const std::regex* regex)
    {
        if (runtimeRebindModeIsRegex(binding.RebindMode))
        {
            if (!regex) return false;
            const auto matches = [&](const std::string& value) { return !value.empty() && std::regex_search(value, *regex); };
            switch (binding.RebindMode)
            {
            case ProcessRebindMode::NameRegex: return matches(process.Name);
            case ProcessRebindMode::ExecutableRegex: return matches(process.Exe);
            case ProcessRebindMode::TitleRegex: return matches(process.Title);
            case ProcessRebindMode::CommandLineRegex: return matches(process.CommandLine);
            case ProcessRebindMode::AnyRegex: return matches(process.Name) || matches(process.Exe) || matches(process.Title) || matches(process.CommandLine);
            default: return false;
            }
        }
        return runtimeProcessRebindValue(process, binding.RebindMode) == binding.ProcessRebindPattern;
    }

    static std::optional<RuntimeProcessInfo> findRuntimeRebindProcess(const RuntimeBinding& binding, std::string& error)
    {
        if (!binding.AutoReattach) { error = "automatic process rebind is disabled"; return std::nullopt; }
        if (binding.ProcessRebindPattern[0] == '\0') { error = "rebind pattern is empty"; return std::nullopt; }
        std::optional<std::regex> regex;
        if (runtimeRebindModeIsRegex(binding.RebindMode))
        {
            try { regex.emplace(binding.ProcessRebindPattern, std::regex::ECMAScript | std::regex::icase); }
            catch (const std::regex_error& e) { error = std::string("invalid rebind regex: ") + e.what(); return std::nullopt; }
        }
        std::optional<RuntimeProcessInfo> best;
        for (const auto& process : enumerateRuntimeProcesses())
            if (runtimeProcessMatchesRebind(process, binding, regex ? &*regex : nullptr) && (!best || process.Pid > best->Pid)) best = process;
        if (!best) error = "waiting for process match: " + std::string(binding.ProcessRebindPattern);
        return best;
    }

    static bool runtimeProcessIsAlive(const pid_t pid) noexcept
    {
        if (pid <= 0) return false;
        errno = 0;
        return ::kill(pid, 0) == 0 || errno != ESRCH;
    }

    static double runtimeSteadySeconds() noexcept
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static bool tryRuntimeProcessRebind(RuntimeBinding& binding, pid_t& pid, std::string& error)
    {
        if (!binding.AutoReattach) { error = "process is not running"; return false; }
        const double now = runtimeSteadySeconds();
        if (now < binding.NextProcessSearch) { error = "waiting for process match: " + std::string(binding.ProcessRebindPattern); return false; }
        binding.NextProcessSearch = now + 1.0;
        const auto process = findRuntimeRebindProcess(binding, error);
        if (!process) return false;
        pid = process->Pid;
        binding.ProcessId = static_cast<int>(pid);
        std::snprintf(binding.ProcessName, sizeof(binding.ProcessName), "%s", process->Name.c_str());
        binding.NextProcessSearch = 0.0;
        error.clear();
        return true;
    }

    static std::vector<RuntimeProcessModule> enumerateRuntimeModules(const pid_t pid)
    {
        std::vector<RuntimeProcessModule> modules;
        std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
        if (!maps) return modules;
        std::unordered_map<std::string, std::size_t> indices;
        std::string line;
        while (std::getline(maps, line))
        {
            std::istringstream stream(line);
            std::string range, permissions, offsetText, device, inode;
            if (!(stream >> range >> permissions >> offsetText >> device >> inode)) continue;
            std::string path;
            std::getline(stream, path);
            path = trim(std::move(path));
            if (path.empty() || path.front() == '[') continue;
            const std::size_t dash = range.find('-');
            if (dash == std::string::npos) continue;
            std::uintptr_t start = 0, end = 0, offset = 0;
            auto parseHex = [](const std::string_view value, std::uintptr_t& result)
            {
                const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result, 16);
                return ec == std::errc{} && ptr == value.data() + value.size();
            };
            if (!parseHex(std::string_view(range).substr(0, dash), start) || !parseHex(std::string_view(range).substr(dash + 1), end) || !parseHex(offsetText, offset)) continue;
            const std::uintptr_t base = start >= offset ? start - offset : start;
            const auto existing = indices.find(path);
            if (existing == indices.end())
            {
                RuntimeProcessModule module;
                module.Base = base;
                module.End = end;
                module.Path = path;
                module.Name = std::filesystem::path(path).filename().string();
                indices[path] = modules.size();
                modules.emplace_back(std::move(module));
            }
            else
            {
                auto& module = modules[existing->second];
                module.Base = std::min(module.Base, base);
                module.End = std::max(module.End, end);
            }
        }
        std::ranges::sort(modules, std::ranges::less{}, &RuntimeProcessModule::Base);
        return modules;
    }

    static std::vector<RuntimeProcessRegion> enumerateRuntimeRegions(const pid_t pid)
    {
        std::vector<RuntimeProcessRegion> regions;
        std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
        if (!maps) return regions;
        std::string line;
        while (std::getline(maps, line))
        {
            std::istringstream stream(line);
            std::string range, permissions, offset, device, inode;
            if (!(stream >> range >> permissions >> offset >> device >> inode)) continue;
            const std::size_t dash = range.find('-');
            if (dash == std::string::npos) continue;
            std::uintptr_t base = 0, end = 0;
            const auto [basePtr, baseEc] = std::from_chars(range.data(), range.data() + dash, base, 16);
            const auto [endPtr, endEc] = std::from_chars(range.data() + dash + 1, range.data() + range.size(), end, 16);
            if (baseEc != std::errc{} || endEc != std::errc{} || base >= end) continue;
            std::string path;
            std::getline(stream, path);
            RuntimeProcessRegion region;
            region.Base = base;
            region.End = end;
            region.Readable = !permissions.empty() && permissions[0] == 'r';
            region.Executable = permissions.size() > 2 && permissions[2] == 'x';
            region.Path = trim(std::move(path));
            regions.emplace_back(std::move(region));
        }
        return regions;
    }

    static int runtimeHexNibble(const char c) noexcept
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static bool parseRuntimeHexPattern(const std::string_view signature, std::vector<std::uint8_t>& bytes, std::vector<std::uint8_t>& masks, std::string& error)
    {
        bytes.clear();
        masks.clear();
        std::istringstream stream{std::string(signature)};
        std::string token;
        while (stream >> token)
        {
            if (token == "?" || token == "??")
            {
                bytes.push_back(0);
                masks.push_back(0);
                continue;
            }
            if (token.size() != 2)
            {
                error = "hexadecimal pattern token must be two hex nibbles or ?: " + token;
                return false;
            }
            const bool highWildcard = token[0] == '?';
            const bool lowWildcard = token[1] == '?';
            const int high = highWildcard ? 0 : runtimeHexNibble(token[0]);
            const int low = lowWildcard ? 0 : runtimeHexNibble(token[1]);
            if (high < 0 || low < 0)
            {
                error = "invalid hexadecimal pattern byte: " + token;
                return false;
            }
            bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
            masks.push_back(static_cast<std::uint8_t>((highWildcard ? 0 : 0xF0) | (lowWildcard ? 0 : 0x0F)));
        }
        if (bytes.empty())
        {
            error = "hexadecimal pattern is empty";
            return false;
        }
        if (std::ranges::all_of(masks, [](const std::uint8_t mask) { return mask == 0; }))
        {
            error = "hexadecimal pattern cannot contain only wildcards";
            return false;
        }
        error.clear();
        return true;
    }

    static std::string runtimeNormalizeOpcodeText(std::string value)
    {
        value = trim(std::move(value));
        std::string result; result.reserve(value.size());
        bool space = false;
        for (const unsigned char c : value)
        {
            if (std::isspace(c)) { space = !result.empty(); continue; }
            if (space) { result.push_back(' '); space = false; }
            result.push_back(static_cast<char>(std::tolower(c)));
        }
        return result;
    }

    static bool runtimeWildcardTextMatch(const std::string_view pattern, const std::string_view value) noexcept
    {
        std::size_t p = 0, v = 0, star = std::string_view::npos, retry = 0;
        while (v < value.size())
        {
            if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) { ++p; ++v; continue; }
            if (p < pattern.size() && pattern[p] == '*') { star = p++; retry = v; continue; }
            if (star != std::string_view::npos) { p = star + 1; v = ++retry; continue; }
            return false;
        }
        while (p < pattern.size() && pattern[p] == '*') ++p;
        return p == pattern.size();
    }

    static std::vector<std::string> parseRuntimeOpcodePattern(const std::string_view specification)
    {
        std::vector<std::string> lines; std::istringstream stream{std::string(specification)}; std::string line;
        while (std::getline(stream, line))
        {
            line = runtimeNormalizeOpcodeText(line);
            if (line.empty() || line.starts_with('#') || line.starts_with("//")) continue;
            lines.emplace_back(std::move(line));
        }
        return lines;
    }

#if QUARTZ_HAS_ZYDIS
    static bool runtimeDecodeInstructionText(const std::span<const std::uint8_t> bytes, const std::uintptr_t address, std::string& text, std::size_t& length)
    {
        ZydisDisassembledInstruction instruction{};
        if (!ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64, address, bytes.data(), bytes.size(), &instruction))) return false;
        text = runtimeNormalizeOpcodeText(instruction.text);
        length = instruction.info.length;
        return length != 0;
    }
#else
    static bool runtimeDecodeInstructionText(const std::span<const std::uint8_t>, const std::uintptr_t, std::string&, std::size_t&) { return false; }
#endif

    static bool runtimeOpcodePatternMatches(const std::span<const std::uint8_t> bytes, const std::uintptr_t address, const std::vector<std::string>& patterns, std::size_t& matchedLength)
    {
        matchedLength = 0;
        if (patterns.empty()) return false;
        std::size_t offset = 0;
        for (const auto& pattern : patterns)
        {
            if (offset >= bytes.size()) return false;
            std::string text; std::size_t instructionLength = 0;
            if (!runtimeDecodeInstructionText(bytes.subspan(offset), address + offset, text, instructionLength)) return false;
            if (!runtimeWildcardTextMatch(pattern, text)) return false;
            offset += instructionLength;
        }
        matchedLength = offset;
        return true;
    }

    static bool readProcessMemoryBlock(const pid_t pid, const std::uintptr_t address, std::span<std::uint8_t> buffer, std::string& error)
    {
        if (buffer.empty()) return true;
        iovec local{buffer.data(), buffer.size()};
        iovec remote{reinterpret_cast<void*>(address), buffer.size()};
        errno = 0;
        const ssize_t count = ::process_vm_readv(pid, &local, 1, &remote, 1, 0);
        if (count == static_cast<ssize_t>(buffer.size())) return true;
        error = count < 0 ? std::string(std::strerror(errno)) : "short read (" + std::to_string(count) + "/" + std::to_string(buffer.size()) + ")";
        return false;
    }

    template<typename T>
    static bool readProcessMemoryValue(pid_t pid, std::uintptr_t address, T& value, std::string& error);

    static const char* runtimeX64RegisterName(const RuntimeX64Register reg) noexcept
    {
        static constexpr const char* Names[] = {"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};
        return Names[std::clamp(static_cast<int>(reg), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    static std::uint64_t runtimeX64RegisterValue(const user_regs_struct& regs, const RuntimeX64Register reg) noexcept
    {
        switch (reg)
        {
        case RuntimeX64Register::Rax: return regs.rax;
        case RuntimeX64Register::Rbx: return regs.rbx;
        case RuntimeX64Register::Rcx: return regs.rcx;
        case RuntimeX64Register::Rdx: return regs.rdx;
        case RuntimeX64Register::Rsi: return regs.rsi;
        case RuntimeX64Register::Rdi: return regs.rdi;
        case RuntimeX64Register::Rbp: return regs.rbp;
        case RuntimeX64Register::Rsp: return regs.rsp;
        case RuntimeX64Register::R8: return regs.r8;
        case RuntimeX64Register::R9: return regs.r9;
        case RuntimeX64Register::R10: return regs.r10;
        case RuntimeX64Register::R11: return regs.r11;
        case RuntimeX64Register::R12: return regs.r12;
        case RuntimeX64Register::R13: return regs.r13;
        case RuntimeX64Register::R14: return regs.r14;
        case RuntimeX64Register::R15: return regs.r15;
        }
        return 0;
    }
