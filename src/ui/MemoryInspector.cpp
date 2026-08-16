#include "quartz/client/ui/MemoryInspector.hpp"
#include "quartz/client/ui/AddressInput.hpp"
#include "quartz/client/ui/I18n.hpp"
#include "quartz/client/ui/ProcessPicker.hpp"
#include "quartz/client/ui/SignatureMaker.hpp"
#include "quartz/client/ui/TextEditorSupport.hpp"
#include "quartz/client/Model.hpp"
#include "quartz/client/native/ExecutionProbe.hpp"
#include "quartz/client/native/FunctionAnalysis.hpp"
#include "quartz/client/native/NativeDisassembly.hpp"
#include "quartz/client/native/OpcodePatternEditor.hpp"
#include "quartz/client/settings/RuntimeConfiguration.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <imgui_internal.h>

namespace quartz::client
{
    namespace
    {
        struct DisassemblyMarker
        {
            std::array<char, 64> Tag{};
            ImVec4 Color{0.0f, 0.72f, 1.0f, 0.22f};
            DisassemblyMarker() { std::snprintf(Tag.data(), Tag.size(), "%s", "bookmark"); }
        };

        struct RegisterValue { const char* Name; std::uint64_t Value; };

        struct DisassemblyInstruction
        {
            std::uintptr_t Address = 0;
            std::size_t Length = 0;
            RuntimeDecodedInstruction Decoded;
            std::size_t DisplayLine = 0;
        };

        struct CachedControlFlow
        {
            std::uintptr_t Source = 0;
            RuntimeBranchKind Branch = RuntimeBranchKind::None;
            std::optional<std::uintptr_t> Target;
            std::optional<std::uintptr_t> FallThrough;
            std::string Mnemonic;
        };

        struct FarControlFlowEntry
        {
            std::uintptr_t Source = 0;
            std::uintptr_t Destination = 0;
            RuntimeBranchKind Branch = RuntimeBranchKind::None;
            std::string Mnemonic;
            ImU32 Color = 0;
        };

        struct PatchRecord
        {
            pid_t Pid = 0;
            std::uintptr_t Address = 0;
            std::vector<std::uint8_t> Original;
            std::vector<std::uint8_t> Patched;
            std::string Label;
        };

        struct AssemblerState
        {
            TextEditor Editor;
            bool Initialized = false;
            bool Active = false;
            bool Dirty = false;
            bool PreviewValid = false;
            bool NopMode = false;
            bool ConfirmArmed = false;
            double LastEdit = 0.0;
            std::size_t FirstInstruction = 0;
            std::size_t LastInstruction = 0;
            std::uintptr_t Address = 0;
            std::size_t Span = 0;
            std::vector<std::uint8_t> Original;
            std::vector<std::uint8_t> Assembled;
            std::vector<std::uint8_t> Patch;
            std::string Error;
            std::string PreviewText;
        };

        struct EnhancedMemoryInspectorState
        {
            pid_t LastPid = 0;
            std::uintptr_t LastAddress = 0;
            int LastReadSize = 0;
            std::uintptr_t AddressTextValue = std::numeric_limits<std::uintptr_t>::max();
            std::array<char, 256> AddressText{};
            std::vector<RuntimeProcessInfo> Processes;
            std::array<char, 256> ProcessSearch{};
            std::uintptr_t BufferBase = 0;
            std::size_t BufferSize = 0;
            std::uintptr_t BufferRegionBase = 0;
            std::uintptr_t BufferRegionEnd = 0;
            double LastDisassemblyWindowShift = 0.0;
            std::vector<std::uintptr_t> NavigationBack;
            std::vector<std::uintptr_t> NavigationForward;
            std::array<char, 256> GoToText{};
            std::vector<DisassemblyInstruction> Instructions;
            std::vector<std::uintptr_t> DisplayLineAddresses;
            std::unordered_map<std::uintptr_t,std::size_t> DisplayLineByAddress;
            std::vector<RuntimeProcessModule> DisplayModules;
            RuntimeX86Mode DisplayMode = RuntimeX86Mode::X64;
            float AddressColumnWidth = 205.0f;
            std::map<std::uintptr_t,CachedControlFlow> FlowGraph;
            pid_t FlowPid = 0;
            std::vector<FarControlFlowEntry> FarFlowEntries;
            std::unordered_map<std::uintptr_t,std::string> FlowPreviewCache;
            bool OpenFarFlowPopup = false;
            std::optional<std::uintptr_t> ContextAddress;
            bool OpenContextPopup = false;
            std::map<std::pair<pid_t, std::uintptr_t>, DisassemblyMarker> Markers;
            std::map<std::pair<pid_t, std::uintptr_t>, std::array<char, 96>> FunctionNames;
            std::vector<std::uint8_t> RawBytes;
            std::uintptr_t SyncedAddress = 0;
            std::uintptr_t NavigationHighlightAddress = 0;
            bool SynchronizeAddress = true;
            bool DisassemblyDirty = false;
            bool LastProbeRunning = false;
            pid_t LastProbePid = 0;
            std::uintptr_t LastProbeAddress = 0;
            double LastProbeHitTime = 0.0;
            ExecutionProbeHit CapturedHit{};
            bool HasCapturedHit = false;
            bool SelectRegistersTab = false;
            bool SelectAssemblerTab = false;
            std::optional<std::uintptr_t> PendingInspectorAddress;
            std::optional<std::uintptr_t> PendingScrollAnchor;
            TextEditor::Scroll PendingScrollAlignment = TextEditor::Scroll::alignTop;
            RuntimeFunctionAnalysisSnapshot FunctionAnalysis;
            std::uint64_t FunctionRevision = 0;
            double LastFunctionAnalysisPoll = 0.0;
            double LastDisassemblyRefresh = 0.0;
            double LastRawRefresh = 0.0;
            AssemblerState Assembler;
            std::vector<PatchRecord> PatchHistory;
        };

        EnhancedMemoryInspectorState& enhancedMemoryInspectorState() { static EnhancedMemoryInspectorState state; return state; }

        std::string lower(std::string value) { std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); }); return value; }
        std::string upper(std::string value) { std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); }); return value; }

        std::string moduleDisplayName(const RuntimeProcessModule& module)
        {
            if (!module.Name.empty()) return module.Name; const std::size_t slash=module.Path.find_last_of("/\\"); return slash==std::string::npos?module.Path:module.Path.substr(slash+1);
        }

        std::string processAddressText(const RuntimeX86Mode mode, const std::span<const RuntimeProcessModule> modules, const std::uintptr_t address)
        {
            return runtimeFormatProcessAddress(mode,modules,address);
        }

        void syncInspectorAddressText(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); if (ui.AddressTextValue == state.Address) return;
            std::snprintf(ui.AddressText.data(), ui.AddressText.size(), "0x%llX", static_cast<unsigned long long>(state.Address)); ui.AddressTextValue = state.Address;
        }

        void configureDisassemblyEditor(RuntimeMemoryInspectorState& state)
        {
            state.Disassembly.SetPalette(ui::quartzTextEditorPalette()); state.Disassembly.SetLanguage(intelAsmPatternLanguage()); state.Disassembly.SetReadOnlyEnabled(true); state.Disassembly.SetCaretsVisible(false); state.Disassembly.SetShowLineNumbersEnabled(false); state.Disassembly.SetShowMiniMapEnabled(false); state.Disassembly.SetWordWrapEnabled(false); state.Disassembly.SetDecorationLeftMargin(1); state.Disassembly.SetTextLeftMargin(2); state.EditorInitialized = true;
        }

        void configureAssemblerEditor(AssemblerState& assembler)
        {
            if (!assembler.Initialized)
            {
                assembler.Editor.SetLanguage(intelAsmPatternLanguage()); assembler.Editor.SetTabSize(4); assembler.Editor.SetInsertSpacesOnTabs(true); assembler.Editor.SetShowLineNumbersEnabled(true); assembler.Editor.SetShowMiniMapEnabled(false); assembler.Editor.SetShowMatchingBrackets(false); assembler.Editor.SetWordWrapEnabled(false); assembler.Initialized = true;
            }
            assembler.Editor.SetPalette(ui::quartzTextEditorPalette());
        }

        std::vector<RegisterValue> registerValues(const ExecutionProbeHit& hit, const RuntimeX86Mode mode)
        {
            const auto& r = hit.Registers; std::vector<RegisterValue> values;
            if (mode == RuntimeX86Mode::X86)
            {
                const auto low = [](const unsigned long long value) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)); };
                values = {{"EAX", low(r.rax)}, {"EBX", low(r.rbx)}, {"ECX", low(r.rcx)}, {"EDX", low(r.rdx)}, {"ESI", low(r.rsi)}, {"EDI", low(r.rdi)}, {"EBP", low(r.rbp)}, {"ESP", low(r.rsp)}, {"EIP", low(r.rip)}, {"EFLAGS", low(r.eflags)}, {"ORIG_EAX", low(r.orig_rax)}, {"CS", r.cs}, {"SS", r.ss}, {"DS", r.ds}, {"ES", r.es}, {"FS", r.fs}, {"GS", r.gs}, {"FS_BASE", r.fs_base}, {"GS_BASE", r.gs_base}};
            }
            else values = {{"RAX", r.rax}, {"RBX", r.rbx}, {"RCX", r.rcx}, {"RDX", r.rdx}, {"RSI", r.rsi}, {"RDI", r.rdi}, {"RBP", r.rbp}, {"RSP", r.rsp}, {"R8", r.r8}, {"R9", r.r9}, {"R10", r.r10}, {"R11", r.r11}, {"R12", r.r12}, {"R13", r.r13}, {"R14", r.r14}, {"R15", r.r15}, {"RIP", r.rip}, {"RFLAGS", r.eflags}, {"ORIG_RAX", r.orig_rax}, {"CS", r.cs}, {"SS", r.ss}, {"DS", r.ds}, {"ES", r.es}, {"FS", r.fs}, {"GS", r.gs}, {"FS_BASE", r.fs_base}, {"GS_BASE", r.gs_base}};
            return values;
        }

        std::optional<std::uint64_t> registerValue(const ExecutionProbeHit& hit, std::string token)
        {
            token = upper(std::move(token)); const auto& r = hit.Registers;
            const auto alias = [&](const char* r64, const char* r32, const char* r16, const char* lo8, const char* hi8, const std::uint64_t value) -> std::optional<std::uint64_t>
            {
                if (token == r64) return value; if (token == r32) return value & 0xFFFFFFFFULL; if (token == r16) return value & 0xFFFFULL; if (token == lo8) return value & 0xFFULL; if (hi8 && *hi8 && token == hi8) return (value >> 8) & 0xFFULL; return std::nullopt;
            };
            if (auto value = alias("RAX","EAX","AX","AL","AH",r.rax)) return value; if (auto value = alias("RBX","EBX","BX","BL","BH",r.rbx)) return value;
            if (auto value = alias("RCX","ECX","CX","CL","CH",r.rcx)) return value; if (auto value = alias("RDX","EDX","DX","DL","DH",r.rdx)) return value;
            if (auto value = alias("RSI","ESI","SI","SIL","",r.rsi)) return value; if (auto value = alias("RDI","EDI","DI","DIL","",r.rdi)) return value;
            if (auto value = alias("RBP","EBP","BP","BPL","",r.rbp)) return value; if (auto value = alias("RSP","ESP","SP","SPL","",r.rsp)) return value;
            const std::array<std::pair<const char*, std::uint64_t>, 8> extended = {{{"R8",r.r8},{"R9",r.r9},{"R10",r.r10},{"R11",r.r11},{"R12",r.r12},{"R13",r.r13},{"R14",r.r14},{"R15",r.r15}}};
            for (const auto& [name, value] : extended)
            {
                const std::string base(name); if (token == base) return value; if (token == base + "D") return value & 0xFFFFFFFFULL; if (token == base + "W") return value & 0xFFFFULL; if (token == base + "B") return value & 0xFFULL;
            }
            if (token == "RIP") return r.rip; if (token == "EIP") return r.rip & 0xFFFFFFFFULL; if (token == "RFLAGS" || token == "EFLAGS" || token == "FLAGS") return r.eflags; return std::nullopt;
        }

        bool indexedRegister(const std::string_view token, const std::string_view prefix, const unsigned maximum)
        {
            if (!token.starts_with(prefix) || token.size() == prefix.size()) return false; unsigned value=0; const auto digits=token.substr(prefix.size()); for (const char c:digits) { if (!std::isdigit(static_cast<unsigned char>(c))) return false; value=value*10U+static_cast<unsigned>(c-'0'); } return value<=maximum;
        }

        bool knownRegister(std::string token)
        {
            token = upper(std::move(token)); static ExecutionProbeHit dummy{}; if (registerValue(dummy, token)) return true;
            if (token=="CS"||token=="SS"||token=="DS"||token=="ES"||token=="FS"||token=="GS"||token=="FS_BASE"||token=="GS_BASE"||token=="ORIG_RAX"||token=="ORIG_EAX"||token=="MXCSR"||token=="ST") return true;
            if (indexedRegister(token,"XMM",31)||indexedRegister(token,"YMM",31)||indexedRegister(token,"ZMM",31)||indexedRegister(token,"MM",7)||indexedRegister(token,"ST",7)||indexedRegister(token,"CR",8)||indexedRegister(token,"DR",7)) return true;
            return token == "RAX" || token == "EAX" || token == "AX" || token == "AL" || token == "AH" || token == "RBX" || token == "EBX" || token == "BX" || token == "BL" || token == "BH" || token == "RCX" || token == "ECX" || token == "CX" || token == "CL" || token == "CH" || token == "RDX" || token == "EDX" || token == "DX" || token == "DL" || token == "DH" || token == "RSI" || token == "ESI" || token == "SI" || token == "SIL" || token == "RDI" || token == "EDI" || token == "DI" || token == "DIL" || token == "RBP" || token == "EBP" || token == "BP" || token == "BPL" || token == "RSP" || token == "ESP" || token == "SP" || token == "SPL" || token == "RIP" || token == "EIP" || token == "RFLAGS" || token == "EFLAGS" || token == "FLAGS" || (token.size() >= 2 && token[0] == 'R' && token[1] >= '8' && token[1] <= '9') || token.starts_with("R10") || token.starts_with("R11") || token.starts_with("R12") || token.starts_with("R13") || token.starts_with("R14") || token.starts_with("R15");
        }

        const char* registerDescription(std::string token)
        {
            token=upper(std::move(token)); if (token=="ORIG_RAX"||token=="ORIG_EAX") return ui::i18n::tr("re.regOriginalSyscall"); if (token=="RAX"||token=="EAX"||token=="AX"||token=="AL"||token=="AH") return ui::i18n::tr("re.regAccumulator"); if (token=="RBX"||token=="EBX"||token=="BX"||token=="BL"||token=="BH") return ui::i18n::tr("re.regBase"); if (token=="RCX"||token=="ECX"||token=="CX"||token=="CL"||token=="CH") return ui::i18n::tr("re.regCounter"); if (token=="RDX"||token=="EDX"||token=="DX"||token=="DL"||token=="DH") return ui::i18n::tr("re.regData");
            if (token=="RSI"||token=="ESI"||token=="SI"||token=="SIL") return ui::i18n::tr("re.regSourceIndex"); if (token=="RDI"||token=="EDI"||token=="DI"||token=="DIL") return ui::i18n::tr("re.regDestinationIndex"); if (token=="RBP"||token=="EBP"||token=="BP"||token=="BPL") return ui::i18n::tr("re.regBasePointer"); if (token=="RSP"||token=="ESP"||token=="SP"||token=="SPL") return ui::i18n::tr("re.regStackPointer"); if (token=="RIP"||token=="EIP") return ui::i18n::tr("re.regInstructionPointer"); if (token=="RFLAGS"||token=="EFLAGS"||token=="FLAGS") return ui::i18n::tr("re.regFlags");
            if (token=="CS"||token=="SS"||token=="DS"||token=="ES"||token=="FS"||token=="GS") return ui::i18n::tr("re.regSegment"); if (token=="FS_BASE"||token=="GS_BASE") return ui::i18n::tr("re.regSegmentBase"); if (token=="MXCSR") return ui::i18n::tr("re.regMxcsr"); if (indexedRegister(token,"XMM",31)||indexedRegister(token,"YMM",31)||indexedRegister(token,"ZMM",31)) return ui::i18n::tr("re.regVector"); if (indexedRegister(token,"MM",7)) return ui::i18n::tr("re.regMmx"); if (token=="ST"||indexedRegister(token,"ST",7)) return ui::i18n::tr("re.regX87"); if (indexedRegister(token,"CR",8)) return ui::i18n::tr("re.regControl"); if (indexedRegister(token,"DR",7)) return ui::i18n::tr("re.regDebug"); if (token.size()>=2&&token[0]=='R'&&std::isdigit(static_cast<unsigned char>(token[1]))) return ui::i18n::tr("re.regExtended"); return nullptr;
        }

        std::optional<bool> branchCondition(const std::string_view mnemonic, const std::uint64_t flags)
        {
            const bool cf = flags & (1ULL << 0), pf = flags & (1ULL << 2), zf = flags & (1ULL << 6), sf = flags & (1ULL << 7), of = flags & (1ULL << 11); const std::string op = lower(std::string(mnemonic));
            if (op == "je" || op == "jz") return zf; if (op == "jne" || op == "jnz") return !zf; if (op == "ja" || op == "jnbe") return !cf && !zf; if (op == "jae" || op == "jnb" || op == "jnc") return !cf;
            if (op == "jb" || op == "jc" || op == "jnae") return cf; if (op == "jbe" || op == "jna") return cf || zf; if (op == "jg" || op == "jnle") return !zf && sf == of; if (op == "jge" || op == "jnl") return sf == of;
            if (op == "jl" || op == "jnge") return sf != of; if (op == "jle" || op == "jng") return zf || sf != of; if (op == "js") return sf; if (op == "jns") return !sf; if (op == "jo") return of; if (op == "jno") return !of; if (op == "jp" || op == "jpe") return pf; if (op == "jnp" || op == "jpo") return !pf;
            return std::nullopt;
        }

        const char* mnemonicDescription(const std::string_view mnemonic) noexcept
        {
            if (mnemonic == "mov") return ui::i18n::tr("re.asmMov"); if (mnemonic == "movzx") return ui::i18n::tr("re.asmMovzx"); if (mnemonic == "movsx" || mnemonic == "movsxd") return ui::i18n::tr("re.asmMovsx"); if (mnemonic == "lea") return ui::i18n::tr("re.asmLea"); if (mnemonic == "xchg") return ui::i18n::tr("re.asmXchg"); if (mnemonic == "cmp") return ui::i18n::tr("re.asmCmp"); if (mnemonic == "test") return ui::i18n::tr("re.asmTest"); if (mnemonic == "add" || mnemonic == "adc") return ui::i18n::tr("re.asmAdd"); if (mnemonic == "sub" || mnemonic == "sbb") return ui::i18n::tr("re.asmSub"); if (mnemonic == "inc") return ui::i18n::tr("re.asmInc"); if (mnemonic == "dec") return ui::i18n::tr("re.asmDec"); if (mnemonic == "and") return ui::i18n::tr("re.asmAnd"); if (mnemonic == "or") return ui::i18n::tr("re.asmOr"); if (mnemonic == "xor") return ui::i18n::tr("re.asmXor"); if (mnemonic == "not") return ui::i18n::tr("re.asmNot"); if (mnemonic == "neg") return ui::i18n::tr("re.asmNeg");
            if (mnemonic == "imul") return ui::i18n::tr("re.asmImul"); if (mnemonic == "mul") return ui::i18n::tr("re.asmMul"); if (mnemonic == "idiv") return ui::i18n::tr("re.asmIdiv"); if (mnemonic == "div") return ui::i18n::tr("re.asmDiv"); if (mnemonic == "shl" || mnemonic == "sal") return ui::i18n::tr("re.asmShl"); if (mnemonic == "shr") return ui::i18n::tr("re.asmShr"); if (mnemonic == "sar") return ui::i18n::tr("re.asmSar"); if (mnemonic == "call") return ui::i18n::tr("re.asmCall"); if (mnemonic == "ret" || mnemonic == "retf") return ui::i18n::tr("re.asmRet"); if (mnemonic == "jmp") return ui::i18n::tr("re.asmJmp"); if (!mnemonic.empty() && mnemonic.front() == 'j') return ui::i18n::tr("re.asmJcc"); if (mnemonic.starts_with("cmov")) return ui::i18n::tr("re.asmCmov"); if (mnemonic.starts_with("set")) return ui::i18n::tr("re.asmSetcc"); if (mnemonic == "push") return ui::i18n::tr("re.asmPush"); if (mnemonic == "pop") return ui::i18n::tr("re.asmPop"); if (mnemonic == "leave") return ui::i18n::tr("re.asmLeave"); if (mnemonic == "enter") return ui::i18n::tr("re.asmEnter"); if (mnemonic == "int" || mnemonic == "int3") return ui::i18n::tr("re.asmInt"); if (mnemonic == "syscall" || mnemonic == "sysenter" || mnemonic == "sysexit") return ui::i18n::tr("re.asmSyscall"); if (mnemonic.starts_with("loop")) return ui::i18n::tr("re.asmLoop"); if (mnemonic == "bt" || mnemonic == "bts" || mnemonic == "btr" || mnemonic == "btc") return ui::i18n::tr("re.asmBitTest"); if (mnemonic == "bswap") return ui::i18n::tr("re.asmBswap"); if (mnemonic == "nop") return ui::i18n::tr("re.asmNop"); return nullptr;
        }

        const char* mnemonicReadableName(const std::string_view mnemonic) noexcept
        {
            if (mnemonic=="mov") return ui::i18n::tr("re.asmNameMov"); if (mnemonic=="movzx") return ui::i18n::tr("re.asmNameMovzx"); if (mnemonic=="movsx"||mnemonic=="movsxd") return ui::i18n::tr("re.asmNameMovsx"); if (mnemonic=="lea") return ui::i18n::tr("re.asmNameLea"); if (mnemonic=="xchg") return ui::i18n::tr("re.asmNameXchg"); if (mnemonic=="cmp") return ui::i18n::tr("re.asmNameCmp"); if (mnemonic=="test") return ui::i18n::tr("re.asmNameTest"); if (mnemonic=="add"||mnemonic=="adc") return ui::i18n::tr("re.asmNameAdd"); if (mnemonic=="sub"||mnemonic=="sbb") return ui::i18n::tr("re.asmNameSub"); if (mnemonic=="inc") return ui::i18n::tr("re.asmNameInc"); if (mnemonic=="dec") return ui::i18n::tr("re.asmNameDec"); if (mnemonic=="and") return ui::i18n::tr("re.asmNameAnd"); if (mnemonic=="or") return ui::i18n::tr("re.asmNameOr"); if (mnemonic=="xor") return ui::i18n::tr("re.asmNameXor"); if (mnemonic=="not") return ui::i18n::tr("re.asmNameNot"); if (mnemonic=="neg") return ui::i18n::tr("re.asmNameNeg"); if (mnemonic=="mul") return ui::i18n::tr("re.asmNameMul"); if (mnemonic=="imul") return ui::i18n::tr("re.asmNameImul"); if (mnemonic=="div") return ui::i18n::tr("re.asmNameDiv"); if (mnemonic=="idiv") return ui::i18n::tr("re.asmNameIdiv"); if (mnemonic=="shl"||mnemonic=="sal") return ui::i18n::tr("re.asmNameShl"); if (mnemonic=="shr") return ui::i18n::tr("re.asmNameShr"); if (mnemonic=="sar") return ui::i18n::tr("re.asmNameSar"); if (mnemonic=="call") return ui::i18n::tr("re.asmNameCall"); if (mnemonic=="ret"||mnemonic=="retf") return ui::i18n::tr("re.asmNameRet"); if (mnemonic=="jmp") return ui::i18n::tr("re.asmNameJmp"); if (!mnemonic.empty()&&mnemonic.front()=='j') return ui::i18n::tr("re.asmNameJcc"); if (mnemonic.starts_with("cmov")) return ui::i18n::tr("re.asmNameCmov"); if (mnemonic.starts_with("set")) return ui::i18n::tr("re.asmNameSetcc"); if (mnemonic=="push") return ui::i18n::tr("re.asmNamePush"); if (mnemonic=="pop") return ui::i18n::tr("re.asmNamePop"); if (mnemonic=="leave") return ui::i18n::tr("re.asmNameLeave"); if (mnemonic=="enter") return ui::i18n::tr("re.asmNameEnter"); if (mnemonic=="int"||mnemonic=="int3") return ui::i18n::tr("re.asmNameInt"); if (mnemonic=="syscall"||mnemonic=="sysenter"||mnemonic=="sysexit") return ui::i18n::tr("re.asmNameSyscall"); if (mnemonic.starts_with("loop")) return ui::i18n::tr("re.asmNameLoop"); if (mnemonic=="bt"||mnemonic=="bts"||mnemonic=="btr"||mnemonic=="btc") return ui::i18n::tr("re.asmNameBitTest"); if (mnemonic=="bswap") return ui::i18n::tr("re.asmNameBswap"); if (mnemonic=="nop") return ui::i18n::tr("re.asmNameNop"); return nullptr;
        }

        std::string tokenAt(const TextEditor& editor, const TextEditor::DocPos pos)
        {
            const std::string line = editor.GetLineText(pos.line); if (line.empty()) return {}; std::size_t index = std::min(pos.index, line.size()); if (index == line.size() && index) --index;
            const auto allowed = [](const unsigned char c) { return std::isalnum(c) || c == '_' || c == '$'; }; if (index < line.size() && !allowed(static_cast<unsigned char>(line[index])) && index && allowed(static_cast<unsigned char>(line[index - 1]))) --index; if (index >= line.size() || !allowed(static_cast<unsigned char>(line[index]))) return {};
            std::size_t begin = index, end = index + 1; while (begin && allowed(static_cast<unsigned char>(line[begin - 1]))) --begin; while (end < line.size() && allowed(static_cast<unsigned char>(line[end]))) ++end; return line.substr(begin, end - begin);
        }

        std::optional<std::size_t> instructionIndexForDisplayLine(const EnhancedMemoryInspectorState& ui, const std::size_t line)
        {
            if (line >= ui.DisplayLineAddresses.size() || ui.DisplayLineAddresses[line] == 0) return std::nullopt; const std::uintptr_t address = ui.DisplayLineAddresses[line];
            const auto it = std::ranges::find(ui.Instructions, address, &DisassemblyInstruction::Address); return it == ui.Instructions.end() ? std::nullopt : std::optional<std::size_t>(static_cast<std::size_t>(it - ui.Instructions.begin()));
        }

        std::optional<std::uintptr_t> displayAddressNearRow(const EnhancedMemoryInspectorState& ui, std::size_t row)
        {
            if (ui.DisplayLineAddresses.empty()) return std::nullopt; row = std::min(row,ui.DisplayLineAddresses.size()-1); if (ui.DisplayLineAddresses[row]) return ui.DisplayLineAddresses[row];
            for (std::size_t distance=1;distance<ui.DisplayLineAddresses.size();++distance)
            {
                if (row>=distance&&ui.DisplayLineAddresses[row-distance]) return ui.DisplayLineAddresses[row-distance]; if (row+distance<ui.DisplayLineAddresses.size()&&ui.DisplayLineAddresses[row+distance]) return ui.DisplayLineAddresses[row+distance];
            }
            return std::nullopt;
        }

        const RuntimeFunctionCandidate* functionForAddress(const RuntimeFunctionAnalysisSnapshot& analysis, const std::uintptr_t address)
        {
            const RuntimeFunctionCandidate* result = nullptr; for (const auto& candidate : analysis.Candidates) { if (candidate.Address > address) break; result = &candidate; } return result;
        }

        std::string functionName(EnhancedMemoryInspectorState& ui, const pid_t pid, const std::span<const RuntimeProcessModule> modules, const RuntimeFunctionCandidate& candidate)
        {
            if (const auto it = ui.FunctionNames.find({pid, candidate.Address}); it != ui.FunctionNames.end() && it->second[0]) return it->second.data();
            std::uintptr_t offset = candidate.Address; for (const auto& module : modules) if (module.contains(candidate.Address)) { offset = candidate.Address - module.Base; break; }
            std::ostringstream out; out << "fn_" << std::hex << std::uppercase << static_cast<unsigned long long>(offset); return out.str();
        }

        bool calculateDisassemblyWindow(const pid_t pid, const std::uintptr_t focus, const int viewportBytes, std::uintptr_t& base, std::size_t& size, std::uintptr_t& regionBase, std::uintptr_t& regionEnd)
        {
            if (pid <= 0 || focus == 0) return false;
            const auto regions = enumerateRuntimeRegions(pid);
            const auto region = std::ranges::find_if(regions, [&](const RuntimeProcessRegion& value) { return value.Readable && focus >= value.Base && focus < value.End; });
            if (region == regions.end() || region->End <= region->Base) return false;
            const std::size_t index = static_cast<std::size_t>(region - regions.begin()); regionBase = region->Base; regionEnd = region->End;
            for (std::size_t i=index;i>0;--i) { const auto& previous=regions[i-1]; if (!previous.Readable||previous.End!=regionBase) break; regionBase=previous.Base; }
            for (std::size_t i=index+1;i<regions.size();++i) { const auto& next=regions[i]; if (!next.Readable||next.Base!=regionEnd) break; regionEnd=next.End; }
            const std::size_t desired = static_cast<std::size_t>(std::clamp<std::uint64_t>(static_cast<std::uint64_t>(std::max(viewportBytes,16)) * 3ULL, 96ULL, 4096ULL));
            const std::uintptr_t before = static_cast<std::uintptr_t>(desired / 2);
            base = focus > regionBase + before ? focus - before : regionBase;
            std::uintptr_t end = base + desired; if (end < base || end > regionEnd) end = regionEnd;
            if (static_cast<std::size_t>(end - base) < desired && end == regionEnd && regionEnd - regionBase > desired) base = regionEnd - desired;
            size = static_cast<std::size_t>(std::min<std::uint64_t>(desired, regionEnd - base));
            return size != 0;
        }

        void applyDisassemblyMarkers(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); auto& probe = executionProbe(); const bool armed = probe.running() && probe.pid() == state.Pid; const std::uintptr_t armedAddress = armed ? probe.address() : 0; state.Disassembly.ClearMarkers();
            for (std::size_t line = 0; line < ui.DisplayLineAddresses.size(); ++line)
            {
                const std::uintptr_t address = ui.DisplayLineAddresses[line]; if (!address) continue;
                if (address == armedAddress) { const char* label=ui::i18n::tr("re.armedNextExecution"); state.Disassembly.AddMarker(line, IM_COL32(255,80,80,255), IM_COL32(180,30,30,82), label, label); continue; }
                if (ui.NavigationHighlightAddress==address)
                {
                    ImVec4 solid=ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive),fill=solid; solid.w=1.0f; fill.w=0.34f; const char* label=ui::i18n::tr("re.synchronizedMarker"); state.Disassembly.AddMarker(line,ImGui::ColorConvertFloat4ToU32(solid),ImGui::ColorConvertFloat4ToU32(fill),label,label); continue;
                }
                if (const auto marker = ui.Markers.find({state.Pid, address}); marker != ui.Markers.end()) { ImVec4 fill = marker->second.Color; fill.w = std::clamp(fill.w, 0.08f, 0.55f); const ImVec4 solid{fill.x,fill.y,fill.z,1.0f}; const std::string_view tag = marker->second.Tag.data(); state.Disassembly.AddMarker(line, ImGui::ColorConvertFloat4ToU32(solid), ImGui::ColorConvertFloat4ToU32(fill), tag, tag); continue; }
                if (ui.SynchronizeAddress && ui.SyncedAddress == address) { ImVec4 solid=ImGui::GetStyleColorVec4(ImGuiCol_Header),fill=solid; solid.w=0.92f; fill.w=0.18f; const char* label=ui::i18n::tr("re.synchronizedMarker"); state.Disassembly.AddMarker(line,ImGui::ColorConvertFloat4ToU32(solid),ImGui::ColorConvertFloat4ToU32(fill),label,label); }
            }
        }

        void rebuildDisassembly(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); const RuntimeX86Mode mode = runtimeProcessX86Mode(state.Pid); const auto modules = runtimeProcessModules(state.Pid); ui.DisplayMode=mode; ui.DisplayModules=modules; if (ui.FlowPid!=state.Pid) { ui.FlowGraph.clear(); ui.FlowPreviewCache.clear(); ui.FlowPid=state.Pid; }
            auto analysis = runtimeFunctionAnalysisSnapshot(state.Pid, state.Address); ui.FunctionAnalysis = analysis; ui.FunctionRevision = analysis.Revision;
            std::ostringstream disassembly; ui.Instructions.clear(); ui.DisplayLineAddresses.clear(); ui.DisplayLineByAddress.clear(); std::size_t offset = 0;
            while (offset < state.Original.size())
            {
                const std::uintptr_t address = ui.BufferBase + offset; RuntimeDecodedInstruction decoded; std::size_t length = 1;
                if (runtimeDecodeProcessInstruction(mode, std::span<const std::uint8_t>(state.Original).subspan(offset), address, decoded) && decoded.Length) length = decoded.Length;
                else { std::ostringstream fallback; fallback << "db 0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<unsigned>(state.Original[offset]); decoded.Text = fallback.str(); decoded.Length = 1; }
                if (decoded.Branch == RuntimeBranchKind::Call && decoded.Target) runtimeObserveFunctionTarget(state.Pid, *decoded.Target, RuntimeFunctionCandidateSource::CallTarget);
                else if ((decoded.Branch == RuntimeBranchKind::Conditional || decoded.Branch == RuntimeBranchKind::Unconditional) && decoded.Target) runtimeObserveFunctionTarget(state.Pid, *decoded.Target, RuntimeFunctionCandidateSource::EndBranchTarget);
                if (decoded.Target && decoded.Branch != RuntimeBranchKind::None && decoded.Branch != RuntimeBranchKind::Return)
                {
                    const auto space=decoded.Text.find_first_of(" \t"); CachedControlFlow flow; flow.Source=address; flow.Branch=decoded.Branch; flow.Target=decoded.Target; flow.FallThrough=decoded.Branch==RuntimeBranchKind::Conditional?std::optional<std::uintptr_t>(address+length):std::nullopt; flow.Mnemonic=lower(decoded.Text.substr(0,space)); ui.FlowGraph[address]=std::move(flow);
                }
                else ui.FlowGraph.erase(address);
                ui.Instructions.push_back({address, length, decoded, 0}); offset += length;
            }
            while (ui.FlowGraph.size()>8192) ui.FlowGraph.erase(ui.FlowGraph.begin());
            analysis = runtimeFunctionAnalysisSnapshot(state.Pid, state.Address); ui.FunctionAnalysis = analysis; ui.FunctionRevision = analysis.Revision;
            const auto appendBoundary=[&](const std::string& name,const char* label,const std::string& metadata)
            {
                disassembly << "; ────────────────────────────────────────────────────────────────\n"; ui.DisplayLineAddresses.push_back(0);
                disassembly << "; " << name << "    " << label; if (!metadata.empty()) disassembly << "    " << metadata; disassembly << '\n'; ui.DisplayLineAddresses.push_back(0);
                disassembly << "; ────────────────────────────────────────────────────────────────\n"; ui.DisplayLineAddresses.push_back(0);
            };
            for (auto& instruction : ui.Instructions)
            {
                const auto candidateIt = std::ranges::find(ui.FunctionAnalysis.Candidates, instruction.Address, &RuntimeFunctionCandidate::Address); const RuntimeFunctionCandidate* candidate = candidateIt == ui.FunctionAnalysis.Candidates.end() ? nullptr : &*candidateIt;
                if (candidate)
                {
                    const std::string name=functionName(ui,state.Pid,modules,*candidate); std::ostringstream metadata; metadata << '[' << runtimeFunctionCandidateSourceName(candidate->Source) << ", " << static_cast<int>(candidate->Confidence*100.0f) << "%]"; appendBoundary(name,candidate->Source==RuntimeFunctionCandidateSource::Prologue?ui::i18n::tr("re.functionPrologue"):ui::i18n::tr("re.functionStart"),metadata.str());
                }
                instruction.DisplayLine = ui.DisplayLineAddresses.size(); ui.DisplayLineAddresses.push_back(instruction.Address); ui.DisplayLineByAddress[instruction.Address]=instruction.DisplayLine;
                std::string text = instruction.Decoded.Text; if (instruction.Decoded.Target && instruction.Decoded.Branch != RuntimeBranchKind::None)
                {
                    const std::size_t space = text.find_first_of(" \t"); const std::string op = space == std::string::npos ? text : text.substr(0, space); const auto targetCandidate = std::ranges::find(ui.FunctionAnalysis.Candidates, *instruction.Decoded.Target, &RuntimeFunctionCandidate::Address);
                    const std::string target = targetCandidate != ui.FunctionAnalysis.Candidates.end() ? functionName(ui, state.Pid, modules, *targetCandidate) + "  ; " + processAddressText(mode,modules,*instruction.Decoded.Target) : processAddressText(mode,modules,*instruction.Decoded.Target); text = op + " " + target;
                }
                disassembly << text;
                if (const auto marker = ui.Markers.find({state.Pid, instruction.Address}); marker != ui.Markers.end()) { const std::string_view tag = marker->second.Tag.data(); disassembly << " ; [" << (tag.empty() ? "bookmark" : tag) << ']'; }
                if (executionProbe().running() && executionProbe().pid() == state.Pid && executionProbe().address() == instruction.Address) disassembly << " ; [" << ui::i18n::tr("re.armedNextExecution") << ']'; disassembly << '\n';
                if (instruction.Decoded.Branch==RuntimeBranchKind::Return)
                {
                    if (const RuntimeFunctionCandidate* owner=functionForAddress(ui.FunctionAnalysis,instruction.Address)) appendBoundary(functionName(ui,state.Pid,modules,*owner),ui::i18n::tr("re.functionEnd"),{});
                }
            }
            state.Disassembly.SetText(disassembly.str()); applyDisassemblyMarkers(state); ui.LastProbeRunning = executionProbe().running(); ui.LastProbePid = executionProbe().pid(); ui.LastProbeAddress = executionProbe().address(); ui.DisassemblyDirty = false;
            if (ui.PendingScrollAnchor)
            {
                const auto it = std::ranges::find(ui.DisplayLineAddresses, *ui.PendingScrollAnchor); if (it != ui.DisplayLineAddresses.end()) state.Disassembly.ScrollToLine(static_cast<std::size_t>(it - ui.DisplayLineAddresses.begin()), ui.PendingScrollAlignment); ui.PendingScrollAnchor.reset(); ui.PendingScrollAlignment=TextEditor::Scroll::alignTop;
            }
        }

        bool refreshDisassemblyBytes(RuntimeMemoryInspectorState& state, const bool resetPatchEditor, const bool recenter = true)
        {
            auto& ui = enhancedMemoryInspectorState(); if (state.Pid <= 0 || state.Address == 0 || state.ReadSize <= 0) return false;
            if (recenter || ui.BufferBase == 0 || ui.BufferSize == 0)
            {
                std::uintptr_t base = 0, regionBase = 0, regionEnd = 0; std::size_t size = 0;
                if (!calculateDisassemblyWindow(state.Pid,state.Address,state.ReadSize,base,size,regionBase,regionEnd)) { state.Status = "target address is not in readable memory"; return false; }
                ui.BufferBase=base; ui.BufferSize=size; ui.BufferRegionBase=regionBase; ui.BufferRegionEnd=regionEnd;
            }
            std::vector<std::uint8_t> bytes(ui.BufferSize); std::string error;
            if (!readProcessMemoryBlock(state.Pid, ui.BufferBase, bytes, error)) { state.Status = "memory read failed: " + error; return false; }
            state.Original = std::move(bytes); if (resetPatchEditor) { state.Patched = state.Original; const std::string formatted = runtimeFormatHexBytes(state.Original); std::snprintf(state.HexEdit.data(), state.HexEdit.size(), "%s", formatted.c_str()); state.WriteConfirm = 0; }
            configureDisassemblyEditor(state); rebuildDisassembly(state); ui.LastDisassemblyRefresh = runtimeSteadySeconds(); return true;
        }

        bool refreshRawBytes(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); if (state.Pid <= 0 || ui.BufferBase == 0 || ui.BufferSize == 0) return false; ui.RawBytes.assign(ui.BufferSize, 0); std::string error;
            if (!readProcessMemoryBlock(state.Pid, ui.BufferBase, ui.RawBytes, error)) { ui.RawBytes.clear(); return false; } ui.LastRawRefresh = runtimeSteadySeconds(); return true;
        }

        void setInspectorAddress(RuntimeMemoryInspectorState& state, const std::uintptr_t address, const std::optional<std::uintptr_t> scrollAnchor = std::nullopt, const TextEditor::Scroll alignment = TextEditor::Scroll::alignTop)
        {
            auto& ui = enhancedMemoryInspectorState(); if (scrollAnchor) { ui.PendingScrollAnchor = scrollAnchor; ui.PendingScrollAlignment=alignment; } state.Address = address; ui.AddressTextValue = std::numeric_limits<std::uintptr_t>::max(); syncInspectorAddressText(state); refreshEnhancedRuntimeMemoryInspector(state);
        }

        void navigateInspector(RuntimeMemoryInspectorState& state, const std::uintptr_t address, const bool recordHistory = true)
        {
            auto& ui = enhancedMemoryInspectorState(); if (address == 0 || address == state.Address) { if (address) { ui.NavigationHighlightAddress=address; if (ui.SynchronizeAddress) ui.SyncedAddress=address; applyDisassemblyMarkers(state); } return; }
            if (recordHistory && state.Address != 0)
            {
                if (ui.NavigationBack.empty() || ui.NavigationBack.back() != state.Address) ui.NavigationBack.push_back(state.Address); if (ui.NavigationBack.size() > 128) ui.NavigationBack.erase(ui.NavigationBack.begin()); ui.NavigationForward.clear();
            }
            ui.NavigationHighlightAddress=address; if (ui.SynchronizeAddress) ui.SyncedAddress=address; setInspectorAddress(state,address,address,TextEditor::Scroll::alignMiddle);
        }

        bool navigateBack(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); if (ui.NavigationBack.empty()) return false; const std::uintptr_t target=ui.NavigationBack.back(); ui.NavigationBack.pop_back(); if (state.Address) ui.NavigationForward.push_back(state.Address); ui.NavigationHighlightAddress=target; if (ui.SynchronizeAddress) ui.SyncedAddress=target; setInspectorAddress(state,target,target,TextEditor::Scroll::alignMiddle); return true;
        }

        bool navigateForward(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); if (ui.NavigationForward.empty()) return false; const std::uintptr_t target=ui.NavigationForward.back(); ui.NavigationForward.pop_back(); if (state.Address) ui.NavigationBack.push_back(state.Address); ui.NavigationHighlightAddress=target; if (ui.SynchronizeAddress) ui.SyncedAddress=target; setInspectorAddress(state,target,target,TextEditor::Scroll::alignMiddle); return true;
        }

        std::string groupedByteText(const std::span<const std::uint8_t> bytes, const std::size_t offset, const std::size_t count)
        {
            std::ostringstream out; for (std::size_t i=0;i<16;++i) { if (i<count) out << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[offset+i]); else out << "  "; if (i+1<16) out << (i==7?"  ":" "); } return out.str();
        }

        std::string groupedAsciiText(const std::span<const std::uint8_t> bytes, const std::size_t offset, const std::size_t count)
        {
            std::string out; out.reserve(16); for (std::size_t i=0;i<count;++i) { const unsigned char c=bytes[offset+i]; out.push_back(std::isprint(c)?static_cast<char>(c):'.'); } return out;
        }

        std::string decodedPreview(const RuntimeX86Mode mode, const std::uintptr_t address, const std::span<const std::uint8_t> bytes, const pid_t pid)
        {
            const auto modules = runtimeProcessModules(pid); std::ostringstream out; std::size_t offset = 0;
            while (offset < bytes.size())
            {
                RuntimeDecodedInstruction decoded; if (!runtimeDecodeProcessInstruction(mode, bytes.subspan(offset), address + offset, decoded) || !decoded.Length) { out << processAddressText(mode,modules,address+offset) << "  db 0x" << std::hex << std::uppercase << static_cast<unsigned>(bytes[offset]) << '\n'; ++offset; continue; }
                out << processAddressText(mode,modules,address+offset) << "  " << decoded.Text; if (decoded.Target) out << "  ; -> " << processAddressText(mode,modules,*decoded.Target); out << '\n'; offset += decoded.Length;
            }
            return out.str();
        }

        std::string instructionPreviewAt(RuntimeMemoryInspectorState& state, const std::uintptr_t address)
        {
            auto& ui=enhancedMemoryInspectorState(); if (const auto cached=ui.FlowPreviewCache.find(address);cached!=ui.FlowPreviewCache.end()) return cached->second; if (const auto found=std::ranges::find(ui.Instructions,address,&DisassemblyInstruction::Address);found!=ui.Instructions.end()) return ui.FlowPreviewCache.emplace(address,found->Decoded.Text).first->second;
            std::vector<std::uint8_t> bytes(16); std::string error; if (!readProcessMemoryBlock(state.Pid,address,bytes,error)) return ui.FlowPreviewCache.emplace(address,"<unreadable>").first->second; RuntimeDecodedInstruction decoded; if (!runtimeDecodeProcessInstruction(runtimeProcessX86Mode(state.Pid),bytes,address,decoded)||!decoded.Length) return ui.FlowPreviewCache.emplace(address,"db ?").first->second; if (ui.FlowPreviewCache.size()>2048) ui.FlowPreviewCache.clear(); return ui.FlowPreviewCache.emplace(address,decoded.Text).first->second;
        }

        void rebuildAssemblerPreview(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); auto& assembler = ui.Assembler; assembler.Error.clear(); assembler.Assembled.clear(); assembler.Patch.clear(); assembler.PreviewText.clear(); assembler.PreviewValid = false; assembler.ConfirmArmed = false;
            if (!assembler.Active || assembler.Address == 0 || assembler.Original.empty()) return;
            if (assembler.NopMode) assembler.Assembled.assign(assembler.Span, 0x90);
            else if (!runtimeAssembleInstructionText(runtimeProcessX86Mode(state.Pid), assembler.Address, assembler.Editor.GetText(), assembler.Assembled, assembler.Error)) return;
            std::size_t requiredSpan = assembler.Span;
            if (assembler.Assembled.size() > requiredSpan)
            {
                if (!runtimeConfiguration().AssemblerConsumeFollowing) { assembler.Error = "assembled code is " + std::to_string(assembler.Assembled.size()) + " bytes but the selected instruction span is only " + std::to_string(requiredSpan) + " bytes"; return; }
                std::size_t last = assembler.LastInstruction; while (assembler.Assembled.size() > requiredSpan && last + 1 < ui.Instructions.size()) { ++last; requiredSpan = static_cast<std::size_t>((ui.Instructions[last].Address + ui.Instructions[last].Length) - assembler.Address); }
                if (assembler.Assembled.size() > requiredSpan) { assembler.Error = "assembled code still exceeds all complete following instructions in the loaded block"; return; }
                assembler.LastInstruction = last; assembler.Span = requiredSpan; assembler.Original.resize(requiredSpan); std::string error; if (!readProcessMemoryBlock(state.Pid, assembler.Address, assembler.Original, error)) { assembler.Error = "could not expand patch span: " + error; return; }
            }
            assembler.Patch = assembler.Assembled; if (assembler.Patch.size() < assembler.Span && runtimeConfiguration().AssemblerFillNops) assembler.Patch.resize(assembler.Span, 0x90);
            assembler.PreviewText = decodedPreview(runtimeProcessX86Mode(state.Pid), assembler.Address, assembler.Patch, state.Pid); assembler.PreviewValid = true; assembler.Dirty = false;
        }

        void openAssembler(RuntimeMemoryInspectorState& state, std::size_t first, std::size_t last, const bool nops)
        {
            auto& ui = enhancedMemoryInspectorState(); if (ui.Instructions.empty()) return; first = std::min(first, ui.Instructions.size() - 1); last = std::min(std::max(last, first), ui.Instructions.size() - 1); auto& assembler = ui.Assembler; configureAssemblerEditor(assembler);
            assembler.Active = true; assembler.NopMode = nops; assembler.FirstInstruction = first; assembler.LastInstruction = last; assembler.Address = ui.Instructions[first].Address; assembler.Span = static_cast<std::size_t>((ui.Instructions[last].Address + ui.Instructions[last].Length) - assembler.Address); assembler.Original.assign(assembler.Span, 0); assembler.Error.clear(); assembler.ConfirmArmed = false;
            std::string error; if (!readProcessMemoryBlock(state.Pid, assembler.Address, assembler.Original, error)) { assembler.Error = "could not read patch span: " + error; assembler.PreviewValid = false; }
            std::ostringstream source; for (std::size_t i = first; i <= last; ++i) source << ui.Instructions[i].Decoded.Text << (i == last ? "" : "\n"); assembler.Editor.SetText(nops ? "nop" : source.str()); assembler.Dirty = true; assembler.LastEdit = 0.0; rebuildAssemblerPreview(state); ui.SelectAssemblerTab = true;
        }

        bool writeAssemblerPatch(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); auto& assembler = ui.Assembler; if (!assembler.PreviewValid || assembler.Patch.empty()) return false; std::string error;
            if (runtimeConfiguration().AssemblerVerifyBeforeWrite)
            {
                std::vector<std::uint8_t> current(assembler.Original.size()); if (!readProcessMemoryBlock(state.Pid, assembler.Address, current, error)) { assembler.Error = "verification read failed: " + error; return false; }
                if (current != assembler.Original) { assembler.Error = "process bytes changed since this patch span was opened; refresh/reopen before writing"; assembler.ConfirmArmed = false; return false; }
            }
            if (runtimeConfiguration().AssemblerRequireConfirmation && !assembler.ConfirmArmed) { assembler.ConfirmArmed = true; assembler.Error = "write armed; click Confirm patch to modify process memory"; return false; }
            const std::size_t writeSize = assembler.Patch.size(); std::vector<std::uint8_t> originalPrefix(assembler.Original.begin(), assembler.Original.begin() + std::min(writeSize, assembler.Original.size()));
            if (!runtimeWriteProcessMemory(state.Pid, assembler.Address, assembler.Patch, error)) { assembler.Error = "patch write failed: " + error; assembler.ConfirmArmed = false; return false; }
            if (runtimeConfiguration().AssemblerKeepOriginalBytes) ui.PatchHistory.push_back({state.Pid, assembler.Address, std::move(originalPrefix), assembler.Patch, assembler.NopMode ? "NOP patch" : "assembled patch"});
            assembler.Error = "patch written"; assembler.ConfirmArmed = false; if (runtimeConfiguration().AssemblerAutoRedisassemble) refreshDisassemblyBytes(state, true, false); return true;
        }

        void drawAssemblerTweaks()
        {
            auto& config = runtimeConfiguration(); bool changed = false; changed |= ImGui::Checkbox(ui::i18n::tr("configuration.fillNops"), &config.AssemblerFillNops); changed |= ImGui::Checkbox(ui::i18n::tr("configuration.wholeInstructions"), &config.AssemblerWholeInstructions); changed |= ImGui::Checkbox(ui::i18n::tr("configuration.consumeFollowing"), &config.AssemblerConsumeFollowing); changed |= ImGui::Checkbox(ui::i18n::tr("configuration.verifyBeforeWrite"), &config.AssemblerVerifyBeforeWrite); changed |= ImGui::Checkbox(ui::i18n::tr("configuration.requireConfirmation"), &config.AssemblerRequireConfirmation); changed |= ImGui::Checkbox(ui::i18n::tr("configuration.redisassemble"), &config.AssemblerAutoRedisassemble); changed |= ImGui::Checkbox(ui::i18n::tr("configuration.keepOriginal"), &config.AssemblerKeepOriginalBytes); if (changed) saveRuntimeConfiguration();
        }

        void drawAssembler(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); auto& assembler = ui.Assembler; configureAssemblerEditor(assembler);
            if (!assembler.Active) { ImGui::TextDisabled("%s",ui::i18n::tr("re.assemblerEmpty")); return; }
            const auto modules = runtimeProcessModules(state.Pid); const RuntimeX86Mode mode=runtimeProcessX86Mode(state.Pid); ImGui::Text(ui::i18n::tr("re.patchAt"), processAddressText(mode,modules,assembler.Address).c_str(), assembler.Span); ImGui::SameLine(); if (assembler.NopMode) ImGui::TextColored(ImVec4(0.95f,0.72f,0.28f,1.0f), "%s",ui::i18n::tr("re.nopMode"));
            if (assembler.Editor.Render("##AssemblerEditor", ImVec2(-1.0f, 118.0f), ImGuiChildFlags_Borders)) { assembler.NopMode = false; assembler.Dirty = true; assembler.LastEdit = runtimeSteadySeconds(); assembler.ConfirmArmed = false; }
            const double now = runtimeSteadySeconds(); if (assembler.Dirty && (assembler.LastEdit == 0.0 || now - assembler.LastEdit >= 0.25)) rebuildAssemblerPreview(state);
            if (assembler.PreviewValid)
            {
                ImGui::SeparatorText(ui::i18n::tr("re.encodingPreview")); ImGui::TextDisabled(ui::i18n::tr("re.originalBytes"), runtimeFormatHexBytes(assembler.Original).c_str()); ImGui::TextWrapped(ui::i18n::tr("re.patchBytesValue"), runtimeFormatHexBytes(assembler.Patch).c_str());
                if (assembler.Assembled.size() < assembler.Span && !runtimeConfiguration().AssemblerFillNops) ImGui::TextColored(ImVec4(0.95f,0.72f,0.28f,1.0f), "%s",ui::i18n::tr("re.shortPatchWarning"));
                ImGui::BeginChild("##AssemblerPreview", ImVec2(0.0f, 120.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar); ImGui::TextUnformatted(assembler.PreviewText.c_str()); ImGui::EndChild();
            }
            if (!assembler.Error.empty()) ImGui::TextWrapped("%s", assembler.Error.c_str());
            ImGui::BeginDisabled(!assembler.PreviewValid); const char* label = runtimeConfiguration().AssemblerRequireConfirmation && assembler.ConfirmArmed ? ui::i18n::tr("re.confirmPatch") : ui::i18n::tr("re.writePatch"); if (ImGui::Button(label)) writeAssemblerPatch(state); ImGui::EndDisabled(); ImGui::SameLine(); if (ImGui::Button(ui::i18n::tr("re.rebuildPreview"))) rebuildAssemblerPreview(state); ImGui::SameLine(); if (ImGui::Button(ui::i18n::tr("common.cancel"))) { assembler.Active = false; assembler.ConfirmArmed = false; }
        }

        void drawPatchHistory(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); if (ui.PatchHistory.empty()) { ImGui::TextDisabled("%s",ui::i18n::tr("re.noPatchHistory")); return; }
            for (std::size_t i = ui.PatchHistory.size(); i-- > 0;)
            {
                auto& patch = ui.PatchHistory[i]; if (patch.Pid != state.Pid) continue; ImGui::PushID(static_cast<int>(i)); const std::string address = runtimeFormatProcessAddress(state.Pid, patch.Address); ImGui::Text("%s  %s", address.c_str(), patch.Label.c_str()); ImGui::SameLine();
                if (ImGui::SmallButton(ui::i18n::tr("re.restore"))) { std::string error; if (runtimeWriteProcessMemory(patch.Pid, patch.Address, patch.Original, error)) { state.Status = "restored original patch bytes"; if (runtimeConfiguration().AssemblerAutoRedisassemble) refreshDisassemblyBytes(state, true, false); } else state.Status = "restore failed: " + error; }
                ImGui::SameLine(); if (ImGui::SmallButton(ui::i18n::tr("re.copyAddress"))) ImGui::SetClipboardText(address.c_str()); ImGui::PopID();
            }
        }

        void drawRegisterSnapshot(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); if (!ui.HasCapturedHit || ui.CapturedHit.Pid != state.Pid || !ui.CapturedHit.HasRegisters) { ImGui::TextDisabled("%s",ui::i18n::tr("re.noRegisterCapture")); return; }
            const RuntimeX86Mode mode = runtimeProcessX86Mode(state.Pid); const auto registers = registerValues(ui.CapturedHit, mode); const auto modules = runtimeProcessModules(state.Pid); ImGui::TextDisabled(ui::i18n::tr("re.capturedAt"), processAddressText(mode,modules,ui.CapturedHit.Address).c_str(), ui.CapturedHit.Tid, mode == RuntimeX86Mode::X86 ? "32-bit" : "64-bit");
            if (!ImGui::BeginTable("ExecutionProbeRegisters", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 235.0f))) return;
            ImGui::TableSetupColumn(ui::i18n::tr("re.register"), ImGuiTableColumnFlags_WidthFixed, 90.0f); ImGui::TableSetupColumn(ui::i18n::tr("re.value")); ImGui::TableSetupColumn(ui::i18n::tr("re.register"), ImGuiTableColumnFlags_WidthFixed, 90.0f); ImGui::TableSetupColumn(ui::i18n::tr("re.value")); ImGui::TableHeadersRow();
            for (std::size_t pair = 0; pair < (registers.size() + 1) / 2; ++pair)
            {
                ImGui::TableNextRow(); for (std::size_t side = 0; side < 2; ++side)
                {
                    const std::size_t index = pair + side * ((registers.size() + 1) / 2); ImGui::TableNextColumn(); if (index >= registers.size()) { ImGui::TableNextColumn(); continue; }
                    const auto& reg = registers[index]; ImGui::TextUnformatted(reg.Name); ImGui::TableNextColumn(); ImGui::PushID(static_cast<int>(index)); const std::string hex = runtimeFormatAbsoluteAddress(mode,static_cast<std::uintptr_t>(reg.Value)), symbolic = processAddressText(mode,modules,static_cast<std::uintptr_t>(reg.Value));
                    ImGui::Selectable(symbolic.c_str(), ui.SynchronizeAddress && ui.SyncedAddress == static_cast<std::uintptr_t>(reg.Value), ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeight()));
                    if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::Text("%s = %s",reg.Name,symbolic.c_str()); ImGui::TextDisabled(ui::i18n::tr("re.absoluteValue"),hex.c_str()); ImGui::TextDisabled(ui::i18n::tr("re.unsignedValue"),static_cast<unsigned long long>(reg.Value)); ImGui::TextDisabled(ui::i18n::tr("re.signedValue"),static_cast<long long>(reg.Value)); if (const char* description=registerDescription(reg.Name)) { ImGui::Separator(); ImGui::TextWrapped("%s",description); } ImGui::EndTooltip(); }
                    if (ImGui::BeginPopupContextItem("ExecutionRegisterContext")) { if (ImGui::MenuItem(ui::i18n::tr("re.copySymbolicAddress"))) ImGui::SetClipboardText(symbolic.c_str()); if (ImGui::MenuItem(ui::i18n::tr("re.copyHex"))) ImGui::SetClipboardText(hex.c_str()); if (ImGui::MenuItem(ui::i18n::tr("re.copyDecimal"))) { const std::string text = std::to_string(reg.Value); ImGui::SetClipboardText(text.c_str()); } ImGui::Separator(); ImGui::BeginDisabled(reg.Value == 0); if (ImGui::MenuItem(ui::i18n::tr("re.inspectValueAddress"))) ui.PendingInspectorAddress = static_cast<std::uintptr_t>(reg.Value); if (ImGui::MenuItem(ui::i18n::tr("re.synchronizeAddress"))) { ui.SyncedAddress = static_cast<std::uintptr_t>(reg.Value); ui.NavigationHighlightAddress=ui.SyncedAddress; applyDisassemblyMarkers(state); } ImGui::EndDisabled(); ImGui::EndPopup(); } ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }

        bool assemblyHoverAvailable(RuntimeMemoryInspectorState& state, TextEditor& editor, const bool mappedLines, const TextEditor::DocPos pos)
        {
            auto& ui=enhancedMemoryInspectorState(); const std::string token=tokenAt(editor,pos); if (!token.empty()&&knownRegister(token)) return true; const DisassemblyInstruction* instruction=nullptr; if (mappedLines) if (const auto index=instructionIndexForDisplayLine(ui,pos.line)) instruction=&ui.Instructions[*index]; std::string mnemonic; if (instruction) { const auto space=instruction->Decoded.Text.find_first_of(" \t"); mnemonic=lower(instruction->Decoded.Text.substr(0,space)); } else { const std::string line=editor.GetLineText(pos.line); const auto first=line.find_first_not_of(" \t"); if (first!=std::string::npos) { const auto end=line.find_first_of(" \t",first); mnemonic=lower(line.substr(first,end-first)); } } if ((!token.empty()&&lower(token)==mnemonic&&(mnemonicDescription(mnemonic)||mnemonicReadableName(mnemonic)))||(instruction&&instruction->Decoded.Target&&(instruction->Decoded.Branch==RuntimeBranchKind::Conditional||instruction->Decoded.Branch==RuntimeBranchKind::Unconditional||instruction->Decoded.Branch==RuntimeBranchKind::Call))) return true; return false;
        }

        bool drawAssemblyHoverContent(RuntimeMemoryInspectorState& state, TextEditor& editor, const bool mappedLines, const TextEditor::DocPos pos)
        {
            auto& ui = enhancedMemoryInspectorState(); const std::string token = tokenAt(editor,pos); bool rendered = false; const RuntimeX86Mode mode=runtimeProcessX86Mode(state.Pid); const auto modules = runtimeProcessModules(state.Pid);
            if (!token.empty() && knownRegister(token))
            {
                rendered = true; const std::string reg=upper(token); ImGui::TextUnformatted(reg.c_str()); if (const char* description=registerDescription(reg)) ImGui::TextWrapped("%s",description); ImGui::Separator();
                if (ui.HasCapturedHit && ui.CapturedHit.Pid == state.Pid && ui.CapturedHit.HasRegisters)
                {
                    const auto value = registerValue(ui.CapturedHit, token); if (value) { const std::string absolute = runtimeFormatAbsoluteAddress(mode,static_cast<std::uintptr_t>(*value)), symbolic = processAddressText(mode,modules,static_cast<std::uintptr_t>(*value)); ImGui::Text("%s", symbolic.c_str()); if (symbolic != absolute) ImGui::TextDisabled(ui::i18n::tr("re.absoluteValue"), absolute.c_str()); ImGui::TextDisabled(ui::i18n::tr("re.capturedAtShort"), processAddressText(mode,modules,ui.CapturedHit.Address).c_str(), ui.CapturedHit.Tid); }
                    else ImGui::TextDisabled("%s",ui::i18n::tr("re.uncapturedAlias"));
                }
                else { ImGui::TextDisabled("%s",ui::i18n::tr("re.uncaptured")); ImGui::TextWrapped("%s",ui::i18n::tr("re.captureRegisterHint")); }
            }
            const DisassemblyInstruction* instruction = nullptr; if (mappedLines) if (const auto index = instructionIndexForDisplayLine(ui,pos.line)) instruction = &ui.Instructions[*index];
            std::string mnemonic; if (instruction) { const auto space = instruction->Decoded.Text.find_first_of(" \t"); mnemonic = lower(instruction->Decoded.Text.substr(0, space)); } else { const std::string line = editor.GetLineText(pos.line); const auto first = line.find_first_not_of(" \t"); if (first != std::string::npos) { const auto end = line.find_first_of(" \t", first); mnemonic = lower(line.substr(first, end - first)); } }
            if (!token.empty() && lower(token) == mnemonic)
            {
                const char* readable=mnemonicReadableName(mnemonic); const char* description=mnemonicDescription(mnemonic); if (readable||description) { if (rendered) ImGui::Separator(); rendered=true; const std::string opcode=upper(mnemonic); if (readable) ImGui::Text("%s — %s",opcode.c_str(),readable); else ImGui::TextUnformatted(opcode.c_str()); if (description) ImGui::TextWrapped("%s",description); }
            }
            if (instruction && instruction->Decoded.Target && (instruction->Decoded.Branch == RuntimeBranchKind::Conditional || instruction->Decoded.Branch == RuntimeBranchKind::Unconditional || instruction->Decoded.Branch == RuntimeBranchKind::Call))
            {
                if (rendered) ImGui::Separator(); rendered = true; ImGui::Text(ui::i18n::tr("re.hoverTarget"), processAddressText(mode,modules,*instruction->Decoded.Target).c_str());
                if (instruction->Decoded.Branch == RuntimeBranchKind::Conditional)
                {
                    if (ui.HasCapturedHit && ui.CapturedHit.Pid == state.Pid && ui.CapturedHit.Address == instruction->Address)
                    {
                        if (const auto condition = branchCondition(mnemonic, ui.CapturedHit.Registers.eflags)) ImGui::TextColored(*condition ? ImVec4(0.35f,0.90f,0.55f,1.0f) : ImVec4(0.95f,0.40f,0.38f,1.0f), ui::i18n::tr("re.capturedCondition"), *condition ? ui::i18n::tr("common.true") : ui::i18n::tr("common.false"), *condition ? ui::i18n::tr("re.branchTaken") : ui::i18n::tr("re.fallThrough"));
                    }
                    else ImGui::TextDisabled("%s",ui::i18n::tr("re.conditionUncaptured"));
                }
            }
            return rendered;
        }

        void installAssemblyHover(RuntimeMemoryInspectorState& state, TextEditor& editor, const bool mappedLines)
        {
            editor.SetTextHoverCallback([&state,&editor,mappedLines](TextEditor::PopupData& data) { if (!drawAssemblyHoverContent(state,editor,mappedLines,data.pos)) ImGui::CloseCurrentPopup(); });
        }

        void drawDisassemblyHeader(RuntimeMemoryInspectorState& state)
        {
            auto& ui=enhancedMemoryInspectorState(); const ImGuiTableFlags flags=ImGuiTableFlags_Resizable|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_NoSavedSettings;
            if (ImGui::BeginTable("##DisassemblyColumnHeader",2,flags))
            {
                ImGui::TableSetupColumn(ui::i18n::tr("re.address"),ImGuiTableColumnFlags_WidthFixed,ui.AddressColumnWidth); ImGui::TableSetupColumn(ui::i18n::tr("re.instruction"),ImGuiTableColumnFlags_WidthStretch,1.0f); ImGui::TableHeadersRow();
                if (const ImGuiTable* table=ImGui::GetCurrentTable(); table&&table->ColumnsCount>=2) ui.AddressColumnWidth=std::clamp(table->Columns[0].WidthGiven,110.0f,520.0f); ImGui::EndTable();
            }
            const float glyph=std::max(ImGui::CalcTextSize("0").x,1.0f); const int reservedMargins=state.Disassembly.GetDecorationLeftMargin()+state.Disassembly.GetTextLeftMargin(); const std::size_t decoratorGlyphs=static_cast<std::size_t>(std::max(8.0f,std::floor(ui.AddressColumnWidth/glyph)-static_cast<float>(reservedMargins)));
            state.Disassembly.SetLineDecorator(decoratorGlyphs,[&state,&ui](TextEditor::Decorator& decorator)
            {
                if (decorator.line>=ui.DisplayLineAddresses.size()) return; const std::uintptr_t address=ui.DisplayLineAddresses[decorator.line]; if (!address) return; const std::string text=processAddressText(ui.DisplayMode,ui.DisplayModules,address); const ImVec2 pos=ImGui::GetCursorScreenPos(); auto* draw=ImGui::GetWindowDrawList(); draw->PushClipRect(pos,ImVec2(pos.x+decorator.width,pos.y+decorator.height),true); draw->AddText(ImVec2(pos.x,pos.y+std::max(0.0f,(decorator.height-ImGui::GetTextLineHeight())*0.5f)),state.Disassembly.GetPalette().get(TextEditor::Color::lineNumber),text.c_str()); draw->PopClipRect();
            });
        }

        void drawVerticalArrowHead(ImDrawList* draw, const ImVec2 tip, const bool downward, const ImU32 color)
        {
            const float s = 4.0f; if (downward) draw->AddTriangleFilled(tip, ImVec2(tip.x - s, tip.y - s * 1.5f), ImVec2(tip.x + s, tip.y - s * 1.5f), color); else draw->AddTriangleFilled(tip, ImVec2(tip.x - s, tip.y + s * 1.5f), ImVec2(tip.x + s, tip.y + s * 1.5f), color);
        }

        void drawLeftArrowHead(ImDrawList* draw, const ImVec2 tip, const ImU32 color)
        {
            const float s=4.0f; draw->AddTriangleFilled(tip,ImVec2(tip.x+s*1.6f,tip.y-s),ImVec2(tip.x+s*1.6f,tip.y+s),color);
        }

        float pointSegmentDistanceSquared(const ImVec2 point, const ImVec2 a, const ImVec2 b)
        {
            const float vx=b.x-a.x,vy=b.y-a.y,wx=point.x-a.x,wy=point.y-a.y,length=vx*vx+vy*vy; if (length<=0.0001f) return wx*wx+wy*wy; const float t=std::clamp((wx*vx+wy*vy)/length,0.0f,1.0f),dx=point.x-(a.x+t*vx),dy=point.y-(a.y+t*vy); return dx*dx+dy*dy;
        }

        float branchLineEndX(RuntimeMemoryInspectorState& state, const std::size_t row)
        {
            const std::string text=state.Disassembly.GetLineText(row); return state.Disassembly.GetDocPosScreenPos(TextEditor::DocPos(row,text.size())).x;
        }

        float branchRowCenterY(RuntimeMemoryInspectorState& state, const std::size_t row)
        {
            return state.Disassembly.GetDocPosScreenPos(TextEditor::DocPos(row,0)).y+state.Disassembly.GetLineHeight()*0.5f;
        }

        void drawFarControlFlowPopup(RuntimeMemoryInspectorState& state)
        {
            auto& ui=enhancedMemoryInspectorState(); if (ui.OpenFarFlowPopup) { ImGui::OpenPopup("##QuartzFarControlFlow"); ui.OpenFarFlowPopup=false; }
            if (!ImGui::BeginPopup("##QuartzFarControlFlow")) return; ImGui::SeparatorText(ui::i18n::tr("re.farFlow")); ImGui::TextDisabled(ui::i18n::tr("re.farFlowCount"),ui.FarFlowEntries.size()); const RuntimeX86Mode mode=runtimeProcessX86Mode(state.Pid); const auto modules=runtimeProcessModules(state.Pid);
            const ImGuiTableFlags flags=ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollX|ImGuiTableFlags_ScrollY|ImGuiTableFlags_SizingFixedFit;
            if (ImGui::BeginTable("##FarFlowTable",5,flags,ImVec2(760.0f,300.0f),1080.0f))
            {
                ImGui::TableSetupScrollFreeze(0,1); ImGui::TableSetupColumn(ui::i18n::tr("re.source"),ImGuiTableColumnFlags_WidthFixed,170.0f); ImGui::TableSetupColumn(ui::i18n::tr("re.sourceInstruction"),ImGuiTableColumnFlags_WidthFixed,260.0f); ImGui::TableSetupColumn(ui::i18n::tr("re.destination"),ImGuiTableColumnFlags_WidthFixed,170.0f); ImGui::TableSetupColumn(ui::i18n::tr("re.destinationInstruction"),ImGuiTableColumnFlags_WidthFixed,260.0f); ImGui::TableSetupColumn("##go",ImGuiTableColumnFlags_WidthFixed,100.0f); ImGui::TableHeadersRow(); ImGuiListClipper clipper; clipper.Begin(static_cast<int>(ui.FarFlowEntries.size())); while (clipper.Step()) for (int i=clipper.DisplayStart;i<clipper.DisplayEnd;++i)
                {
                    const auto& entry=ui.FarFlowEntries[static_cast<std::size_t>(i)]; const std::string source=processAddressText(mode,modules,entry.Source),destination=processAddressText(mode,modules,entry.Destination),sourcePreview=instructionPreviewAt(state,entry.Source),destinationPreview=instructionPreviewAt(state,entry.Destination); ImGui::PushID(i); ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(entry.Color),"%s",source.c_str()); ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(sourcePreview.c_str()); ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(destination.c_str()); ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(destinationPreview.c_str()); ImGui::TableSetColumnIndex(4); if (ImGui::SmallButton("S")) { ui.PendingInspectorAddress=entry.Source; ImGui::CloseCurrentPopup(); } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",ui::i18n::tr("re.goSource")); ImGui::SameLine(); if (ImGui::SmallButton("D")) { ui.PendingInspectorAddress=entry.Destination; ImGui::CloseCurrentPopup(); } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",ui::i18n::tr("re.goDestination")); ImGui::PopID();
                } ImGui::EndTable();
            }
            ImGui::EndPopup();
        }

        void drawBranchLanes(RuntimeMemoryInspectorState& state, const ImVec2 min, const ImVec2 max)
        {
            struct Route
            {
                std::optional<std::size_t> SourceRow,TargetRow;
                std::uintptr_t Source=0,Destination=0;
                bool SourceVisible=false,TargetVisible=false,TargetBelow=false,SourceDot=true,FallThrough=false;
                float SourceY=0.0f,TargetY=0.0f,SourceDock=0.0f,TargetDock=0.0f,DesiredLane=0.0f,LaneX=0.0f;
                ImU32 Color=0;
            };
            struct FarRoute { FarControlFlowEntry Entry; bool SourceVisible=false,TargetVisible=false,DestinationBelow=false; float SourceY=0.0f,TargetY=0.0f,SourceDock=0.0f,TargetDock=0.0f; };
            struct Reservation { float X=0.0f,Y0=0.0f,Y1=0.0f; };

            auto& ui=enhancedMemoryInspectorState(); if (ui.FlowGraph.empty()||ui.DisplayLineAddresses.empty()) return; const std::size_t firstRow=state.Disassembly.GetFirstVisibleRow(),lastRow=state.Disassembly.GetLastVisibleRow(); if (lastRow<firstRow) return; const auto firstAddress=displayAddressNearRow(ui,firstRow),lastAddress=displayAddressNearRow(ui,lastRow); if (!firstAddress||!lastAddress) return;
            const float top=min.y+6.0f,bottom=max.y-9.0f,right=max.x-24.0f,farRailX=max.x-11.0f,dockGap=15.0f,routeGap=32.0f,laneStep=19.0f,portOffset=3.0f; if (bottom<=top||right<=min.x+20.0f) return; const std::size_t visibleRows=std::max<std::size_t>(1,lastRow-firstRow+1);
            std::vector<Route> routes; routes.reserve(ui.FlowGraph.size()*2); std::vector<FarRoute> farRoutes;

            const auto rowFor=[&](const std::uintptr_t address)->std::optional<std::size_t> { const auto it=ui.DisplayLineByAddress.find(address); return it==ui.DisplayLineByAddress.end()?std::nullopt:std::optional<std::size_t>(it->second); };
            const auto classify=[&](const std::uintptr_t source,const std::uintptr_t destination,const ImU32 color,const RuntimeBranchKind branch,const std::string& mnemonic,const bool sourceDot,const bool fallThrough)
            {
                const auto sourceRow=rowFor(source),targetRow=rowFor(destination); const bool sourceVisible=sourceRow&&*sourceRow>=firstRow&&*sourceRow<=lastRow,targetVisible=targetRow&&*targetRow>=firstRow&&*targetRow<=lastRow; const bool sourceAbove=sourceRow?*sourceRow<firstRow:source<*firstAddress,sourceBelow=sourceRow?*sourceRow>lastRow:source>*lastAddress,targetAbove=targetRow?*targetRow<firstRow:destination<*firstAddress,targetBelow=targetRow?*targetRow>lastRow:destination>*lastAddress; if (!sourceVisible&&!targetVisible&&!((sourceAbove&&targetBelow)||(sourceBelow&&targetAbove))) return;
                const bool far=!fallThrough&&(!sourceRow||!targetRow||(sourceRow&&targetRow&&static_cast<std::size_t>(std::max(*sourceRow,*targetRow)-std::min(*sourceRow,*targetRow))>visibleRows*3));
                const float sourceY=std::clamp(sourceVisible?branchRowCenterY(state,*sourceRow)-portOffset:(sourceAbove?top:bottom),top,bottom),targetY=std::clamp(targetVisible?branchRowCenterY(state,*targetRow)+portOffset:(targetBelow?bottom:top),top,bottom); const float sourceDock=sourceVisible?branchLineEndX(state,*sourceRow)+dockGap:min.x+ui.AddressColumnWidth+dockGap,targetDock=targetVisible?branchLineEndX(state,*targetRow)+dockGap:sourceDock;
                if (far) { farRoutes.push_back({{source,destination,branch,mnemonic,color},sourceVisible,targetVisible,targetBelow,sourceY,targetY,sourceDock,targetDock}); return; }
                Route route; route.SourceRow=sourceRow; route.TargetRow=targetRow; route.Source=source; route.Destination=destination; route.SourceVisible=sourceVisible; route.TargetVisible=targetVisible; route.TargetBelow=targetBelow; route.SourceDot=sourceDot; route.FallThrough=fallThrough; route.Color=color; route.SourceY=sourceY; route.TargetY=targetY; route.SourceDock=sourceDock; route.TargetDock=targetDock; const std::size_t sourceScan=sourceRow?std::clamp(*sourceRow,firstRow,lastRow):(sourceAbove?firstRow:lastRow),targetScan=targetRow?std::clamp(*targetRow,firstRow,lastRow):(targetAbove?firstRow:lastRow); const std::size_t scanBegin=std::min(sourceScan,targetScan),scanEnd=std::max(sourceScan,targetScan); float desired=std::max(route.SourceDock,route.TargetDock)+routeGap; for (std::size_t row=scanBegin;row<=scanEnd&&row<state.Disassembly.GetLineCount();++row) { if (row<ui.DisplayLineAddresses.size()&&!ui.DisplayLineAddresses[row]) continue; desired=std::max(desired,branchLineEndX(state,row)+routeGap); } if (destination<source) desired+=laneStep; route.DesiredLane=std::min(desired,right); route.LaneX=route.DesiredLane; routes.emplace_back(route);
            };

            for (const auto& [source,flow]:ui.FlowGraph)
            {
                float targetAlpha=0.85f,fallAlpha=0.65f; if (flow.Branch==RuntimeBranchKind::Conditional&&ui.HasCapturedHit&&ui.CapturedHit.Pid==state.Pid&&ui.CapturedHit.Address==source) if (const auto captured=branchCondition(flow.Mnemonic,ui.CapturedHit.Registers.eflags)) { targetAlpha=*captured?1.0f:0.24f; fallAlpha=*captured?0.24f:1.0f; }
                if (flow.Target)
                {
                    const ImU32 targetColor=flow.Branch==RuntimeBranchKind::Conditional?ImGui::ColorConvertFloat4ToU32(ImVec4(0.28f,0.88f,0.48f,targetAlpha)):flow.Branch==RuntimeBranchKind::Call?ImGui::ColorConvertFloat4ToU32(ImVec4(0.68f,0.48f,0.95f,0.85f)):ImGui::ColorConvertFloat4ToU32(ImVec4(0.32f,0.68f,0.96f,0.85f)); classify(source,*flow.Target,targetColor,flow.Branch,flow.Mnemonic,true,false);
                }
                if (flow.FallThrough) classify(source,*flow.FallThrough,ImGui::ColorConvertFloat4ToU32(ImVec4(0.95f,0.34f,0.34f,fallAlpha)),flow.Branch,flow.Mnemonic,false,true);
            }

            std::vector<std::size_t> order(routes.size()); for (std::size_t i=0;i<order.size();++i) order[i]=i; std::ranges::sort(order,[&](const std::size_t a,const std::size_t b) { return std::abs(routes[a].TargetY-routes[a].SourceY)<std::abs(routes[b].TargetY-routes[b].SourceY); }); std::vector<Reservation> reservations; reservations.reserve(routes.size());
            for (const std::size_t index:order)
            {
                auto& route=routes[index]; const float y0=std::min(route.SourceY,route.TargetY)-4.0f,y1=std::max(route.SourceY,route.TargetY)+4.0f; float best=route.DesiredLane; int bestConflicts=std::numeric_limits<int>::max();
                for (float candidate=route.DesiredLane;candidate<=right+0.1f;candidate+=laneStep)
                {
                    int conflicts=0; for (const auto& reservation:reservations) if (!(y1<reservation.Y0||y0>reservation.Y1)&&std::abs(candidate-reservation.X)<laneStep*0.82f) ++conflicts; if (conflicts<bestConflicts) { best=candidate; bestConflicts=conflicts; } if (conflicts==0) break;
                }
                route.LaneX=std::min(best,right); reservations.push_back({route.LaneX,y0,y1});
            }

            const ImVec2 mouse=ImGui::GetMousePos(); const float hoverDistance2=64.0f; auto* draw=ImGui::GetWindowDrawList(); draw->PushClipRect(ImVec2(min.x+1.0f,min.y+1.0f),ImVec2(max.x-1.0f,max.y-1.0f),true);
            const auto routeDistance=[&](const Route& route)
            {
                float distance=std::numeric_limits<float>::max(); if (route.SourceVisible) distance=std::min(distance,pointSegmentDistanceSquared(mouse,{route.SourceDock,route.SourceY},{route.LaneX,route.SourceY})); distance=std::min(distance,pointSegmentDistanceSquared(mouse,{route.LaneX,route.SourceY},{route.LaneX,route.TargetY})); if (route.TargetVisible) distance=std::min(distance,pointSegmentDistanceSquared(mouse,{route.LaneX,route.TargetY},{route.TargetDock,route.TargetY})); return distance;
            };
            std::optional<std::size_t> hotRoute; float hotDistance=hoverDistance2; if (ImGui::IsMouseHoveringRect(min,max)) for (std::size_t i=0;i<routes.size();++i) { const float distance=routeDistance(routes[i]); if (distance<=hotDistance) { hotDistance=distance; hotRoute=i; } }
            const auto drawSegments=[&](const Route& route,const ImU32 color,const float width)
            {
                if (route.SourceVisible) draw->AddLine({route.SourceDock,route.SourceY},{route.LaneX,route.SourceY},color,width); draw->AddLine({route.LaneX,route.SourceY},{route.LaneX,route.TargetY},color,width); if (route.TargetVisible) draw->AddLine({route.LaneX,route.TargetY},{route.TargetDock,route.TargetY},color,width);
            };
            for (std::size_t i=0;i<routes.size();++i)
            {
                const auto& route=routes[i]; const bool hot=hotRoute&&*hotRoute==i; if (hot) drawSegments(route,IM_COL32(255,255,255,70),5.5f); const ImU32 color=hot?IM_COL32(255,255,255,245):route.Color; drawSegments(route,color,hot?2.4f:1.7f); if (route.SourceVisible&&route.SourceDot) draw->AddCircleFilled({route.SourceDock,route.SourceY},hot?3.2f:2.5f,color); if (route.TargetVisible) drawLeftArrowHead(draw,{route.TargetDock,route.TargetY},color); else drawVerticalArrowHead(draw,{route.LaneX,route.TargetY},route.TargetBelow,color);
            }

            if (!farRoutes.empty())
            {
                bool hasUp=false,hasDown=false,hasConditional=false,hasCall=false,hasJump=false; for (const auto& route:farRoutes) { hasUp|=route.Entry.Destination<route.Entry.Source; hasDown|=route.Entry.Destination>=route.Entry.Source; hasConditional|=route.Entry.Branch==RuntimeBranchKind::Conditional; hasCall|=route.Entry.Branch==RuntimeBranchKind::Call; hasJump|=route.Entry.Branch==RuntimeBranchKind::Unconditional; if (route.SourceVisible) { const float end=std::min(farRailX-18.0f,route.SourceDock+22.0f); draw->AddLine({route.SourceDock,route.SourceY},{end,route.SourceY},route.Entry.Color,2.0f); draw->AddCircleFilled({route.SourceDock,route.SourceY},2.7f,route.Entry.Color); } if (route.TargetVisible) { const float start=std::min(farRailX-18.0f,route.TargetDock+22.0f); draw->AddLine({route.TargetDock,route.TargetY},{start,route.TargetY},route.Entry.Color,2.0f); drawLeftArrowHead(draw,{route.TargetDock,route.TargetY},route.Entry.Color); } }
                const bool hot=ImGui::IsMouseHoveringRect(ImVec2(farRailX-11.0f,top),ImVec2(max.x-1.0f,bottom)); std::vector<ImU32> colors; if (hasConditional) colors.push_back(IM_COL32(72,224,122,225)); if (hasCall) colors.push_back(IM_COL32(174,122,242,225)); if (hasJump) colors.push_back(IM_COL32(82,174,245,225)); if (colors.empty()) colors.push_back(IM_COL32(180,180,190,225)); const float stripe=3.0f,total=stripe*static_cast<float>(colors.size()-1); if (hot) draw->AddLine({farRailX,top},{farRailX,bottom},IM_COL32(255,255,255,210),8.0f); for (std::size_t i=0;i<colors.size();++i) { const float x=farRailX-total*0.5f+stripe*static_cast<float>(i); draw->AddLine({x,top},{x,bottom},hot?IM_COL32(255,255,255,245):colors[i],2.5f); }
                const ImU32 headColor=hot?IM_COL32(255,255,255,245):colors.front(); if (hasUp) drawVerticalArrowHead(draw,{farRailX,top},false,headColor); if (hasDown) drawVerticalArrowHead(draw,{farRailX,bottom},true,headColor); const std::string count="×"+std::to_string(farRoutes.size()); draw->AddText({farRailX-ImGui::CalcTextSize(count.c_str()).x-8.0f,top+5.0f},hot?IM_COL32(255,255,255,255):IM_COL32(215,215,225,230),count.c_str()); if (hot) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); ImGui::SetTooltip(ui::i18n::tr("re.farFlowTooltip"),farRoutes.size()); if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { ui.FarFlowEntries.clear(); ui.FarFlowEntries.reserve(farRoutes.size()); for (const auto& route:farRoutes) ui.FarFlowEntries.push_back(route.Entry); ui.OpenFarFlowPopup=true; } }
            }
            draw->PopClipRect();
            if (hotRoute)
            {
                const auto& route=routes[*hotRoute]; ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); const std::string destination=runtimeFormatProcessAddress(state.Pid,route.Destination); ImGui::SetTooltip(route.FallThrough?ui::i18n::tr("re.flowFallThrough"):ui::i18n::tr("re.flowDestination"),destination.c_str()); if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ui.PendingInspectorAddress=route.Destination;
            }
        }

        void drawDisassemblyContextMenu(RuntimeMemoryInspectorState& state, const std::uintptr_t address)
        {
            auto& ui=enhancedMemoryInspectorState(); auto& probe=executionProbe(); const auto modules=runtimeProcessModules(state.Pid); const RuntimeX86Mode mode=runtimeProcessX86Mode(state.Pid); const auto instructionIt=std::ranges::find(ui.Instructions,address,&DisassemblyInstruction::Address); if (instructionIt==ui.Instructions.end()) { ImGui::CloseCurrentPopup(); return; } const std::size_t index=static_cast<std::size_t>(instructionIt-ui.Instructions.begin()); const auto& instruction=*instructionIt; const auto key=std::pair{state.Pid,address}; ImGui::TextDisabled("%s",processAddressText(mode,modules,address).c_str());
            if (ImGui::MenuItem(ui::i18n::tr("re.syncHere"))) { ui.SyncedAddress=address; ui.NavigationHighlightAddress=address; applyDisassemblyMarkers(state); } if (ImGui::MenuItem(ui::i18n::tr("re.useInspectorBase"))) ui.PendingInspectorAddress=address; if (ImGui::MenuItem(ui::i18n::tr("re.copyModuleAddress"))) { const std::string value=processAddressText(mode,modules,address); ImGui::SetClipboardText(value.c_str()); } if (ImGui::MenuItem(ui::i18n::tr("re.copyAbsoluteAddress"))) { const std::string value=runtimeFormatAbsoluteAddress(mode,address); ImGui::SetClipboardText(value.c_str()); } if (ImGui::MenuItem(ui::i18n::tr("re.copyInstruction"))) { const std::string value=state.Disassembly.GetLineText(instruction.DisplayLine); ImGui::SetClipboardText(value.c_str()); }
            const bool hasSelection=state.Disassembly.CurrentCursorHasSelection(); std::size_t first=index,last=index; if (hasSelection) { const auto selection=state.Disassembly.GetCurrentCursorSelection(); for (std::size_t i=0;i<ui.Instructions.size();++i) if (ui.Instructions[i].DisplayLine>=selection.start.line&&ui.Instructions[i].DisplayLine<=selection.end.line) { first=std::min(first,i); last=std::max(last,i); } }
            if (ImGui::MenuItem(hasSelection?ui::i18n::tr("re.createSignatureSelection"):ui::i18n::tr("re.createSignatureHere"))) ui::requestSignatureMaker(state.Pid,ui.Instructions[first].Address,static_cast<int>(last-first+1));
            ImGui::Separator(); if (ImGui::MenuItem(hasSelection?ui::i18n::tr("re.assembleSelection"):ui::i18n::tr("re.assembleInstruction"))) openAssembler(state,first,last,false); if (ImGui::MenuItem(hasSelection?ui::i18n::tr("re.nopSelection"):ui::i18n::tr("re.nopInstruction"))) openAssembler(state,first,last,true);
            ImGui::Separator(); const RuntimeFunctionCandidate* function=functionForAddress(ui.FunctionAnalysis,address); if (function)
            {
                const std::string name=functionName(ui,state.Pid,modules,*function); ImGui::SeparatorText(name.c_str()); if (ImGui::MenuItem(ui::i18n::tr("re.goFunctionStart"))) ui.PendingInspectorAddress=function->Address; if (ImGui::MenuItem(ui::i18n::tr("re.copyFunctionAddress"))) { const std::string value=processAddressText(mode,modules,function->Address); ImGui::SetClipboardText(value.c_str()); } auto& rename=ui.FunctionNames[{state.Pid,function->Address}]; ImGui::InputText(ui::i18n::tr("re.functionName"),rename.data(),rename.size()); if (ImGui::IsItemDeactivatedAfterEdit()) ui.DisassemblyDirty=true;
            }
            if (ImGui::MenuItem(ui::i18n::tr("re.analyzeFunctions"))) requestRuntimeFunctionAnalysis(state.Pid,address,true);
            ImGui::Separator(); const bool thisProbe=probe.running()&&probe.pid()==state.Pid&&probe.address()==address; if (thisProbe) { if (ImGui::MenuItem(ui::i18n::tr("re.cancelProbe"))) { probe.stop(); ui.DisassemblyDirty=true; state.Status="execution probe cancelled"; } } else { ImGui::BeginDisabled(probe.running()); if (ImGui::MenuItem(ui::i18n::tr("re.captureRegisters"))) { std::string error; if (!probe.start(state.Pid,address,error)) state.Status=error; else { ui.SyncedAddress=address; ui.NavigationHighlightAddress=address; ui.DisassemblyDirty=true; state.Status="armed for next execution"; } } ImGui::EndDisabled(); }
            ImGui::Separator(); auto marker=ui.Markers.find(key); if (marker==ui.Markers.end()) { if (ImGui::MenuItem(ui::i18n::tr("re.addBookmark"))) { ui.Markers.emplace(key,DisassemblyMarker{}); ui.DisassemblyDirty=true; } } else { ImGui::SeparatorText(ui::i18n::tr("re.bookmark")); if (ImGui::InputText(ui::i18n::tr("re.tag"),marker->second.Tag.data(),marker->second.Tag.size())) ui.DisassemblyDirty=true; if (ImGui::ColorEdit4(ui::i18n::tr("re.color"),&marker->second.Color.x,ImGuiColorEditFlags_AlphaBar)) { applyDisassemblyMarkers(state); ui.DisassemblyDirty=true; } if (ImGui::MenuItem(ui::i18n::tr("re.removeBookmark"))) { ui.Markers.erase(marker); ui.DisassemblyDirty=true; } }
        }

        void drawBookmarks(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); const auto modules = runtimeProcessModules(state.Pid); const RuntimeX86Mode mode=runtimeProcessX86Mode(state.Pid); bool any = false; std::optional<std::pair<pid_t,std::uintptr_t>> erase;
            for (auto& [key, marker] : ui.Markers)
            {
                if (key.first != state.Pid) continue; any = true; ImGui::PushID(reinterpret_cast<void*>(key.second)); const std::string address = processAddressText(mode,modules,key.second); ImGui::ColorButton("##bookmark-color", marker.Color, ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetTextLineHeight(),ImGui::GetTextLineHeight())); ImGui::SameLine(); ImGui::Text("%s  %s", marker.Tag.data(), address.c_str()); ImGui::SameLine(); if (ImGui::SmallButton(ui::i18n::tr("re.go"))) ui.PendingInspectorAddress = key.second; ImGui::SameLine(); if (ImGui::SmallButton(ui::i18n::tr("common.copy"))) ImGui::SetClipboardText(address.c_str()); ImGui::SameLine(); if (ImGui::SmallButton(ui::i18n::tr("re.remove"))) erase = key; ImGui::PopID();
            }
            if (erase) { ui.Markers.erase(*erase); ui.DisassemblyDirty = true; } if (!any) ImGui::TextDisabled("%s",ui::i18n::tr("re.noBookmarks"));
        }

        void drawPatchBytes(RuntimeMemoryInspectorState& state)
        {
            auto& ui=enhancedMemoryInspectorState(); ImGui::TextDisabled("%s",ui::i18n::tr("re.rawPatcherDescription")); ImGui::InputTextMultiline("##hexPatch2", state.HexEdit.data(), state.HexEdit.size(), ImVec2(-1.0f,125.0f)); const char* writeLabel = state.WriteConfirm == 0 ? ui::i18n::tr("re.writeChanges") : ui::i18n::tr("re.confirmWrite");
            if (ImGui::Button(writeLabel))
            {
                std::vector<std::uint8_t> bytes; std::string error; if (!runtimeParseHexBytes(state.HexEdit.data(), bytes, error)) { state.Status = error; state.WriteConfirm = 0; } else if (bytes.size() != state.Original.size()) { state.Status = "patch byte count must stay at " + std::to_string(state.Original.size()); state.WriteConfirm = 0; } else if (state.WriteConfirm == 0) { state.Patched = std::move(bytes); state.WriteConfirm = 1; state.Status = "write armed; click Confirm write to modify process memory"; } else if (runtimeWriteProcessMemory(state.Pid, ui.BufferBase, bytes, error)) { state.Status = "memory written"; refreshEnhancedRuntimeMemoryInspector(state); } else { state.Status = "memory write failed: " + error; state.WriteConfirm = 0; }
            }
            ImGui::SameLine(); if (ImGui::Button(ui::i18n::tr("re.cancelWrite"))) { state.WriteConfirm = 0; state.Status = "write cancelled"; }
        }
    }

    void refreshEnhancedRuntimeMemoryInspector(RuntimeMemoryInspectorState& state)
    {
        auto& ui = enhancedMemoryInspectorState(); state.ReadSize = std::clamp(state.ReadSize, 16, 4096); if (state.Pid <= 0 || state.Address == 0) { state.Original.clear(); ui.RawBytes.clear(); ui.BufferBase=0; ui.BufferSize=0; state.Disassembly.SetText({}); ui.Instructions.clear(); ui.DisplayLineAddresses.clear(); ui.DisplayLineByAddress.clear(); state.Status = "select a process and enter an address"; return; }
        if (!refreshDisassemblyBytes(state, true, true)) return; refreshRawBytes(state); ui.LastPid = state.Pid; ui.LastAddress = state.Address; ui.LastReadSize = state.ReadSize; ui.LastFunctionAnalysisPoll=0.0; if (ui.SyncedAddress < ui.BufferBase || ui.SyncedAddress >= ui.BufferBase + ui.BufferSize) ui.SyncedAddress = state.Address; state.Status = "read " + std::to_string(state.Original.size()) + " prefetched bytes as " + runtimeX86ModeName(runtimeProcessX86Mode(state.Pid)); syncInspectorAddressText(state); requestRuntimeFunctionAnalysis(state.Pid, state.Address);
    }

    void drawEnhancedRuntimeMemoryInspector(RuntimeMemoryInspectorState& state)
    {
        auto& ui = enhancedMemoryInspectorState(); auto& probe = executionProbe(); auto& configuration = runtimeConfiguration(); syncInspectorAddressText(state); if ((ui.LastPid != state.Pid || ui.LastAddress != state.Address || ui.LastReadSize != state.ReadSize) && state.Pid > 0 && state.Address != 0) refreshEnhancedRuntimeMemoryInspector(state); configureDisassemblyEditor(state);
        if (const auto hit = probe.hit(); hit && hit->HasRegisters && hit->Pid == state.Pid && hit->Time > ui.LastProbeHitTime) { ui.CapturedHit = *hit; ui.HasCapturedHit = true; ui.LastProbeHitTime = hit->Time; ui.SyncedAddress = hit->Address; ui.NavigationHighlightAddress=hit->Address; ui.SelectRegistersTab = true; state.Status = "captured registers at " + runtimeFormatProcessAddress(state.Pid, hit->Address) + " on TID " + std::to_string(hit->Tid); ui.DisassemblyDirty = true; }
        const bool probeRunning = probe.running(); const pid_t probePid = probe.pid(); const std::uintptr_t probeAddress = probe.address(); if (probeRunning != ui.LastProbeRunning || probePid != ui.LastProbePid || probeAddress != ui.LastProbeAddress) ui.DisassemblyDirty = true;
        const double now = runtimeSteadySeconds();
        if (configuration.FunctionHeuristics && state.Pid > 0 && state.Address)
        {
            const double pollInterval=ui.FunctionAnalysis.Running?0.05:0.25;
            if (ui.LastFunctionAnalysisPoll==0.0||now-ui.LastFunctionAnalysisPoll>=pollInterval) { requestRuntimeFunctionAnalysis(state.Pid, state.Address); const auto snapshot = runtimeFunctionAnalysisSnapshot(state.Pid, state.Address); if (snapshot.Revision != ui.FunctionRevision) { ui.FunctionAnalysis = snapshot; ui.FunctionRevision = snapshot.Revision; ui.DisassemblyDirty = true; } else ui.FunctionAnalysis.Running=snapshot.Running,ui.FunctionAnalysis.Progress=snapshot.Progress,ui.FunctionAnalysis.Status=snapshot.Status; ui.LastFunctionAnalysisPoll=now; }
        }
        if (configuration.DisassemblyRefreshHz > 0.0f && state.Pid > 0 && state.Address && now - ui.LastDisassemblyRefresh >= 1.0 / configuration.DisassemblyRefreshHz) refreshDisassemblyBytes(state, false, false); if (ui.DisassemblyDirty && !state.Original.empty()) rebuildDisassembly(state);

        const pid_t oldPid=state.Pid; if (ui::drawProcessPicker("DisassemblyProcess",ui.Processes,state.Pid,ui.ProcessSearch.data(),ui.ProcessSearch.size(),360.0f) && state.Pid!=oldPid)
        {
            if (oldPid>0) invalidateRuntimeFunctionAnalysis(oldPid); ui.BufferBase=0; ui.BufferSize=0; ui.LastPid=0; ui.NavigationBack.clear(); ui.NavigationForward.clear(); ui.FlowGraph.clear(); ui.FlowPreviewCache.clear(); ui.FarFlowEntries.clear(); ui.FlowPid=state.Pid; ui.ContextAddress.reset(); ui.NavigationHighlightAddress=0; state.Address=0; ui.AddressTextValue=std::numeric_limits<std::uintptr_t>::max(); state.Original.clear(); state.Disassembly.SetText({}); state.Status="select an address";
        }

        ImGui::BeginDisabled(ui.NavigationBack.empty()); if (ImGui::SmallButton("<##DisassemblyBack")) navigateBack(state); ImGui::EndDisabled(); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",ui::i18n::tr("re.backTooltip")); ImGui::SameLine();
        ImGui::BeginDisabled(ui.NavigationForward.empty()); if (ImGui::SmallButton(">##DisassemblyForward")) navigateForward(state); ImGui::EndDisabled(); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",ui::i18n::tr("re.forwardTooltip")); ImGui::SameLine();
        if (ImGui::Button(ui::i18n::tr("re.goTo"))) { std::snprintf(ui.GoToText.data(),ui.GoToText.size(),"%s",ui.AddressText.data()); ImGui::OpenPopup("##DisassemblyGoToPopup"); } ImGui::SameLine();
        const bool addressEnter = ui::drawAddressInput("Address##memory2", ui.AddressText.data(), ui.AddressText.size(), state.Pid, 260.0f, ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine(); ImGui::SetNextItemWidth(105.0f); if (ImGui::InputInt("Bytes##memory2", &state.ReadSize)) state.ReadSize = std::clamp(state.ReadSize,16,4096); ImGui::SameLine(); const bool readPressed = ImGui::Button(ui::i18n::tr("re.readDisassemble"));
        if (addressEnter || readPressed) { std::uintptr_t address = 0; std::string error; if (!ui::evaluateAddressExpression(state.Pid, ui.AddressText.data(), address, error) || address == 0) state.Status = error.empty() ? "invalid memory address" : error; else navigateInspector(state,address,true); }
        ImGui::SameLine(); if (state.Pid>0) ImGui::TextDisabled("%s", runtimeX86ModeName(runtimeProcessX86Mode(state.Pid))); if (ImGui::SmallButton(ui::i18n::tr("common.refresh"))) { if (refreshDisassemblyBytes(state,true,false)) refreshRawBytes(state); } ImGui::SameLine(); ImGui::Checkbox(ui::i18n::tr("re.synchronizeAddress"), &ui.SynchronizeAddress); if (ui.SynchronizeAddress && ui.SyncedAddress) { ImGui::SameLine(); ImGui::TextDisabled(ui::i18n::tr("re.syncedAddress"), runtimeFormatProcessAddress(state.Pid, ui.SyncedAddress).c_str()); }
        const auto& io=ImGui::GetIO();
        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_G,false)) { std::snprintf(ui.GoToText.data(),ui.GoToText.size(),"%s",ui.AddressText.data()); ImGui::OpenPopup("##DisassemblyGoToPopup"); }
        if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z,false)) navigateBack(state); else if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y,false)) navigateForward(state);
        if (ImGui::BeginPopup("##DisassemblyGoToPopup"))
        {
            ImGui::TextUnformatted(ui::i18n::tr("re.goToAddress")); const bool enter=ui::drawAddressInput("##DisassemblyGoToAddress",ui.GoToText.data(),ui.GoToText.size(),state.Pid,300.0f,ImGuiInputTextFlags_EnterReturnsTrue); if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1); ImGui::SameLine(); const bool go=ImGui::Button(ui::i18n::tr("re.go"));
            if (enter||go) { std::uintptr_t address=0; std::string error; if (ui::evaluateAddressExpression(state.Pid,ui.GoToText.data(),address,error)&&address) { navigateInspector(state,address,true); ImGui::CloseCurrentPopup(); } else state.Status=error.empty()?"invalid memory address":error; }
            ImGui::EndPopup();
        }
        if (!state.Status.empty()) ImGui::TextDisabled("%s", state.Status.c_str()); if (state.Original.empty()) return;

        ImGui::SeparatorText(ui::i18n::tr("re.disassembly")); if (probeRunning && probePid == state.Pid) { ImGui::TextColored(ImVec4(1.0f,0.30f,0.30f,1.0f), "ARMED %s — %s", runtimeFormatProcessAddress(state.Pid, probeAddress).c_str(),ui::i18n::tr("re.armedNextExecution")); ImGui::SameLine(); if (ImGui::SmallButton(ui::i18n::tr("re.cancelProbe"))) { probe.stop(); ui.DisassemblyDirty = true; } } else if (ui.HasCapturedHit && ui.CapturedHit.Pid == state.Pid) { ImGui::TextDisabled(ui::i18n::tr("re.lastCapture"), runtimeFormatProcessAddress(state.Pid, ui.CapturedHit.Address).c_str(), ui.CapturedHit.Tid); ImGui::SameLine(); if (ImGui::SmallButton(ui::i18n::tr("re.armAgain"))) { std::string error; if (!probe.start(state.Pid, ui.CapturedHit.Address, error)) state.Status = error; else { state.Status = "armed for next execution"; ui.DisassemblyDirty = true; } } }
        if (ui.FunctionAnalysis.Running) { ImGui::ProgressBar(ui.FunctionAnalysis.Progress, ImVec2(180.0f,0.0f), ui::i18n::tr("re.functionAnalysis")); ImGui::SameLine(); ImGui::TextDisabled(ui::i18n::tr("re.functionAnalysisLive"), ui.FunctionAnalysis.Candidates.size()); } else if (!ui.FunctionAnalysis.Status.empty()) ImGui::TextDisabled(ui::i18n::tr("re.functionHints"), ui.FunctionAnalysis.Status.c_str(), ui.FunctionAnalysis.Candidates.size());
        ImGui::TextDisabled("%s",ui::i18n::tr("re.prefetchHint"));

        drawDisassemblyHeader(state); state.Disassembly.ClearTextHoverCallback(); state.Disassembly.ClearTextContextMenuCallback(); state.Disassembly.SetMouseClickCallback([&](TextEditor::MouseClickData& data)
        {
            const auto index=instructionIndexForDisplayLine(ui,data.pos.line); if (!index) return false; const auto& instruction=ui.Instructions[*index];
            if (data.button==ImGuiMouseButton_Left&&data.ctrl&&instruction.Decoded.Target) { ui.PendingInspectorAddress=*instruction.Decoded.Target; return true; }
            if (data.button==ImGuiMouseButton_Right) { ui.ContextAddress=instruction.Address; ui.OpenContextPopup=true; return true; }
            return false;
        });
        state.Disassembly.Render("##memory-disassembly", ImVec2(-1.0f,390.0f), ImGuiChildFlags_Borders); const ImVec2 disassemblyMin = ImGui::GetItemRectMin(), disassemblyMax = ImGui::GetItemRectMax();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)&&ImGui::IsMouseHoveringRect(disassemblyMin,disassemblyMax)) { const auto pos=state.Disassembly.GetDocPosAtMousePos(ImGui::GetMousePos()); if (const auto index=instructionIndexForDisplayLine(ui,pos.line)) { ui.ContextAddress=ui.Instructions[*index].Address; ui.OpenContextPopup=true; } }
        drawBranchLanes(state,disassemblyMin,disassemblyMax); drawFarControlFlowPopup(state);
        if (ui.OpenContextPopup) { ImGui::OpenPopup("##QuartzDisassemblyContext"); ui.OpenContextPopup=false; }
        if (ImGui::BeginPopup("##QuartzDisassemblyContext")) { if (ui.ContextAddress) drawDisassemblyContextMenu(state,*ui.ContextAddress); else ImGui::CloseCurrentPopup(); ImGui::EndPopup(); }
        const bool contextOpen=ImGui::IsPopupOpen("##QuartzDisassemblyContext")||ImGui::IsPopupOpen("##QuartzFarControlFlow"); if (!contextOpen&&!ImGui::IsMouseDown(ImGuiMouseButton_Right)&&!ImGui::IsMouseClicked(ImGuiMouseButton_Right)&&state.Disassembly.IsMousePosOverTextArea(ImGui::GetMousePos())) { const auto pos=state.Disassembly.GetDocPosAtMousePos(ImGui::GetMousePos()); if (assemblyHoverAvailable(state,state.Disassembly,true,pos)) { ImGui::BeginTooltip(); drawAssemblyHoverContent(state,state.Disassembly,true,pos); ImGui::EndTooltip(); } }
        if (!ui.PendingInspectorAddress && ui.SynchronizeAddress && state.Disassembly.IsMousePosOverTextArea(ImGui::GetMousePos()) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { const auto pos = state.Disassembly.GetDocPosAtMousePos(ImGui::GetMousePos()); if (pos.line < ui.DisplayLineAddresses.size() && ui.DisplayLineAddresses[pos.line]) { ui.SyncedAddress = ui.DisplayLineAddresses[pos.line]; ui.NavigationHighlightAddress=ui.SyncedAddress; applyDisassemblyMarkers(state); } }
        if (!ui.PendingInspectorAddress && !ui.DisplayLineAddresses.empty() && ui.BufferSize && now-ui.LastDisassemblyWindowShift>=0.10)
        {
            const std::size_t first=state.Disassembly.GetFirstVisibleRow(),last=state.Disassembly.GetLastVisibleRow(); if (last>=first)
            {
                const auto focus=displayAddressNearRow(ui,(first+last)/2); if (focus&&*focus>=ui.BufferBase&&*focus<ui.BufferBase+ui.BufferSize)
                {
                    const double position=static_cast<double>(*focus-ui.BufferBase)/static_cast<double>(ui.BufferSize); const bool needUp=position<0.38&&ui.BufferBase>ui.BufferRegionBase,needDown=position>0.62&&ui.BufferBase+ui.BufferSize<ui.BufferRegionEnd;
                    if (needUp||needDown)
                    {
                        const auto topAnchor=displayAddressNearRow(ui,first); ui.LastDisassemblyWindowShift=now; setInspectorAddress(state,*focus,topAnchor?topAnchor:focus,topAnchor?TextEditor::Scroll::alignTop:TextEditor::Scroll::alignMiddle);
                    }
                }
            }
        }
        if (ui.DisassemblyDirty) rebuildDisassembly(state);

        if (ImGui::BeginTabBar("MemoryInspectorLowerTabs"))
        {
            if (ImGui::BeginTabItem(ui::i18n::tr("re.rawBytes")))
            {
                if (configuration.RawBytesRefreshHz > 0.0f && now - ui.LastRawRefresh >= 1.0 / configuration.RawBytesRefreshHz) refreshRawBytes(state); ImGui::TextDisabled(ui::i18n::tr("re.rawBytesDescription"), configuration.RawBytesRefreshHz); const RuntimeX86Mode mode=runtimeProcessX86Mode(state.Pid); const auto modules = runtimeProcessModules(state.Pid); const auto bytes = std::span<const std::uint8_t>(ui.RawBytes.empty() ? state.Original : ui.RawBytes);
                const ImGuiTableFlags rawFlags=ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollX|ImGuiTableFlags_ScrollY|ImGuiTableFlags_SizingFixedFit|ImGuiTableFlags_NoSavedSettings;
                if (ImGui::BeginTable("##GroupedRawMemory",3,rawFlags,ImVec2(0.0f,235.0f),760.0f))
                {
                    ImGui::TableSetupScrollFreeze(1,1); ImGui::TableSetupColumn(ui::i18n::tr("re.address"),ImGuiTableColumnFlags_WidthFixed,ui.AddressColumnWidth); ImGui::TableSetupColumn(ui::i18n::tr("re.bytes"),ImGuiTableColumnFlags_WidthFixed,430.0f); ImGui::TableSetupColumn(ui::i18n::tr("re.ascii"),ImGuiTableColumnFlags_WidthFixed,150.0f); ImGui::TableHeadersRow(); if (const ImGuiTable* table=ImGui::GetCurrentTable(); table&&table->ColumnsCount>=3) ui.AddressColumnWidth=std::clamp(table->Columns[0].WidthGiven,110.0f,520.0f);
                    const int lines=static_cast<int>((bytes.size()+15)/16); ImGuiListClipper clipper; clipper.Begin(lines); while (clipper.Step()) for (int line=clipper.DisplayStart;line<clipper.DisplayEnd;++line)
                    {
                        const std::size_t offset=static_cast<std::size_t>(line)*16,count=std::min<std::size_t>(16,bytes.size()-offset); const std::uintptr_t address=ui.BufferBase+offset; const bool active=(ui.NavigationHighlightAddress>=address&&ui.NavigationHighlightAddress<address+count)||(ui.SynchronizeAddress&&ui.SyncedAddress>=address&&ui.SyncedAddress<address+count); const std::string addressText=processAddressText(mode,modules,address),byteText=groupedByteText(bytes,offset,count),asciiText=groupedAsciiText(bytes,offset,count); ImGui::PushID(line); ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                        if (ImGui::Selectable(addressText.c_str(),active,ImGuiSelectableFlags_SpanAllColumns|ImGuiSelectableFlags_AllowOverlap)&&ui.SynchronizeAddress) { ui.SyncedAddress=address; ui.NavigationHighlightAddress=address; applyDisassemblyMarkers(state); }
                        if (ImGui::BeginPopupContextItem("RawLineContext")) { if (ImGui::MenuItem(ui::i18n::tr("re.useInspectorBase"))) ui.PendingInspectorAddress=address; if (ImGui::MenuItem(ui::i18n::tr("re.copyModuleAddress"))) ImGui::SetClipboardText(addressText.c_str()); if (ImGui::MenuItem(ui::i18n::tr("re.copyAbsoluteAddress"))) { const std::string absolute=runtimeFormatAbsoluteAddress(mode,address); ImGui::SetClipboardText(absolute.c_str()); } if (ImGui::MenuItem(ui::i18n::tr("re.copyByteGroup"))) { const std::string value=runtimeFormatHexBytes(bytes.subspan(offset,count)); ImGui::SetClipboardText(value.c_str()); } ImGui::EndPopup(); }
                        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(byteText.c_str()); ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(asciiText.c_str()); ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            const ImGuiTabItemFlags registerFlags=ui.SelectRegistersTab?ImGuiTabItemFlags_SetSelected:ImGuiTabItemFlags_None; if (ImGui::BeginTabItem(ui::i18n::tr("re.registers"),nullptr,registerFlags)) { ui.SelectRegistersTab=false; drawRegisterSnapshot(state); ImGui::EndTabItem(); }
            const ImGuiTabItemFlags assemblerFlags=ui.SelectAssemblerTab?ImGuiTabItemFlags_SetSelected:ImGuiTabItemFlags_None; if (ImGui::BeginTabItem(ui::i18n::tr("re.assembler"),nullptr,assemblerFlags)) { ui.SelectAssemblerTab=false; installAssemblyHover(state,ui.Assembler.Editor,false); drawAssembler(state); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(ui::i18n::tr("configuration.assemblerTweaks"))) { drawAssemblerTweaks(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(ui::i18n::tr("re.bookmarks"))) { drawBookmarks(state); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(ui::i18n::tr("re.patchHistory"))) { drawPatchHistory(state); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(ui::i18n::tr("re.patchBytes"))) { drawPatchBytes(state); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        if (ui.PendingInspectorAddress) { const std::uintptr_t address=*ui.PendingInspectorAddress; ui.PendingInspectorAddress.reset(); navigateInspector(state,address,true); }
    }
}
