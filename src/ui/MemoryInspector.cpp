#include "quartz/client/ui/MemoryInspector.hpp"
#include "quartz/client/ui/AddressInput.hpp"
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
            std::vector<DisassemblyInstruction> Instructions;
            std::vector<std::uintptr_t> DisplayLineAddresses;
            std::map<std::pair<pid_t, std::uintptr_t>, DisassemblyMarker> Markers;
            std::map<std::pair<pid_t, std::uintptr_t>, std::array<char, 96>> FunctionNames;
            std::vector<std::uint8_t> RawBytes;
            std::uintptr_t SyncedAddress = 0;
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
            RuntimeFunctionAnalysisSnapshot FunctionAnalysis;
            std::uint64_t FunctionRevision = 0;
            double LastDisassemblyRefresh = 0.0;
            double LastRawRefresh = 0.0;
            AssemblerState Assembler;
            std::vector<PatchRecord> PatchHistory;
        };

        EnhancedMemoryInspectorState& enhancedMemoryInspectorState() { static EnhancedMemoryInspectorState state; return state; }

        std::string lower(std::string value) { std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); }); return value; }
        std::string upper(std::string value) { std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); }); return value; }

        void syncInspectorAddressText(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); if (ui.AddressTextValue == state.Address) return;
            std::snprintf(ui.AddressText.data(), ui.AddressText.size(), "0x%llX", static_cast<unsigned long long>(state.Address)); ui.AddressTextValue = state.Address;
        }

        void configureDisassemblyEditor(RuntimeMemoryInspectorState& state)
        {
            state.Disassembly.SetPalette(ui::quartzTextEditorPalette()); state.Disassembly.SetLanguage(intelAsmPatternLanguage()); state.Disassembly.SetReadOnlyEnabled(true); state.Disassembly.SetCaretsVisible(false); state.Disassembly.SetShowLineNumbersEnabled(false); state.Disassembly.SetShowMiniMapEnabled(false); state.Disassembly.SetWordWrapEnabled(false); state.EditorInitialized = true;
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

        bool knownRegister(std::string token)
        {
            token = upper(std::move(token)); static ExecutionProbeHit dummy{}; if (registerValue(dummy, token)) return true;
            return token == "RAX" || token == "EAX" || token == "AX" || token == "AL" || token == "AH" || token == "RBX" || token == "EBX" || token == "BX" || token == "BL" || token == "BH" || token == "RCX" || token == "ECX" || token == "CX" || token == "CL" || token == "CH" || token == "RDX" || token == "EDX" || token == "DX" || token == "DL" || token == "DH" || token == "RSI" || token == "ESI" || token == "SI" || token == "SIL" || token == "RDI" || token == "EDI" || token == "DI" || token == "DIL" || token == "RBP" || token == "EBP" || token == "BP" || token == "BPL" || token == "RSP" || token == "ESP" || token == "SP" || token == "SPL" || token == "RIP" || token == "EIP" || token == "RFLAGS" || token == "EFLAGS" || token == "FLAGS" || (token.size() >= 2 && token[0] == 'R' && token[1] >= '8' && token[1] <= '9') || token.starts_with("R10") || token.starts_with("R11") || token.starts_with("R12") || token.starts_with("R13") || token.starts_with("R14") || token.starts_with("R15");
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
            if (mnemonic == "mov") return "Copy the source operand into the destination."; if (mnemonic == "lea") return "Compute an effective address without dereferencing memory."; if (mnemonic == "cmp") return "Subtract operands for flags only; the result is discarded."; if (mnemonic == "test") return "Bitwise AND operands for flags only; the result is discarded.";
            if (mnemonic == "call") return "Call a procedure and push a return address."; if (mnemonic == "ret" || mnemonic == "retf") return "Return to the saved caller address."; if (mnemonic == "jmp") return "Unconditional control-flow transfer."; if (!mnemonic.empty() && mnemonic.front() == 'j') return "Conditional control-flow transfer based on CPU flags.";
            if (mnemonic == "push") return "Push an operand onto the stack."; if (mnemonic == "pop") return "Pop the top stack value into an operand."; if (mnemonic == "nop") return "No operation; commonly used as patch padding."; return nullptr;
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

        const RuntimeFunctionCandidate* functionForAddress(const RuntimeFunctionAnalysisSnapshot& analysis, const std::uintptr_t address)
        {
            const RuntimeFunctionCandidate* result = nullptr; for (const auto& candidate : analysis.Candidates) { if (candidate.Address > address) break; result = &candidate; } return result;
        }

        std::string functionName(EnhancedMemoryInspectorState& ui, const pid_t pid, const std::span<const RuntimeProcessModule> modules, const RuntimeFunctionCandidate& candidate)
        {
            if (const auto it = ui.FunctionNames.find({pid, candidate.Address}); it != ui.FunctionNames.end() && it->second[0]) return it->second.data();
            std::uintptr_t offset = candidate.Address; for (const auto& module : modules) if (candidate.Address >= module.Base && candidate.Address < module.End) { offset = candidate.Address - module.Base; break; }
            std::ostringstream out; out << "fn_" << std::hex << std::uppercase << static_cast<unsigned long long>(offset); return out.str();
        }

        void applyDisassemblyMarkers(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); auto& probe = executionProbe(); const bool armed = probe.running() && probe.pid() == state.Pid; const std::uintptr_t armedAddress = armed ? probe.address() : 0; state.Disassembly.ClearMarkers();
            for (std::size_t line = 0; line < ui.DisplayLineAddresses.size(); ++line)
            {
                const std::uintptr_t address = ui.DisplayLineAddresses[line]; if (!address) continue;
                if (address == armedAddress) { state.Disassembly.AddMarker(line, IM_COL32(255,80,80,255), IM_COL32(180,30,30,82), "armed for next execution", "armed for next execution"); continue; }
                if (const auto marker = ui.Markers.find({state.Pid, address}); marker != ui.Markers.end()) { ImVec4 fill = marker->second.Color; fill.w = std::clamp(fill.w, 0.08f, 0.55f); const ImVec4 solid{fill.x,fill.y,fill.z,1.0f}; const std::string_view tag = marker->second.Tag.data(); state.Disassembly.AddMarker(line, ImGui::ColorConvertFloat4ToU32(solid), ImGui::ColorConvertFloat4ToU32(fill), tag, tag); continue; }
                if (ui.SynchronizeAddress && ui.SyncedAddress == address) state.Disassembly.AddMarker(line, IM_COL32(120,210,235,220), IM_COL32(70,115,130,42), "synchronized address", "synchronized address");
            }
        }

        void rebuildDisassembly(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); const RuntimeX86Mode mode = runtimeProcessX86Mode(state.Pid); const auto modules = enumerateRuntimeModules(state.Pid); auto analysis = runtimeFunctionAnalysisSnapshot(state.Pid, state.Address); ui.FunctionAnalysis = analysis; ui.FunctionRevision = analysis.Revision;
            std::ostringstream disassembly; ui.Instructions.clear(); ui.DisplayLineAddresses.clear(); std::size_t offset = 0;
            while (offset < state.Original.size())
            {
                const std::uintptr_t address = state.Address + offset; RuntimeDecodedInstruction decoded; std::size_t length = 1;
                if (runtimeDecodeProcessInstruction(mode, std::span<const std::uint8_t>(state.Original).subspan(offset), address, decoded) && decoded.Length) length = decoded.Length;
                else { std::ostringstream fallback; fallback << "db 0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<unsigned>(state.Original[offset]); decoded.Text = fallback.str(); decoded.Length = 1; }
                if (decoded.Branch == RuntimeBranchKind::Call && decoded.Target) runtimeObserveFunctionTarget(state.Pid, *decoded.Target, RuntimeFunctionCandidateSource::CallTarget);
                else if ((decoded.Branch == RuntimeBranchKind::Conditional || decoded.Branch == RuntimeBranchKind::Unconditional) && decoded.Target) runtimeObserveFunctionTarget(state.Pid, *decoded.Target, RuntimeFunctionCandidateSource::EndBranchTarget);
                ui.Instructions.push_back({address, length, decoded, 0}); offset += length;
            }
            analysis = runtimeFunctionAnalysisSnapshot(state.Pid, state.Address); ui.FunctionAnalysis = analysis; ui.FunctionRevision = analysis.Revision;
            for (auto& instruction : ui.Instructions)
            {
                const RuntimeFunctionCandidate* candidate = std::ranges::find(ui.FunctionAnalysis.Candidates, instruction.Address, &RuntimeFunctionCandidate::Address) == ui.FunctionAnalysis.Candidates.end() ? nullptr : &*std::ranges::find(ui.FunctionAnalysis.Candidates, instruction.Address, &RuntimeFunctionCandidate::Address);
                if (candidate)
                {
                    const std::string name = functionName(ui, state.Pid, modules, *candidate); disassembly << "; ── " << name << " ──  [" << runtimeFunctionCandidateSourceName(candidate->Source) << ", " << static_cast<int>(candidate->Confidence * 100.0f) << "%]\n"; ui.DisplayLineAddresses.push_back(0);
                }
                instruction.DisplayLine = ui.DisplayLineAddresses.size(); ui.DisplayLineAddresses.push_back(instruction.Address);
                std::string text = instruction.Decoded.Text; if (instruction.Decoded.Target && instruction.Decoded.Branch != RuntimeBranchKind::None)
                {
                    const std::size_t space = text.find_first_of(" \t"); const std::string op = space == std::string::npos ? text : text.substr(0, space); const auto targetCandidate = std::ranges::find(ui.FunctionAnalysis.Candidates, *instruction.Decoded.Target, &RuntimeFunctionCandidate::Address);
                    const std::string target = targetCandidate != ui.FunctionAnalysis.Candidates.end() ? functionName(ui, state.Pid, modules, *targetCandidate) + "  ; " + runtimeFormatProcessAddress(modules, *instruction.Decoded.Target) : runtimeFormatProcessAddress(modules, *instruction.Decoded.Target); text = op + " " + target;
                }
                disassembly << runtimeFormatProcessAddress(modules, instruction.Address) << "  " << text;
                if (const auto marker = ui.Markers.find({state.Pid, instruction.Address}); marker != ui.Markers.end()) { const std::string_view tag = marker->second.Tag.data(); disassembly << " ; [" << (tag.empty() ? "bookmark" : tag) << ']'; }
                if (executionProbe().running() && executionProbe().pid() == state.Pid && executionProbe().address() == instruction.Address) disassembly << " ; [armed for next execution]"; disassembly << '\n';
            }
            state.Disassembly.SetText(disassembly.str()); applyDisassemblyMarkers(state); ui.LastProbeRunning = executionProbe().running(); ui.LastProbePid = executionProbe().pid(); ui.LastProbeAddress = executionProbe().address(); ui.DisassemblyDirty = false;
            if (ui.PendingScrollAnchor)
            {
                const auto it = std::ranges::find(ui.DisplayLineAddresses, *ui.PendingScrollAnchor); if (it != ui.DisplayLineAddresses.end()) state.Disassembly.ScrollToLine(static_cast<std::size_t>(it - ui.DisplayLineAddresses.begin()), TextEditor::Scroll::alignTop); ui.PendingScrollAnchor.reset();
            }
        }

        bool refreshDisassemblyBytes(RuntimeMemoryInspectorState& state, const bool resetPatchEditor)
        {
            auto& ui = enhancedMemoryInspectorState(); if (state.Pid <= 0 || state.Address == 0 || state.ReadSize <= 0) return false; std::vector<std::uint8_t> bytes(static_cast<std::size_t>(state.ReadSize)); std::string error;
            if (!readProcessMemoryBlock(state.Pid, state.Address, bytes, error)) { state.Status = "memory read failed: " + error; return false; }
            state.Original = std::move(bytes); if (resetPatchEditor) { state.Patched = state.Original; const std::string formatted = runtimeFormatHexBytes(state.Original); std::snprintf(state.HexEdit.data(), state.HexEdit.size(), "%s", formatted.c_str()); state.WriteConfirm = 0; }
            configureDisassemblyEditor(state); rebuildDisassembly(state); ui.LastDisassemblyRefresh = runtimeSteadySeconds(); return true;
        }

        bool refreshRawBytes(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); if (state.Pid <= 0 || state.Address == 0 || state.ReadSize <= 0) return false; ui.RawBytes.assign(static_cast<std::size_t>(state.ReadSize), 0); std::string error;
            if (!readProcessMemoryBlock(state.Pid, state.Address, ui.RawBytes, error)) { ui.RawBytes.clear(); return false; } ui.LastRawRefresh = runtimeSteadySeconds(); return true;
        }

        void setInspectorAddress(RuntimeMemoryInspectorState& state, const std::uintptr_t address, const std::optional<std::uintptr_t> scrollAnchor = std::nullopt)
        {
            auto& ui = enhancedMemoryInspectorState(); if (scrollAnchor) ui.PendingScrollAnchor = scrollAnchor; state.Address = address; ui.AddressTextValue = std::numeric_limits<std::uintptr_t>::max(); syncInspectorAddressText(state); refreshEnhancedRuntimeMemoryInspector(state);
        }

        std::string groupedBytesLine(const std::span<const std::uint8_t> bytes, const std::uintptr_t base, const std::size_t offset, const std::size_t count, const std::span<const RuntimeProcessModule> modules)
        {
            std::ostringstream out; out << runtimeFormatProcessAddress(modules, base + offset) << "  ";
            for (std::size_t i = 0; i < 16; ++i) { if (i < count) out << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[offset + i]); else out << "  "; out << (i == 7 ? "  " : " "); }
            out << " |"; for (std::size_t i = 0; i < count; ++i) { const unsigned char c = bytes[offset + i]; out << (std::isprint(c) ? static_cast<char>(c) : '.'); } for (std::size_t i = count; i < 16; ++i) out << ' '; out << '|'; return out.str();
        }

        std::string decodedPreview(const RuntimeX86Mode mode, const std::uintptr_t address, const std::span<const std::uint8_t> bytes, const pid_t pid)
        {
            const auto modules = enumerateRuntimeModules(pid); std::ostringstream out; std::size_t offset = 0;
            while (offset < bytes.size())
            {
                RuntimeDecodedInstruction decoded; if (!runtimeDecodeProcessInstruction(mode, bytes.subspan(offset), address + offset, decoded) || !decoded.Length) { out << runtimeFormatProcessAddress(modules, address + offset) << "  db 0x" << std::hex << std::uppercase << static_cast<unsigned>(bytes[offset]) << '\n'; ++offset; continue; }
                out << runtimeFormatProcessAddress(modules, address + offset) << "  " << decoded.Text; if (decoded.Target) out << "  ; -> " << runtimeFormatProcessAddress(modules, *decoded.Target); out << '\n'; offset += decoded.Length;
            }
            return out.str();
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
            assembler.Error = "patch written"; assembler.ConfirmArmed = false; if (runtimeConfiguration().AssemblerAutoRedisassemble) refreshDisassemblyBytes(state, true); return true;
        }

        void drawAssemblerTweaks()
        {
            auto& config = runtimeConfiguration(); bool changed = false; changed |= ImGui::Checkbox("Fill remaining bytes with NOPs", &config.AssemblerFillNops); changed |= ImGui::Checkbox("Whole-instruction spans", &config.AssemblerWholeInstructions); changed |= ImGui::Checkbox("Consume following instructions when needed", &config.AssemblerConsumeFollowing); changed |= ImGui::Checkbox("Verify bytes before write", &config.AssemblerVerifyBeforeWrite); changed |= ImGui::Checkbox("Require confirmation", &config.AssemblerRequireConfirmation); changed |= ImGui::Checkbox("Auto re-disassemble after write", &config.AssemblerAutoRedisassemble); changed |= ImGui::Checkbox("Keep original bytes for Restore", &config.AssemblerKeepOriginalBytes); if (changed) saveRuntimeConfiguration();
        }

        void drawAssembler(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); auto& assembler = ui.Assembler; configureAssemblerEditor(assembler);
            if (!assembler.Active) { ImGui::TextDisabled("Right-click an instruction or selection and choose Assemble / patch to open an in-place patch here."); return; }
            const auto modules = enumerateRuntimeModules(state.Pid); ImGui::Text("Patch at %s | selected span %zu bytes", runtimeFormatProcessAddress(modules, assembler.Address).c_str(), assembler.Span); ImGui::SameLine(); if (assembler.NopMode) ImGui::TextColored(ImVec4(0.95f,0.72f,0.28f,1.0f), "NOP mode");
            if (assembler.Editor.Render("##AssemblerEditor", ImVec2(-1.0f, 118.0f), ImGuiChildFlags_Borders)) { assembler.NopMode = false; assembler.Dirty = true; assembler.LastEdit = runtimeSteadySeconds(); assembler.ConfirmArmed = false; }
            const double now = runtimeSteadySeconds(); if (assembler.Dirty && (assembler.LastEdit == 0.0 || now - assembler.LastEdit >= 0.25)) rebuildAssemblerPreview(state);
            if (assembler.PreviewValid)
            {
                ImGui::SeparatorText("Encoding preview"); ImGui::TextDisabled("Original: %s", runtimeFormatHexBytes(assembler.Original).c_str()); ImGui::TextWrapped("Patch:    %s", runtimeFormatHexBytes(assembler.Patch).c_str());
                if (assembler.Assembled.size() < assembler.Span && !runtimeConfiguration().AssemblerFillNops) ImGui::TextColored(ImVec4(0.95f,0.72f,0.28f,1.0f), "Replacement is shorter than the selected span; trailing original bytes will remain executable.");
                ImGui::BeginChild("##AssemblerPreview", ImVec2(0.0f, 120.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar); ImGui::TextUnformatted(assembler.PreviewText.c_str()); ImGui::EndChild();
            }
            if (!assembler.Error.empty()) ImGui::TextWrapped("%s", assembler.Error.c_str());
            ImGui::BeginDisabled(!assembler.PreviewValid); const char* label = runtimeConfiguration().AssemblerRequireConfirmation && assembler.ConfirmArmed ? "Confirm patch" : "Write patch"; if (ImGui::Button(label)) writeAssemblerPatch(state); ImGui::EndDisabled(); ImGui::SameLine(); if (ImGui::Button("Rebuild preview")) rebuildAssemblerPreview(state); ImGui::SameLine(); if (ImGui::Button("Cancel")) { assembler.Active = false; assembler.ConfirmArmed = false; }
        }

        void drawPatchHistory(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); if (ui.PatchHistory.empty()) { ImGui::TextDisabled("No restorable assembler patches in this session."); return; }
            for (std::size_t i = ui.PatchHistory.size(); i-- > 0;)
            {
                auto& patch = ui.PatchHistory[i]; if (patch.Pid != state.Pid) continue; ImGui::PushID(static_cast<int>(i)); const std::string address = runtimeFormatProcessAddress(state.Pid, patch.Address); ImGui::Text("%s  %s", address.c_str(), patch.Label.c_str()); ImGui::SameLine();
                if (ImGui::SmallButton("Restore")) { std::string error; if (runtimeWriteProcessMemory(patch.Pid, patch.Address, patch.Original, error)) { state.Status = "restored original patch bytes"; if (runtimeConfiguration().AssemblerAutoRedisassemble) refreshDisassemblyBytes(state, true); } else state.Status = "restore failed: " + error; }
                ImGui::SameLine(); if (ImGui::SmallButton("Copy address")) ImGui::SetClipboardText(address.c_str()); ImGui::PopID();
            }
        }

        void drawRegisterSnapshot(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); if (!ui.HasCapturedHit || ui.CapturedHit.Pid != state.Pid || !ui.CapturedHit.HasRegisters) { ImGui::TextDisabled("No one-shot execution capture for this process yet. Right-click a disassembly line and choose Capture registers when executed."); return; }
            const RuntimeX86Mode mode = runtimeProcessX86Mode(state.Pid); const auto registers = registerValues(ui.CapturedHit, mode); const auto modules = enumerateRuntimeModules(state.Pid); ImGui::TextDisabled("Captured at %s | TID %d | %s", runtimeFormatProcessAddress(modules, ui.CapturedHit.Address).c_str(), ui.CapturedHit.Tid, mode == RuntimeX86Mode::X86 ? "32-bit register view" : "64-bit register view");
            if (!ImGui::BeginTable("ExecutionProbeRegisters", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 235.0f))) return;
            ImGui::TableSetupColumn("Register", ImGuiTableColumnFlags_WidthFixed, 90.0f); ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("Register", ImGuiTableColumnFlags_WidthFixed, 90.0f); ImGui::TableSetupColumn("Value"); ImGui::TableHeadersRow();
            for (std::size_t pair = 0; pair < (registers.size() + 1) / 2; ++pair)
            {
                ImGui::TableNextRow(); for (std::size_t side = 0; side < 2; ++side)
                {
                    const std::size_t index = pair + side * ((registers.size() + 1) / 2); ImGui::TableNextColumn(); if (index >= registers.size()) { ImGui::TableNextColumn(); continue; }
                    const auto& reg = registers[index]; ImGui::TextUnformatted(reg.Name); ImGui::TableNextColumn(); ImGui::PushID(static_cast<int>(index)); const std::string hex = runtimeHexAddress(static_cast<std::uintptr_t>(reg.Value)), symbolic = runtimeFormatProcessAddress(modules, static_cast<std::uintptr_t>(reg.Value));
                    ImGui::Selectable(symbolic.c_str(), ui.SynchronizeAddress && ui.SyncedAddress == static_cast<std::uintptr_t>(reg.Value), ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeight()));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s = %s\nabsolute: %s\nunsigned: %llu\nsigned: %lld", reg.Name, symbolic.c_str(), hex.c_str(), static_cast<unsigned long long>(reg.Value), static_cast<long long>(reg.Value));
                    if (ImGui::BeginPopupContextItem("ExecutionRegisterContext")) { if (ImGui::MenuItem("Copy symbolic address")) ImGui::SetClipboardText(symbolic.c_str()); if (ImGui::MenuItem("Copy hexadecimal")) ImGui::SetClipboardText(hex.c_str()); if (ImGui::MenuItem("Copy decimal")) { const std::string text = std::to_string(reg.Value); ImGui::SetClipboardText(text.c_str()); } ImGui::Separator(); ImGui::BeginDisabled(reg.Value == 0); if (ImGui::MenuItem("Inspect/disassemble value as address")) ui.PendingInspectorAddress = static_cast<std::uintptr_t>(reg.Value); if (ImGui::MenuItem("Synchronize address")) { ui.SyncedAddress = static_cast<std::uintptr_t>(reg.Value); applyDisassemblyMarkers(state); } ImGui::EndDisabled(); ImGui::EndPopup(); } ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }

        void installAssemblyHover(RuntimeMemoryInspectorState& state, TextEditor& editor, const bool mappedLines)
        {
            auto& ui = enhancedMemoryInspectorState(); editor.SetTextHoverCallback([&state,&ui,&editor,mappedLines](TextEditor::PopupData& data)
            {
                const std::string token = tokenAt(editor, data.pos); bool rendered = false; const auto modules = enumerateRuntimeModules(state.Pid);
                if (!token.empty() && knownRegister(token))
                {
                    rendered = true; ImGui::TextUnformatted(upper(token).c_str()); ImGui::Separator();
                    if (ui.HasCapturedHit && ui.CapturedHit.Pid == state.Pid && ui.CapturedHit.HasRegisters)
                    {
                        const auto value = registerValue(ui.CapturedHit, token); if (value) { const std::string absolute = runtimeHexAddress(static_cast<std::uintptr_t>(*value)), symbolic = runtimeFormatProcessAddress(modules, static_cast<std::uintptr_t>(*value)); ImGui::Text("%s", symbolic.c_str()); if (symbolic != absolute) ImGui::TextDisabled("absolute %s", absolute.c_str()); ImGui::TextDisabled("captured at %s | TID %d", runtimeFormatProcessAddress(modules, ui.CapturedHit.Address).c_str(), ui.CapturedHit.Tid); }
                        else ImGui::TextDisabled("<uncaptured alias>");
                    }
                    else { ImGui::TextDisabled("<uncaptured>"); ImGui::TextWrapped("Right-click an instruction and capture registers when it executes."); }
                }
                const DisassemblyInstruction* instruction = nullptr; if (mappedLines) if (const auto index = instructionIndexForDisplayLine(ui, data.pos.line)) instruction = &ui.Instructions[*index];
                std::string mnemonic; if (instruction) { const auto space = instruction->Decoded.Text.find_first_of(" \t"); mnemonic = lower(instruction->Decoded.Text.substr(0, space)); } else { const std::string line = editor.GetLineText(data.pos.line); const auto first = line.find_first_not_of(" \t"); if (first != std::string::npos) { const auto end = line.find_first_of(" \t", first); mnemonic = lower(line.substr(first, end - first)); } }
                if (!token.empty() && lower(token) == mnemonic) if (const char* description = mnemonicDescription(mnemonic)) { if (rendered) ImGui::Separator(); rendered = true; ImGui::Text("%s", mnemonic.c_str()); ImGui::TextWrapped("%s", description); }
                if (instruction && instruction->Decoded.Target && (instruction->Decoded.Branch == RuntimeBranchKind::Conditional || instruction->Decoded.Branch == RuntimeBranchKind::Unconditional || instruction->Decoded.Branch == RuntimeBranchKind::Call))
                {
                    if (rendered) ImGui::Separator(); rendered = true; ImGui::Text("Target: %s", runtimeFormatProcessAddress(modules, *instruction->Decoded.Target).c_str());
                    if (instruction->Decoded.Branch == RuntimeBranchKind::Conditional)
                    {
                        if (ui.HasCapturedHit && ui.CapturedHit.Pid == state.Pid && ui.CapturedHit.Address == instruction->Address)
                        {
                            if (const auto condition = branchCondition(mnemonic, ui.CapturedHit.Registers.eflags)) ImGui::TextColored(*condition ? ImVec4(0.35f,0.90f,0.55f,1.0f) : ImVec4(0.95f,0.40f,0.38f,1.0f), "Captured condition: %s (%s)", *condition ? "true" : "false", *condition ? "branch taken" : "fall-through");
                        }
                        else ImGui::TextDisabled("Condition result uncaptured at this exact instruction.");
                    }
                }
                if (!rendered) ImGui::CloseCurrentPopup();
            });
        }

        void drawArrowHead(ImDrawList* draw, const ImVec2 tip, const bool downward, const ImU32 color)
        {
            const float s = 4.0f; if (downward) draw->AddTriangleFilled(tip, ImVec2(tip.x - s, tip.y - s * 1.5f), ImVec2(tip.x + s, tip.y - s * 1.5f), color); else draw->AddTriangleFilled(tip, ImVec2(tip.x - s, tip.y + s * 1.5f), ImVec2(tip.x + s, tip.y + s * 1.5f), color);
        }

        void drawBranchLanes(RuntimeMemoryInspectorState& state, const ImVec2 min, const ImVec2 max)
        {
            auto& ui = enhancedMemoryInspectorState(); if (ui.Instructions.empty()) return; const float width = 92.0f, left = max.x - width, lineHeight = std::max(state.Disassembly.GetLineHeight(), ImGui::GetTextLineHeight()); const std::size_t firstRow = state.Disassembly.GetFirstVisibleRow(), lastRow = state.Disassembly.GetLastVisibleRow(); auto* draw = ImGui::GetWindowDrawList();
            ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_ChildBg); bg.w = 0.72f; draw->AddRectFilled(ImVec2(left,min.y), max, ImGui::ColorConvertFloat4ToU32(bg)); draw->AddLine(ImVec2(left,min.y), ImVec2(left,max.y), ImGui::GetColorU32(ImGuiCol_Border));
            std::unordered_map<std::uintptr_t,std::size_t> lineFor; for (const auto& instruction : ui.Instructions) lineFor[instruction.Address] = instruction.DisplayLine;
            std::size_t branchIndex = 0;
            for (std::size_t i = 0; i < ui.Instructions.size(); ++i)
            {
                const auto& instruction = ui.Instructions[i]; if (instruction.Decoded.Branch == RuntimeBranchKind::None || instruction.Decoded.Branch == RuntimeBranchKind::Return || !instruction.Decoded.Target) continue; const std::size_t sourceRow = state.Disassembly.DocPos2VisPos({instruction.DisplayLine,0}).row; if (sourceRow + 1 < firstRow || sourceRow > lastRow + 1) { ++branchIndex; continue; }
                const float sourceY = min.y + (static_cast<float>(sourceRow) - static_cast<float>(firstRow) + 0.5f) * lineHeight; const float laneX = max.x - 12.0f - static_cast<float>(branchIndex % 6) * 11.0f; ++branchIndex;
                const auto targetIt = lineFor.find(*instruction.Decoded.Target); const bool targetVisible = targetIt != lineFor.end(); std::size_t targetRow = 0; if (targetVisible) targetRow = state.Disassembly.DocPos2VisPos({targetIt->second,0}).row; const bool targetAbove = !targetVisible ? *instruction.Decoded.Target < instruction.Address : targetRow < firstRow; const bool targetBelow = !targetVisible ? *instruction.Decoded.Target > instruction.Address : targetRow > lastRow; float targetY = targetAbove ? min.y + 5.0f : targetBelow ? max.y - 5.0f : min.y + (static_cast<float>(targetRow) - static_cast<float>(firstRow) + 0.5f) * lineHeight;
                float targetAlpha = 0.85f, fallAlpha = 0.65f; std::optional<bool> captured;
                if (instruction.Decoded.Branch == RuntimeBranchKind::Conditional && ui.HasCapturedHit && ui.CapturedHit.Pid == state.Pid && ui.CapturedHit.Address == instruction.Address)
                {
                    const auto space = instruction.Decoded.Text.find_first_of(" \t"); captured = branchCondition(instruction.Decoded.Text.substr(0, space), ui.CapturedHit.Registers.eflags); if (captured) { targetAlpha = *captured ? 1.0f : 0.24f; fallAlpha = *captured ? 0.24f : 1.0f; }
                }
                ImU32 targetColor = instruction.Decoded.Branch == RuntimeBranchKind::Conditional ? ImGui::ColorConvertFloat4ToU32(ImVec4(0.28f,0.88f,0.48f,targetAlpha)) : instruction.Decoded.Branch == RuntimeBranchKind::Call ? ImGui::ColorConvertFloat4ToU32(ImVec4(0.68f,0.48f,0.95f,0.85f)) : ImGui::ColorConvertFloat4ToU32(ImVec4(0.32f,0.68f,0.96f,0.85f));
                draw->AddLine(ImVec2(left + 3.0f,sourceY), ImVec2(laneX,sourceY), targetColor, 1.5f); draw->AddLine(ImVec2(laneX,sourceY), ImVec2(laneX,targetY), targetColor, 1.5f);
                if (!targetAbove && !targetBelow) { draw->AddLine(ImVec2(laneX,targetY), ImVec2(left + 3.0f,targetY), targetColor, 1.5f); draw->AddTriangleFilled(ImVec2(left + 2.0f,targetY), ImVec2(left + 8.0f,targetY - 3.5f), ImVec2(left + 8.0f,targetY + 3.5f), targetColor); } else drawArrowHead(draw, ImVec2(laneX,targetY), targetBelow, targetColor);
                if (instruction.Decoded.Branch == RuntimeBranchKind::Conditional && i + 1 < ui.Instructions.size())
                {
                    const std::size_t fallRow = state.Disassembly.DocPos2VisPos({ui.Instructions[i + 1].DisplayLine,0}).row; const float fallY = min.y + (static_cast<float>(fallRow) - static_cast<float>(firstRow) + 0.5f) * lineHeight; const ImU32 failColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.95f,0.34f,0.34f,fallAlpha)); const float failX = left + 15.0f; draw->AddLine(ImVec2(left + 3.0f,sourceY), ImVec2(failX,sourceY), failColor, 1.4f); draw->AddLine(ImVec2(failX,sourceY), ImVec2(failX,fallY), failColor, 1.4f); drawArrowHead(draw, ImVec2(failX,fallY), true, failColor);
                }
            }
            draw->AddText(ImVec2(left + 4.0f,min.y + 4.0f), ImGui::ColorConvertFloat4ToU32(ImVec4(0.28f,0.88f,0.48f,0.8f)), "T"); draw->AddText(ImVec2(left + 18.0f,min.y + 4.0f), ImGui::ColorConvertFloat4ToU32(ImVec4(0.95f,0.34f,0.34f,0.8f)), "F");
        }

        void drawBookmarks(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); const auto modules = enumerateRuntimeModules(state.Pid); bool any = false; std::optional<std::pair<pid_t,std::uintptr_t>> erase;
            for (auto& [key, marker] : ui.Markers)
            {
                if (key.first != state.Pid) continue; any = true; ImGui::PushID(reinterpret_cast<void*>(key.second)); const std::string address = runtimeFormatProcessAddress(modules, key.second); ImGui::ColorButton("##bookmark-color", marker.Color, ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetTextLineHeight(),ImGui::GetTextLineHeight())); ImGui::SameLine(); ImGui::Text("%s  %s", marker.Tag.data(), address.c_str()); ImGui::SameLine(); if (ImGui::SmallButton("Go")) ui.PendingInspectorAddress = key.second; ImGui::SameLine(); if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(address.c_str()); ImGui::SameLine(); if (ImGui::SmallButton("Remove")) erase = key; ImGui::PopID();
            }
            if (erase) { ui.Markers.erase(*erase); ui.DisassemblyDirty = true; } if (!any) ImGui::TextDisabled("No bookmarks for this process yet. Right-click a disassembly instruction to add one.");
        }

        void drawPatchBytes(RuntimeMemoryInspectorState& state)
        {
            ImGui::TextDisabled("Editable hexadecimal bytes for the current block. This legacy raw patcher keeps the exact byte count; assembler mode is safer for instruction patches."); ImGui::InputTextMultiline("##hexPatch2", state.HexEdit.data(), state.HexEdit.size(), ImVec2(-1.0f,125.0f)); const char* writeLabel = state.WriteConfirm == 0 ? "Write changes" : "Confirm write";
            if (ImGui::Button(writeLabel))
            {
                std::vector<std::uint8_t> bytes; std::string error; if (!runtimeParseHexBytes(state.HexEdit.data(), bytes, error)) { state.Status = error; state.WriteConfirm = 0; } else if (bytes.size() != state.Original.size()) { state.Status = "patch byte count must stay at " + std::to_string(state.Original.size()); state.WriteConfirm = 0; } else if (state.WriteConfirm == 0) { state.Patched = std::move(bytes); state.WriteConfirm = 1; state.Status = "write armed; click Confirm write to modify process memory"; } else if (runtimeWriteProcessMemory(state.Pid, state.Address, bytes, error)) { state.Status = "memory written"; refreshEnhancedRuntimeMemoryInspector(state); } else { state.Status = "memory write failed: " + error; state.WriteConfirm = 0; }
            }
            ImGui::SameLine(); if (ImGui::Button("Cancel write")) { state.WriteConfirm = 0; state.Status = "write cancelled"; }
        }
    }

    void refreshEnhancedRuntimeMemoryInspector(RuntimeMemoryInspectorState& state)
    {
        auto& ui = enhancedMemoryInspectorState(); state.ReadSize = std::clamp(state.ReadSize, 16, 4096); if (state.Pid <= 0 || state.Address == 0) { state.Original.clear(); ui.RawBytes.clear(); state.Disassembly.SetText({}); ui.Instructions.clear(); ui.DisplayLineAddresses.clear(); state.Status = "select a process and enter an address"; return; }
        if (!refreshDisassemblyBytes(state, true)) return; refreshRawBytes(state); ui.LastPid = state.Pid; ui.LastAddress = state.Address; ui.LastReadSize = state.ReadSize; if (ui.SyncedAddress < state.Address || ui.SyncedAddress >= state.Address + state.Original.size()) ui.SyncedAddress = state.Address; state.Status = "read " + std::to_string(state.Original.size()) + " bytes as " + runtimeX86ModeName(runtimeProcessX86Mode(state.Pid)); syncInspectorAddressText(state); requestRuntimeFunctionAnalysis(state.Pid, state.Address);
    }

    void drawEnhancedRuntimeMemoryInspector(RuntimeMemoryInspectorState& state)
    {
        auto& ui = enhancedMemoryInspectorState(); auto& probe = executionProbe(); auto& configuration = runtimeConfiguration(); syncInspectorAddressText(state); if ((ui.LastPid != state.Pid || ui.LastAddress != state.Address || ui.LastReadSize != state.ReadSize) && state.Pid > 0 && state.Address != 0) refreshEnhancedRuntimeMemoryInspector(state); configureDisassemblyEditor(state);
        if (const auto hit = probe.hit(); hit && hit->HasRegisters && hit->Pid == state.Pid && hit->Time > ui.LastProbeHitTime) { ui.CapturedHit = *hit; ui.HasCapturedHit = true; ui.LastProbeHitTime = hit->Time; ui.SyncedAddress = hit->Address; ui.SelectRegistersTab = true; state.Status = "captured registers at " + runtimeFormatProcessAddress(state.Pid, hit->Address) + " on TID " + std::to_string(hit->Tid); ui.DisassemblyDirty = true; }
        const bool probeRunning = probe.running(); const pid_t probePid = probe.pid(); const std::uintptr_t probeAddress = probe.address(); if (probeRunning != ui.LastProbeRunning || probePid != ui.LastProbePid || probeAddress != ui.LastProbeAddress) ui.DisassemblyDirty = true;
        if (configuration.FunctionHeuristics && state.Pid > 0 && state.Address) { requestRuntimeFunctionAnalysis(state.Pid, state.Address); const auto snapshot = runtimeFunctionAnalysisSnapshot(state.Pid, state.Address); if (snapshot.Revision != ui.FunctionRevision) { ui.FunctionAnalysis = snapshot; ui.FunctionRevision = snapshot.Revision; ui.DisassemblyDirty = true; } }
        const double now = runtimeSteadySeconds(); if (configuration.DisassemblyRefreshHz > 0.0f && state.Pid > 0 && state.Address && now - ui.LastDisassemblyRefresh >= 1.0 / configuration.DisassemblyRefreshHz) refreshDisassemblyBytes(state, false); if (ui.DisassemblyDirty && !state.Original.empty()) rebuildDisassembly(state);

        ImGui::SeparatorText("Memory / disassembly"); int pidValue = static_cast<int>(state.Pid); ImGui::SetNextItemWidth(105.0f); if (ImGui::InputInt("PID##memory2", &pidValue)) { const pid_t old = state.Pid; state.Pid = static_cast<pid_t>(std::max(pidValue,0)); ui.LastPid = 0; if (old != state.Pid) invalidateRuntimeFunctionAnalysis(old); } ImGui::SameLine();
        const bool addressEnter = ui::drawAddressInput("Address##memory2", ui.AddressText.data(), ui.AddressText.size(), state.Pid, 260.0f, ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine(); ImGui::SetNextItemWidth(105.0f); if (ImGui::InputInt("Bytes##memory2", &state.ReadSize)) state.ReadSize = std::clamp(state.ReadSize,16,4096); ImGui::SameLine(); const bool readPressed = ImGui::Button("Read / disassemble");
        if (addressEnter || readPressed) { std::uintptr_t address = 0; std::string error; if (!ui::evaluateAddressExpression(state.Pid, ui.AddressText.data(), address, error) || address == 0) state.Status = error.empty() ? "invalid memory address" : error; else { state.Address = address; ui.AddressTextValue = address; refreshEnhancedRuntimeMemoryInspector(state); } }
        ImGui::SameLine(); ImGui::TextDisabled("%s", runtimeX86ModeName(runtimeProcessX86Mode(state.Pid))); if (ImGui::SmallButton("Refresh")) refreshEnhancedRuntimeMemoryInspector(state); ImGui::SameLine(); ImGui::Checkbox("Synchronize address", &ui.SynchronizeAddress); if (ui.SynchronizeAddress) { ImGui::SameLine(); ImGui::TextDisabled("synced %s", runtimeFormatProcessAddress(state.Pid, ui.SyncedAddress).c_str()); }
        if (!state.Status.empty()) ImGui::TextDisabled("%s", state.Status.c_str()); if (state.Original.empty()) return;

        ImGui::SeparatorText("Disassembly"); if (probeRunning && probePid == state.Pid) { ImGui::TextColored(ImVec4(1.0f,0.30f,0.30f,1.0f), "ARMED %s — armed for next execution", runtimeFormatProcessAddress(state.Pid, probeAddress).c_str()); ImGui::SameLine(); if (ImGui::SmallButton("Cancel probe")) { probe.stop(); ui.DisassemblyDirty = true; } } else if (ui.HasCapturedHit && ui.CapturedHit.Pid == state.Pid) { ImGui::TextDisabled("Last one-shot capture: %s on TID %d", runtimeFormatProcessAddress(state.Pid, ui.CapturedHit.Address).c_str(), ui.CapturedHit.Tid); ImGui::SameLine(); if (ImGui::SmallButton("Arm again")) { std::string error; if (!probe.start(state.Pid, ui.CapturedHit.Address, error)) state.Status = error; else { state.Status = "armed for next execution"; ui.DisassemblyDirty = true; } } }
        if (ui.FunctionAnalysis.Running) { ImGui::ProgressBar(ui.FunctionAnalysis.Progress, ImVec2(180.0f,0.0f), "function analysis"); ImGui::SameLine(); ImGui::TextDisabled("background / low priority — %zu candidates appear live", ui.FunctionAnalysis.Candidates.size()); } else if (!ui.FunctionAnalysis.Status.empty()) ImGui::TextDisabled("%s | %zu function hints", ui.FunctionAnalysis.Status.c_str(), ui.FunctionAnalysis.Candidates.size());
        ImGui::TextDisabled("Scroll past either edge to keep disassembling. Green branch lanes are condition-true/taken; red is fall-through. Captured flags emphasize the path actually observed at that instruction.");

        installAssemblyHover(state, state.Disassembly, true); state.Disassembly.SetTextContextMenuCallback([&](TextEditor::PopupData& data)
        {
            const auto index = instructionIndexForDisplayLine(ui, data.pos.line); if (!index) return; const auto& instruction = ui.Instructions[*index]; const std::uintptr_t address = instruction.Address; const auto key = std::pair{state.Pid,address}; const auto modules = enumerateRuntimeModules(state.Pid); ImGui::TextDisabled("%s", runtimeFormatProcessAddress(modules,address).c_str());
            if (ImGui::MenuItem("Synchronize address here")) { ui.SyncedAddress = address; applyDisassemblyMarkers(state); } if (ImGui::MenuItem("Use as inspector base")) ui.PendingInspectorAddress = address; if (ImGui::MenuItem("Copy module-relative address")) { const std::string value = runtimeFormatProcessAddress(modules,address); ImGui::SetClipboardText(value.c_str()); } if (ImGui::MenuItem("Copy absolute address")) { const std::string value = runtimeHexAddress(address); ImGui::SetClipboardText(value.c_str()); } if (ImGui::MenuItem("Copy instruction line")) { const std::string value = state.Disassembly.GetLineText(data.pos.line); ImGui::SetClipboardText(value.c_str()); }
            const bool hasSelection = state.Disassembly.CurrentCursorHasSelection(); std::size_t first = *index, last = *index; if (hasSelection) { const auto selection = state.Disassembly.GetCurrentCursorSelection(); for (std::size_t i = 0; i < ui.Instructions.size(); ++i) if (ui.Instructions[i].DisplayLine >= selection.start.line && ui.Instructions[i].DisplayLine <= selection.end.line) { first = std::min(first,i); last = std::max(last,i); } }
            if (ImGui::MenuItem(hasSelection ? "Create signature from selection" : "Create signature here")) ui::requestSignatureMaker(state.Pid, ui.Instructions[first].Address, static_cast<int>(last - first + 1));
            ImGui::Separator(); if (ImGui::MenuItem(hasSelection ? "Assemble / patch selection" : "Assemble / patch instruction")) openAssembler(state, first, last, false); if (ImGui::MenuItem(hasSelection ? "NOP selection" : "NOP instruction")) openAssembler(state, first, last, true);
            ImGui::Separator(); const RuntimeFunctionCandidate* function = functionForAddress(ui.FunctionAnalysis,address); if (function)
            {
                const std::string name = functionName(ui,state.Pid,modules,*function); ImGui::SeparatorText(name.c_str()); if (ImGui::MenuItem("Go to function start")) ui.PendingInspectorAddress = function->Address; if (ImGui::MenuItem("Copy function address")) { const std::string value = runtimeFormatProcessAddress(modules,function->Address); ImGui::SetClipboardText(value.c_str()); }
                auto& rename = ui.FunctionNames[{state.Pid,function->Address}]; ImGui::InputText("Function name", rename.data(), rename.size()); if (ImGui::IsItemDeactivatedAfterEdit()) ui.DisassemblyDirty = true;
            }
            if (ImGui::MenuItem("Analyze functions around here")) requestRuntimeFunctionAnalysis(state.Pid,address,true);
            ImGui::Separator(); const bool thisProbe = probe.running() && probe.pid() == state.Pid && probe.address() == address; if (thisProbe) { if (ImGui::MenuItem("Cancel execution probe")) { probe.stop(); ui.DisassemblyDirty = true; state.Status = "execution probe cancelled"; } } else { ImGui::BeginDisabled(probe.running()); if (ImGui::MenuItem("Capture registers when executed")) { std::string error; if (!probe.start(state.Pid,address,error)) state.Status = error; else { ui.SyncedAddress = address; ui.DisassemblyDirty = true; state.Status = "armed for next execution"; } } ImGui::EndDisabled(); }
            ImGui::Separator(); auto marker = ui.Markers.find(key); if (marker == ui.Markers.end()) { if (ImGui::MenuItem("Add bookmark / marker")) { ui.Markers.emplace(key,DisassemblyMarker{}); ui.DisassemblyDirty = true; } } else { ImGui::SeparatorText("Bookmark"); if (ImGui::InputText("Tag",marker->second.Tag.data(),marker->second.Tag.size())) ui.DisassemblyDirty = true; if (ImGui::ColorEdit4("Color",&marker->second.Color.x,ImGuiColorEditFlags_AlphaBar)) { applyDisassemblyMarkers(state); ui.DisassemblyDirty = true; } if (ImGui::MenuItem("Remove bookmark")) { ui.Markers.erase(marker); ui.DisassemblyDirty = true; } }
        });
        state.Disassembly.Render("##memory-disassembly", ImVec2(-1.0f,390.0f), ImGuiChildFlags_Borders); const ImVec2 disassemblyMin = ImGui::GetItemRectMin(), disassemblyMax = ImGui::GetItemRectMax(); drawBranchLanes(state,disassemblyMin,disassemblyMax);
        const bool disassemblyHovered = ImGui::IsMouseHoveringRect(disassemblyMin,disassemblyMax); if (ui.SynchronizeAddress && state.Disassembly.IsMousePosOverTextArea(ImGui::GetMousePos()) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { const auto pos = state.Disassembly.GetDocPosAtMousePos(ImGui::GetMousePos()); if (pos.line < ui.DisplayLineAddresses.size() && ui.DisplayLineAddresses[pos.line]) { ui.SyncedAddress = ui.DisplayLineAddresses[pos.line]; applyDisassemblyMarkers(state); } }
        if (disassemblyHovered && ImGui::GetIO().MouseWheel != 0.0f && !ui.Instructions.empty())
        {
            const std::size_t firstVisible = state.Disassembly.GetFirstVisibleRow(), lastVisible = state.Disassembly.GetLastVisibleRow(); const std::uintptr_t shift = static_cast<std::uintptr_t>(std::max(state.ReadSize / 2, 64));
            if (ImGui::GetIO().MouseWheel < 0.0f && lastVisible + 3 >= ui.DisplayLineAddresses.size()) { std::uintptr_t anchor = ui.Instructions[std::min<std::size_t>(ui.Instructions.size() / 2,ui.Instructions.size() - 1)].Address; setInspectorAddress(state,state.Address + shift,anchor); }
            else if (ImGui::GetIO().MouseWheel > 0.0f && firstVisible <= 2 && state.Address > shift) { const std::uintptr_t anchor = ui.Instructions.front().Address; setInspectorAddress(state,state.Address - shift,anchor); }
        }
        if (ui.DisassemblyDirty) rebuildDisassembly(state);

        if (ImGui::BeginTabBar("MemoryInspectorLowerTabs"))
        {
            if (ImGui::BeginTabItem("Raw bytes"))
            {
                if (configuration.RawBytesRefreshHz > 0.0f && now - ui.LastRawRefresh >= 1.0 / configuration.RawBytesRefreshHz) refreshRawBytes(state); ImGui::TextDisabled("16 bytes per row, grouped 8 + 8. Automatic raw refresh %.2f Hz.", configuration.RawBytesRefreshHz); const auto modules = enumerateRuntimeModules(state.Pid); const auto bytes = std::span<const std::uint8_t>(ui.RawBytes.empty() ? state.Original : ui.RawBytes);
                if (ImGui::BeginChild("##GroupedRawMemory",ImVec2(0.0f,235.0f),ImGuiChildFlags_Borders,ImGuiWindowFlags_HorizontalScrollbar))
                {
                    const int lines = static_cast<int>((bytes.size()+15)/16); ImGuiListClipper clipper; clipper.Begin(lines); while (clipper.Step()) for (int line=clipper.DisplayStart;line<clipper.DisplayEnd;++line) { const std::size_t offset=static_cast<std::size_t>(line)*16,count=std::min<std::size_t>(16,bytes.size()-offset); const std::uintptr_t address=state.Address+offset; const bool active=ui.SynchronizeAddress&&ui.SyncedAddress>=address&&ui.SyncedAddress<address+count; const std::string text=groupedBytesLine(bytes,state.Address,offset,count,modules); ImGui::PushID(line); if (ImGui::Selectable("##raw-line",active,ImGuiSelectableFlags_AllowOverlap,ImVec2(0.0f,ImGui::GetTextLineHeight()))&&ui.SynchronizeAddress) { ui.SyncedAddress=address; applyDisassemblyMarkers(state); } ImGui::SameLine(); ImGui::TextUnformatted(text.c_str()); if (ImGui::BeginPopupContextItem("RawLineContext")) { if (ImGui::MenuItem("Use row as inspector base")) ui.PendingInspectorAddress=address; if (ImGui::MenuItem("Copy module-relative address")) { const std::string value=runtimeFormatProcessAddress(modules,address); ImGui::SetClipboardText(value.c_str()); } if (ImGui::MenuItem("Copy 16-byte group")) { const std::string value=runtimeFormatHexBytes(bytes.subspan(offset,count)); ImGui::SetClipboardText(value.c_str()); } ImGui::EndPopup(); } ImGui::PopID(); }
                }
                ImGui::EndChild(); ImGui::EndTabItem();
            }
            const ImGuiTabItemFlags registerFlags=ui.SelectRegistersTab?ImGuiTabItemFlags_SetSelected:ImGuiTabItemFlags_None; if (ImGui::BeginTabItem("Registers",nullptr,registerFlags)) { ui.SelectRegistersTab=false; drawRegisterSnapshot(state); ImGui::EndTabItem(); }
            const ImGuiTabItemFlags assemblerFlags=ui.SelectAssemblerTab?ImGuiTabItemFlags_SetSelected:ImGuiTabItemFlags_None; if (ImGui::BeginTabItem("Assembler",nullptr,assemblerFlags)) { ui.SelectAssemblerTab=false; installAssemblyHover(state,ui.Assembler.Editor,false); drawAssembler(state); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Assembler Tweaks")) { drawAssemblerTweaks(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Bookmarks")) { drawBookmarks(state); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Patch history")) { drawPatchHistory(state); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Patch bytes")) { drawPatchBytes(state); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        if (ui.PendingInspectorAddress) { const std::uintptr_t address=*ui.PendingInspectorAddress; ui.PendingInspectorAddress.reset(); setInspectorAddress(state,address); }
    }
}
