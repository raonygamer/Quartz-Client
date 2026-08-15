#include "quartz/client/Model.hpp"

namespace quartz::client
{
    bool readNativeAddressBinding(RuntimeBinding& binding, float& output)
    {
        binding.HasAddress = false;
        pid_t pid = static_cast<pid_t>(binding.ProcessId);
        if (!runtimeProcessIsAlive(pid) && !tryRuntimeProcessRebind(binding, pid, binding.Error)) return false;

        std::string error;
        std::optional<std::uintptr_t> signatureBase;
        if (binding.AddressMode == ProcessAddressMode::Signature)
        {
            signatureBase = advanceRuntimeSignatureScan(binding, pid, error);
            if (!signatureBase) { binding.Error = std::move(error); return false; }
        }

        std::optional<std::uintptr_t> selectedBase;
        if (binding.AddressMode == ProcessAddressMode::Signature)
        {
            if (binding.Signal == 1)
            {
                if (binding.SignatureCapturedRegister == 0) { binding.Error = "captured register is not available"; return false; }
                selectedBase = static_cast<std::uintptr_t>(binding.SignatureCapturedRegister);
            }
            else if (binding.Signal == 2)
            {
                if (binding.SignatureMatchAddress == 0) { binding.Error = "signature match is not available"; return false; }
                selectedBase = binding.SignatureMatchAddress;
            }
            else if (binding.Signal == 3)
            {
                if (binding.SignatureInstructionAddress == 0) { binding.Error = "instruction address is not available"; return false; }
                selectedBase = binding.SignatureInstructionAddress;
            }
            else selectedBase = signatureBase;
        }

        const auto address = resolveRuntimeAddress(binding, pid, error, selectedBase);
        if (!address) { binding.Error = std::move(error); return false; }
        binding.AddressValue = *address;
        binding.HasAddress = true;
        binding.AddressProvenance.clear();
        binding.AddressProvenance.push_back(std::string("process ") + binding.ProcessName + " pid " + std::to_string(pid));
        if (binding.AddressMode == ProcessAddressMode::Signature)
        {
            if (binding.SignatureMatchAddress) binding.AddressProvenance.push_back("pattern match " + runtimeHexAddress(binding.SignatureMatchAddress));
            if (binding.SignatureInstructionAddress) binding.AddressProvenance.push_back("instruction " + runtimeHexAddress(binding.SignatureInstructionAddress));
            if (binding.SignatureCapturedRegister) binding.AddressProvenance.push_back(std::string(runtimeX64RegisterName(binding.SignatureRegister)) + " = " + runtimeHexAddress(static_cast<std::uintptr_t>(binding.SignatureCapturedRegister)));
        }
        binding.AddressProvenance.push_back("resolved " + runtimeHexAddress(binding.AddressValue));
        output = 1.0f;
        binding.Error.clear();
        return true;
    }

    std::filesystem::path runtimeBindingsPath()
    {
        return settingsPath().parent_path() / "visualizer.bindings.ini";
    }

    std::string runtimeEscape(const std::string_view value)
    {
        static constexpr char Hex[] = "0123456789ABCDEF";
        std::string result;
        for (const unsigned char c : value)
        {
            if (std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '/' || c == ' ') result.push_back(static_cast<char>(c));
            else { result.push_back('%'); result.push_back(Hex[c >> 4]); result.push_back(Hex[c & 0xF]); }
        }
        return result;
    }

    std::string runtimeUnescape(const std::string_view value)
    {
        auto hex = [](const char c) -> int
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        std::string result;
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] == '%' && i + 2 < value.size())
            {
                const int hi = hex(value[i + 1]), lo = hex(value[i + 2]);
                if (hi >= 0 && lo >= 0) { result.push_back(static_cast<char>((hi << 4) | lo)); i += 2; continue; }
            }
            result.push_back(value[i]);
        }
        return result;
    }

    const char* runtimeSourceName(const RuntimeSourceKind source)
    {
        switch (source)
        {
        case RuntimeSourceKind::Constant: return "Constant";
        case RuntimeSourceKind::Time: return "Time";
        case RuntimeSourceKind::Audio: return "Audio";
        case RuntimeSourceKind::Media: return "Media / MPRIS";
        case RuntimeSourceKind::Keyboard: return "Keyboard";
        case RuntimeSourceKind::RPC: return "RPC / firmware";
        case RuntimeSourceKind::Host: return "Host";
        case RuntimeSourceKind::USB: return "USB";
        case RuntimeSourceKind::RGB: return "RGB output";
        case RuntimeSourceKind::NativeProcess: return "Native process memory";
        case RuntimeSourceKind::BindingStatus: return "Binding status";
        case RuntimeSourceKind::Unbound: return "Unbound / writable value";
        case RuntimeSourceKind::BindingValue: return "Binding value / passthrough";
        case RuntimeSourceKind::ShaderState: return "Shader / renderer state";
        case RuntimeSourceKind::ControlStatus: return "Control status";
        case RuntimeSourceKind::Aggregate: return "Aggregate bindings / controls";
        case RuntimeSourceKind::MassCompare: return "Mass compare bindings / controls";
        case RuntimeSourceKind::NativeAddress: return "Native process address";
        case RuntimeSourceKind::ObjectField: return "Object descriptor field";
        case RuntimeSourceKind::ObjectStatus: return "Object descriptor status";
        case RuntimeSourceKind::ValueBank: return "Value bank";
        case RuntimeSourceKind::StringConstant: return "String constant";
        case RuntimeSourceKind::ProfileState: return "Binding profile state";
        }
        return "Unknown";
    }

    std::vector<std::string_view> runtimeSignalNames(const RuntimeSourceKind source)
    {
        switch (source)
        {
        case RuntimeSourceKind::Constant: return {"Value"};
        case RuntimeSourceKind::Time: return {"Seconds", "Sine", "Cosine", "Saw 0..1", "Triangle 0..1", "Pulse 1 Hz", "Pulse 2 Hz"};
        case RuntimeSourceKind::Audio: return {"RMS", "Peak", "Bass", "Mid", "Treble", "Effective gain", "Gain correction", "Spectrum average", "Spectrum maximum", "Bass / treble ratio", "Bass peak"};
        case RuntimeSourceKind::Media: return {"Artwork amount", "Artwork R", "Artwork G", "Artwork B", "Playing", "Title (string)"};
        case RuntimeSourceKind::Keyboard: return {"Caps Lock", "Scroll Lock", "Held fraction", "Recent key pulse", "Keys held", "Recent events", "Last event column", "Last event row"};
        case RuntimeSourceKind::RPC: return {"Keyboard CPU %", "Scan rate Hz", "Matrix us", "RGB us", "Scan period us", "State update us", "HID us", "Total measured us"};
        case RuntimeSourceKind::Host: return {"App CPU %", "Frame delta ms", "Frame rate Hz"};
        case RuntimeSourceKind::USB: return {"Connected", "TX KiB/s", "RX KiB/s", "TX transfers/s", "RX transfers/s", "Errors", "TX total MiB", "RX total MiB", "TX errors", "RX errors"};
        case RuntimeSourceKind::RGB: return {"Average luma", "Lit fraction", "Average R", "Average G", "Average B", "Peak channel", "Peak luma"};
        case RuntimeSourceKind::NativeProcess: return {"Value"};
        case RuntimeSourceKind::BindingStatus: return {"Has value", "Last read succeeded", "Enabled", "Process alive", "Address resolved", "Register captured", "Has error", "Seconds since success", "Priority", "Has exact address"};
        case RuntimeSourceKind::Unbound: return {"Value"};
        case RuntimeSourceKind::BindingValue: return {"Value", "Raw value", "Has value", "Last read succeeded", "Enabled", "Priority"};
        case RuntimeSourceKind::ShaderState: return {"Shader ID (string)", "Custom shader", "Transition active", "Transition progress", "Base color mode", "Global brightness", "Send framebuffer", "Framebuffer width", "Framebuffer height", "Previous shader ID (string)"};
        case RuntimeSourceKind::ControlStatus: return {"Condition active", "Triggered this frame", "Enabled", "Trigger count", "Seconds since trigger", "Priority", "Source value"};
        case RuntimeSourceKind::Aggregate: return {"Value"};
        case RuntimeSourceKind::MassCompare: return {"Value"};
        case RuntimeSourceKind::NativeAddress: return {"Resolved address", "Captured register", "Signature match", "Instruction address"};
        case RuntimeSourceKind::ObjectField: return {"Value"};
        case RuntimeSourceKind::ObjectStatus: return {"Resolved", "Size bytes", "Field count", "Process alive", "Base binding ready"};
        case RuntimeSourceKind::ValueBank: return {"Value", "Boolean", "Integer", "Has value", "Changed this frame", "String length", "Address present"};
        case RuntimeSourceKind::StringConstant: return {"Text present", "Length"};
        case RuntimeSourceKind::ProfileState: return {"Active profile id", "Selected profile active", "Selected profile enabled", "Selected binding members", "Selected control members"};
        }
        return {"Value"};
    }

    const char* runtimeAggregateOperationName(const RuntimeAggregateOperation operation) noexcept
    {
        static constexpr const char* Names[] = {"Sum", "Average", "Minimum", "Maximum", "Product", "Count", "Count truthy", "Fraction truthy", "Any", "All"};
        return Names[std::clamp(static_cast<int>(operation), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    const char* runtimeCompareConditionName(const RuntimeCompareCondition condition) noexcept
    {
        static constexpr const char* Names[] = {"==", "!=", "<", "<=", ">", ">=", "between", "outside"};
        return Names[std::clamp(static_cast<int>(condition), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    const char* runtimeMassCompareResultName(const RuntimeMassCompareResult result) noexcept
    {
        static constexpr const char* Names[] = {"Any", "All", "None", "Count", "Fraction", "First match index"};
        return Names[std::clamp(static_cast<int>(result), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    const char* runtimeObjectFieldTypeName(const RuntimeObjectFieldType type) noexcept
    {
        static constexpr const char* Names[] = {"u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "float", "double", "bool", "pointer", "filler 1", "filler 2", "filler 4", "filler 8", "filler 16", "filler 32", "filler custom", "const char*", "const wchar_t*", "char[N]", "wchar_t[N]"};
        return Names[std::clamp(static_cast<int>(type), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    const char* runtimeObjectPackingName(const RuntimeObjectPacking packing) noexcept
    {
        static constexpr const char* Names[] = {"Natural", "Pack 1", "Pack 2", "Pack 4", "Pack 8", "Pack 16"};
        return Names[std::clamp(static_cast<int>(packing), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    const char* runtimeObjectAlignmentName(const RuntimeObjectAlignment alignment) noexcept
    {
        static constexpr const char* Names[] = {"Auto", "1", "2", "4", "8", "16"};
        return Names[std::clamp(static_cast<int>(alignment), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    const char* runtimeBankValueTypeName(const RuntimeBankValueType type) noexcept
    {
        static constexpr const char* Names[] = {"Number", "Integer", "Boolean", "String", "Address"};
        return Names[std::clamp(static_cast<int>(type), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    const char* runtimeActionTargetName(const RuntimeActionTarget target) noexcept
    {
        static constexpr const char* Names[] = {"Active shader", "Binding enabled", "Global brightness", "Send framebuffer", "Base color mode", "Material parameter", "Unbound binding value", "Value bank", "Control enabled", "Refresh binding", "Force binding update", "Invalidate binding", "Reset binding state", "Retry register capture", "Rescan binding pattern", "Rebind process", "Clear binding error"};
        return Names[std::clamp(static_cast<int>(target), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    bool runtimeActionTargetIsBindingOperation(const RuntimeActionTarget target) noexcept
    {
        return target >= RuntimeActionTarget::BindingRefresh && target <= RuntimeActionTarget::BindingClearError;
    }

    bool runtimeControlTargetIsBindingOperation(const RuntimeControlTarget target) noexcept
    {
        return target >= RuntimeControlTarget::BindingRefresh && target <= RuntimeControlTarget::BindingClearError;
    }

    const char* runtimeActionValueModeName(const RuntimeActionValueMode mode) noexcept
    {
        static constexpr const char* Names[] = {"Constant", "Control/binding source", "Another binding", "Value bank"};
        return Names[std::clamp(static_cast<int>(mode), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    const char* runtimeActionWhenName(const RuntimeActionWhen when) noexcept
    {
        static constexpr const char* Names[] = {"While active", "On trigger", "On update", "On change", "While truthy", "While falsy"};
        return Names[std::clamp(static_cast<int>(when), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    bool runtimeObjectFieldIsFiller(const RuntimeObjectFieldType type) noexcept
    {
        return type >= RuntimeObjectFieldType::Filler1 && type <= RuntimeObjectFieldType::FillerCustom;
    }

    std::size_t runtimeObjectFieldSize(const RuntimeObjectField& field) noexcept
    {
        switch (field.Type)
        {
        case RuntimeObjectFieldType::U8:
        case RuntimeObjectFieldType::I8:
        case RuntimeObjectFieldType::Bool:
        case RuntimeObjectFieldType::Filler1: return 1;
        case RuntimeObjectFieldType::U16:
        case RuntimeObjectFieldType::I16:
        case RuntimeObjectFieldType::Filler2: return 2;
        case RuntimeObjectFieldType::U32:
        case RuntimeObjectFieldType::I32:
        case RuntimeObjectFieldType::Float:
        case RuntimeObjectFieldType::Filler4: return 4;
        case RuntimeObjectFieldType::U64:
        case RuntimeObjectFieldType::I64:
        case RuntimeObjectFieldType::Double:
        case RuntimeObjectFieldType::Filler8: return 8;
        case RuntimeObjectFieldType::Pointer:
        case RuntimeObjectFieldType::CStringPointer:
        case RuntimeObjectFieldType::WStringPointer: return sizeof(std::uintptr_t);
        case RuntimeObjectFieldType::FixedCString: return static_cast<std::size_t>(std::max(field.FixedElementCount, 1));
        case RuntimeObjectFieldType::FixedWString: return static_cast<std::size_t>(std::max(field.FixedElementCount, 1)) * sizeof(wchar_t);
        case RuntimeObjectFieldType::Filler16: return 16;
        case RuntimeObjectFieldType::Filler32: return 32;
        case RuntimeObjectFieldType::FillerCustom: return static_cast<std::size_t>(std::max(field.CustomFillerBytes, 1));
        }
        return 1;
    }

    std::size_t runtimeObjectNaturalAlignment(const RuntimeObjectField& field) noexcept
    {
        if (runtimeObjectFieldIsFiller(field.Type)) return 1;
        switch (field.Type)
        {
        case RuntimeObjectFieldType::U8: case RuntimeObjectFieldType::I8: case RuntimeObjectFieldType::Bool: case RuntimeObjectFieldType::FixedCString: return 1;
        case RuntimeObjectFieldType::U16: case RuntimeObjectFieldType::I16: return alignof(std::uint16_t);
        case RuntimeObjectFieldType::U32: case RuntimeObjectFieldType::I32: return alignof(std::uint32_t);
        case RuntimeObjectFieldType::Float: return alignof(float);
        case RuntimeObjectFieldType::U64: case RuntimeObjectFieldType::I64: return alignof(std::uint64_t);
        case RuntimeObjectFieldType::Double: return alignof(double);
        case RuntimeObjectFieldType::Pointer: case RuntimeObjectFieldType::CStringPointer: case RuntimeObjectFieldType::WStringPointer: return alignof(std::uintptr_t);
        case RuntimeObjectFieldType::FixedWString: return alignof(wchar_t);
        default: return 1;
        }
    }

    std::size_t runtimeObjectPackingBytes(const RuntimeObjectPacking packing) noexcept
    {
        switch (packing)
        {
        case RuntimeObjectPacking::Pack1: return 1;
        case RuntimeObjectPacking::Pack2: return 2;
        case RuntimeObjectPacking::Pack4: return 4;
        case RuntimeObjectPacking::Pack8: return 8;
        case RuntimeObjectPacking::Pack16: return 16;
        case RuntimeObjectPacking::Natural: return 0;
        }
        return 0;
    }

    std::size_t runtimeObjectAlignmentBytes(const RuntimeObjectAlignment alignment) noexcept
    {
        switch (alignment)
        {
        case RuntimeObjectAlignment::Align1: return 1;
        case RuntimeObjectAlignment::Align2: return 2;
        case RuntimeObjectAlignment::Align4: return 4;
        case RuntimeObjectAlignment::Align8: return 8;
        case RuntimeObjectAlignment::Align16: return 16;
        case RuntimeObjectAlignment::Auto: return 0;
        }
        return 0;
    }

    std::size_t runtimeAlignUp(const std::size_t value, const std::size_t alignment) noexcept
    {
        if (alignment <= 1) return value;
        return (value + alignment - 1) / alignment * alignment;
    }

    std::size_t runtimeObjectFieldOffset(const RuntimeObjectDescriptor& object, const std::uint64_t fieldId, std::size_t* objectSize) noexcept
    {
        std::size_t cursor = 0;
        std::size_t maxAlignment = 1;
        std::size_t wanted = 0;
        bool found = false;
        const std::size_t pack = runtimeObjectPackingBytes(object.Packing);
        for (const auto& field : object.Fields)
        {
            if (!field.Enabled) continue;
            const std::size_t natural = runtimeObjectNaturalAlignment(field);
            const std::size_t overrideAlignment = runtimeObjectAlignmentBytes(field.Alignment);
            std::size_t alignment = overrideAlignment ? overrideAlignment : natural;
            if (pack) alignment = std::min(alignment, pack);
            alignment = std::max<std::size_t>(alignment, 1);
            maxAlignment = std::max(maxAlignment, alignment);
            const std::size_t offset = field.ManualOffset ? static_cast<std::size_t>(std::max(field.Offset, 0)) : runtimeAlignUp(cursor, alignment);
            if (field.Id == fieldId) { wanted = offset; found = true; }
            cursor = std::max(cursor, offset + runtimeObjectFieldSize(field));
        }
        const std::size_t finalAlignment = pack ? std::min(maxAlignment, pack) : maxAlignment;
        if (objectSize) *objectSize = runtimeAlignUp(cursor, std::max<std::size_t>(finalAlignment, 1));
        return found ? wanted : std::numeric_limits<std::size_t>::max();
    }

    std::string runtimeHexAddress(const std::uintptr_t value)
    {
        std::ostringstream stream; stream << "0x" << std::hex << std::uppercase << value; return stream.str();
    }


}
