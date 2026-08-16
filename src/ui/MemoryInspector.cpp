#include "quartz/client/ui/MemoryInspector.hpp"
#include "quartz/client/ui/AddressInput.hpp"
#include "quartz/client/ui/SignatureMaker.hpp"
#include "quartz/client/Model.hpp"
#include "quartz/client/native/ExecutionProbe.hpp"
#include "quartz/client/native/NativeDisassembly.hpp"
#include "quartz/client/native/OpcodePatternEditor.hpp"

namespace quartz::client
{
    namespace
    {
        struct DisassemblyMarker
        {
            std::array<char, 64> Tag{};
            ImVec4 Color{0.0f, 0.72f, 1.0f, 0.22f};
            DisassemblyMarker() { std::snprintf(Tag.data(), Tag.size(), "%s", "mark"); }
        };

        struct RegisterValue { const char* Name; std::uint64_t Value; };

        struct EnhancedMemoryInspectorState
        {
            pid_t LastPid = 0;
            std::uintptr_t LastAddress = 0;
            int LastReadSize = 0;
            std::uintptr_t AddressTextValue = std::numeric_limits<std::uintptr_t>::max();
            std::array<char, 256> AddressText{};
            std::vector<std::uintptr_t> DisassemblyLines;
            std::map<std::pair<pid_t, std::uintptr_t>, DisassemblyMarker> Markers;
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
            std::optional<std::uintptr_t> PendingInspectorAddress;
        };

        EnhancedMemoryInspectorState& enhancedMemoryInspectorState() { static EnhancedMemoryInspectorState state; return state; }

        void syncInspectorAddressText(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState();
            if (ui.AddressTextValue == state.Address) return;
            std::snprintf(ui.AddressText.data(), ui.AddressText.size(), "0x%llX", static_cast<unsigned long long>(state.Address)); ui.AddressTextValue = state.Address;
        }

        void configureDisassemblyEditor(RuntimeMemoryInspectorState& state)
        {
            state.Disassembly.SetPalette(shaderEditorPalette()); state.Disassembly.SetLanguage(intelAsmPatternLanguage()); state.Disassembly.SetReadOnlyEnabled(true); state.Disassembly.SetCaretsVisible(false); state.Disassembly.SetShowLineNumbersEnabled(false); state.Disassembly.SetShowMiniMapEnabled(true); state.Disassembly.SetWordWrapEnabled(false); state.EditorInitialized = true;
        }

        std::string groupedBytesLine(const RuntimeMemoryInspectorState& state, const std::size_t offset, const std::size_t count)
        {
            std::ostringstream out; out << "0x" << std::hex << std::uppercase << std::setw(sizeof(std::uintptr_t) * 2) << std::setfill('0') << static_cast<unsigned long long>(state.Address + offset) << "  ";
            for (std::size_t i = 0; i < 16; ++i)
            {
                if (i < count) out << std::setw(2) << static_cast<unsigned>(state.Original[offset + i]); else out << "  ";
                out << (i == 7 ? "  " : " ");
            }
            out << " |";
            for (std::size_t i = 0; i < count; ++i) { const unsigned char c = state.Original[offset + i]; out << (std::isprint(c) ? static_cast<char>(c) : '.'); }
            for (std::size_t i = count; i < 16; ++i) out << ' ';
            out << '|'; return out.str();
        }

        std::vector<RegisterValue> registerValues(const ExecutionProbeHit& hit, const RuntimeX86Mode mode)
        {
            const auto& r = hit.Registers; std::vector<RegisterValue> values;
            if (mode == RuntimeX86Mode::X86)
            {
                const auto low = [](const unsigned long long value) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)); };
                values = {{"EAX", low(r.rax)}, {"EBX", low(r.rbx)}, {"ECX", low(r.rcx)}, {"EDX", low(r.rdx)}, {"ESI", low(r.rsi)}, {"EDI", low(r.rdi)}, {"EBP", low(r.rbp)}, {"ESP", low(r.rsp)}, {"EIP", low(r.rip)}, {"EFLAGS", low(r.eflags)}, {"ORIG_EAX", low(r.orig_rax)}, {"CS", r.cs}, {"SS", r.ss}, {"DS", r.ds}, {"ES", r.es}, {"FS", r.fs}, {"GS", r.gs}, {"FS_BASE", r.fs_base}, {"GS_BASE", r.gs_base}};
            }
            else
            {
                values = {{"RAX", r.rax}, {"RBX", r.rbx}, {"RCX", r.rcx}, {"RDX", r.rdx}, {"RSI", r.rsi}, {"RDI", r.rdi}, {"RBP", r.rbp}, {"RSP", r.rsp}, {"R8", r.r8}, {"R9", r.r9}, {"R10", r.r10}, {"R11", r.r11}, {"R12", r.r12}, {"R13", r.r13}, {"R14", r.r14}, {"R15", r.r15}, {"RIP", r.rip}, {"RFLAGS", r.eflags}, {"ORIG_RAX", r.orig_rax}, {"CS", r.cs}, {"SS", r.ss}, {"DS", r.ds}, {"ES", r.es}, {"FS", r.fs}, {"GS", r.gs}, {"FS_BASE", r.fs_base}, {"GS_BASE", r.gs_base}};
            }
            return values;
        }

        void applyDisassemblyMarkers(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); auto& probe = executionProbe(); const bool armed = probe.running() && probe.pid() == state.Pid; const std::uintptr_t armedAddress = armed ? probe.address() : 0;
            state.Disassembly.ClearMarkers();
            for (std::size_t line = 0; line < ui.DisassemblyLines.size(); ++line)
            {
                const std::uintptr_t address = ui.DisassemblyLines[line];
                if (address == armedAddress)
                {
                    state.Disassembly.AddMarker(line, IM_COL32(255, 80, 80, 255), IM_COL32(180, 30, 30, 82), "armed for next execution", "armed for next execution");
                    continue;
                }
                if (const auto marker = ui.Markers.find({state.Pid, address}); marker != ui.Markers.end())
                {
                    ImVec4 fill = marker->second.Color; fill.w = std::clamp(fill.w, 0.08f, 0.55f); const ImVec4 solid{fill.x, fill.y, fill.z, 1.0f}; const std::string_view tag = marker->second.Tag.data();
                    state.Disassembly.AddMarker(line, ImGui::ColorConvertFloat4ToU32(solid), ImGui::ColorConvertFloat4ToU32(fill), tag, tag);
                    continue;
                }
                if (ui.SynchronizeAddress && ui.SyncedAddress == address) state.Disassembly.AddMarker(line, IM_COL32(120, 210, 235, 220), IM_COL32(70, 115, 130, 42), "synchronized address", "synchronized address");
            }
        }

        void rebuildDisassembly(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState(); auto& probe = executionProbe(); const bool armed = probe.running() && probe.pid() == state.Pid; const std::uintptr_t armedAddress = armed ? probe.address() : 0; const RuntimeX86Mode mode = runtimeProcessX86Mode(state.Pid);
            std::ostringstream disassembly; ui.DisassemblyLines.clear(); std::size_t offset = 0;
            while (offset < state.Original.size())
            {
                const std::uintptr_t address = state.Address + offset; ui.DisassemblyLines.push_back(address); std::string text; std::size_t length = 0;
                if (runtimeDecodeProcessInstructionText(mode, std::span<const std::uint8_t>(state.Original).subspan(offset), address, text, length) && length != 0) offset += length;
                else { std::ostringstream fallback; fallback << "db 0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<unsigned>(state.Original[offset]); text = fallback.str(); ++offset; }
                disassembly << runtimeHexAddress(address) << "  " << text;
                if (const auto marker = ui.Markers.find({state.Pid, address}); marker != ui.Markers.end()) { const std::string_view tag = marker->second.Tag.data(); disassembly << " ; [" << (tag.empty() ? "marked" : tag) << ']'; }
                if (address == armedAddress) disassembly << " ; [armed for next execution]";
                disassembly << '\n';
            }
            state.Disassembly.SetText(disassembly.str()); applyDisassemblyMarkers(state); ui.LastProbeRunning = probe.running(); ui.LastProbePid = probe.pid(); ui.LastProbeAddress = probe.address(); ui.DisassemblyDirty = false;
        }

        void setInspectorAddress(RuntimeMemoryInspectorState& state, const std::uintptr_t address)
        {
            state.Address = address; enhancedMemoryInspectorState().AddressTextValue = std::numeric_limits<std::uintptr_t>::max(); syncInspectorAddressText(state); refreshEnhancedRuntimeMemoryInspector(state);
        }

        void drawRegisterSnapshot(RuntimeMemoryInspectorState& state)
        {
            auto& ui = enhancedMemoryInspectorState();
            if (!ui.HasCapturedHit || ui.CapturedHit.Pid != state.Pid || !ui.CapturedHit.HasRegisters) { ImGui::TextDisabled("No one-shot execution capture for this process yet. Right-click a disassembly line and choose Capture registers when executed."); return; }
            const RuntimeX86Mode mode = runtimeProcessX86Mode(state.Pid); const auto registers = registerValues(ui.CapturedHit, mode);
            ImGui::TextDisabled("Captured at %s | TID %d | %s | right-click any register value", runtimeHexAddress(ui.CapturedHit.Address).c_str(), ui.CapturedHit.Tid, mode == RuntimeX86Mode::X86 ? "32-bit register view" : "64-bit register view");
            if (!ImGui::BeginTable("ExecutionProbeRegisters", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 235.0f))) return;
            ImGui::TableSetupColumn("Register", ImGuiTableColumnFlags_WidthFixed, 90.0f); ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("Register", ImGuiTableColumnFlags_WidthFixed, 90.0f); ImGui::TableSetupColumn("Value"); ImGui::TableHeadersRow();
            for (std::size_t pair = 0; pair < (registers.size() + 1) / 2; ++pair)
            {
                ImGui::TableNextRow();
                for (std::size_t side = 0; side < 2; ++side)
                {
                    const std::size_t index = pair + side * ((registers.size() + 1) / 2); ImGui::TableNextColumn();
                    if (index >= registers.size()) { ImGui::TableNextColumn(); continue; }
                    const auto& reg = registers[index]; ImGui::TextUnformatted(reg.Name); ImGui::TableNextColumn(); ImGui::PushID(static_cast<int>(index)); const std::string hex = runtimeHexAddress(static_cast<std::uintptr_t>(reg.Value));
                    ImGui::Selectable(hex.c_str(), ui.SynchronizeAddress && ui.SyncedAddress == static_cast<std::uintptr_t>(reg.Value), ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeight()));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s = %s\nunsigned: %llu\nsigned: %lld", reg.Name, hex.c_str(), static_cast<unsigned long long>(reg.Value), static_cast<long long>(reg.Value));
                    if (ImGui::BeginPopupContextItem("ExecutionRegisterContext"))
                    {
                        if (ImGui::MenuItem("Copy hexadecimal")) ImGui::SetClipboardText(hex.c_str());
                        if (ImGui::MenuItem("Copy decimal")) { const std::string text = std::to_string(reg.Value); ImGui::SetClipboardText(text.c_str()); }
                        if (ImGui::MenuItem("Copy register = value")) { const std::string text = std::string(reg.Name) + " = " + hex; ImGui::SetClipboardText(text.c_str()); }
                        ImGui::Separator(); ImGui::BeginDisabled(reg.Value == 0);
                        if (ImGui::MenuItem("Inspect/disassemble value as address")) ui.PendingInspectorAddress = static_cast<std::uintptr_t>(reg.Value);
                        if (ImGui::MenuItem("Synchronize address")) { ui.SyncedAddress = static_cast<std::uintptr_t>(reg.Value); applyDisassemblyMarkers(state); }
                        ImGui::EndDisabled(); ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }

        void drawPatchBytes(RuntimeMemoryInspectorState& state)
        {
            ImGui::TextDisabled("Editable hexadecimal bytes for the current block. A write requires two clicks; Restore writes the exact block read above back to the process."); ImGui::InputTextMultiline("##hexPatch2", state.HexEdit.data(), state.HexEdit.size(), ImVec2(-1.0f, 125.0f));
            const char* writeLabel = state.WriteConfirm == 0 ? "Write changes" : "Confirm write";
            if (ImGui::Button(writeLabel))
            {
                std::vector<std::uint8_t> bytes; std::string error;
                if (!runtimeParseHexBytes(state.HexEdit.data(), bytes, error)) { state.Status = error; state.WriteConfirm = 0; }
                else if (bytes.size() != state.Original.size()) { state.Status = "patch byte count must stay at " + std::to_string(state.Original.size()); state.WriteConfirm = 0; }
                else if (state.WriteConfirm == 0) { state.Patched = std::move(bytes); state.WriteConfirm = 1; state.Status = "write armed; click Confirm write to modify process memory"; }
                else if (runtimeWriteProcessMemory(state.Pid, state.Address, bytes, error)) { state.Status = "memory written"; refreshEnhancedRuntimeMemoryInspector(state); }
                else { state.Status = "memory write failed: " + error; state.WriteConfirm = 0; }
            }
            ImGui::SameLine(); if (ImGui::Button("Cancel write")) { state.WriteConfirm = 0; state.Status = "write cancelled"; } ImGui::SameLine();
            if (ImGui::Button("Restore original bytes"))
            {
                std::string error;
                if (runtimeWriteProcessMemory(state.Pid, state.Address, state.Original, error)) { state.Status = "original block restored"; refreshEnhancedRuntimeMemoryInspector(state); }
                else state.Status = "restore failed: " + error;
            }
        }
    }

    void refreshEnhancedRuntimeMemoryInspector(RuntimeMemoryInspectorState& state)
    {
        auto& ui = enhancedMemoryInspectorState(); state.ReadSize = std::clamp(state.ReadSize, 1, 4096); state.Original.assign(static_cast<std::size_t>(state.ReadSize), 0); state.Patched.clear(); state.WriteConfirm = 0;
        if (state.Pid <= 0 || state.Address == 0) { state.Original.clear(); state.Disassembly.SetText({}); ui.DisassemblyLines.clear(); state.Status = "select a process and enter an address"; return; }
        std::string error;
        if (!readProcessMemoryBlock(state.Pid, state.Address, std::span<std::uint8_t>(state.Original.data(), state.Original.size()), error)) { state.Original.clear(); state.Disassembly.SetText({}); ui.DisassemblyLines.clear(); state.Status = "memory read failed: " + error; return; }
        state.Patched = state.Original; const std::string formatted = runtimeFormatHexBytes(state.Original); std::snprintf(state.HexEdit.data(), state.HexEdit.size(), "%s", formatted.c_str()); configureDisassemblyEditor(state); rebuildDisassembly(state); ui.LastPid = state.Pid; ui.LastAddress = state.Address; ui.LastReadSize = state.ReadSize; if (ui.SyncedAddress < state.Address || ui.SyncedAddress >= state.Address + state.Original.size()) ui.SyncedAddress = state.Address; state.Status = "read " + std::to_string(state.Original.size()) + " bytes as " + runtimeX86ModeName(runtimeProcessX86Mode(state.Pid)); syncInspectorAddressText(state);
    }

    void drawEnhancedRuntimeMemoryInspector(RuntimeMemoryInspectorState& state)
    {
        auto& ui = enhancedMemoryInspectorState(); auto& probe = executionProbe(); syncInspectorAddressText(state);
        if ((ui.LastPid != state.Pid || ui.LastAddress != state.Address || ui.LastReadSize != state.ReadSize) && state.Pid > 0 && state.Address != 0) refreshEnhancedRuntimeMemoryInspector(state);
        configureDisassemblyEditor(state);

        if (const auto hit = probe.hit(); hit && hit->HasRegisters && hit->Pid == state.Pid && hit->Time > ui.LastProbeHitTime)
        {
            ui.CapturedHit = *hit; ui.HasCapturedHit = true; ui.LastProbeHitTime = hit->Time; ui.SyncedAddress = hit->Address; ui.SelectRegistersTab = true; state.Status = "captured registers at " + runtimeHexAddress(hit->Address) + " on TID " + std::to_string(hit->Tid); ui.DisassemblyDirty = true;
        }
        const bool probeRunning = probe.running(); const pid_t probePid = probe.pid(); const std::uintptr_t probeAddress = probe.address();
        if (probeRunning != ui.LastProbeRunning || probePid != ui.LastProbePid || probeAddress != ui.LastProbeAddress) ui.DisassemblyDirty = true;
        if (ui.DisassemblyDirty && !state.Original.empty()) rebuildDisassembly(state);

        ImGui::SeparatorText("Memory / disassembly");
        int pidValue = static_cast<int>(state.Pid); ImGui::SetNextItemWidth(105.0f); if (ImGui::InputInt("PID##memory2", &pidValue)) { state.Pid = static_cast<pid_t>(std::max(pidValue, 0)); ui.LastPid = 0; } ImGui::SameLine();
        const bool addressEnter = ui::drawAddressInput("Address##memory2", ui.AddressText.data(), ui.AddressText.size(), state.Pid, 260.0f, ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(105.0f); if (ImGui::InputInt("Bytes##memory2", &state.ReadSize)) state.ReadSize = std::clamp(state.ReadSize, 1, 4096); ImGui::SameLine();
        const bool readPressed = ImGui::Button("Read / disassemble");
        if (addressEnter || readPressed)
        {
            std::uintptr_t address = 0; std::string error;
            if (!ui::evaluateAddressExpression(state.Pid, ui.AddressText.data(), address, error) || address == 0) state.Status = error.empty() ? "invalid memory address" : error;
            else { state.Address = address; ui.AddressTextValue = address; refreshEnhancedRuntimeMemoryInspector(state); }
        }
        ImGui::SameLine(); ImGui::TextDisabled("%s", runtimeX86ModeName(runtimeProcessX86Mode(state.Pid)));

        const std::uintptr_t page = static_cast<std::uintptr_t>(std::max(state.ReadSize, 1));
        ImGui::BeginDisabled(state.Address < 0x100); if (ImGui::SmallButton("-0x100")) setInspectorAddress(state, state.Address - 0x100); ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::BeginDisabled(state.Address < page); if (ImGui::SmallButton("Previous block")) setInspectorAddress(state, state.Address - page); ImGui::EndDisabled(); ImGui::SameLine();
        if (ImGui::SmallButton("Next block")) setInspectorAddress(state, state.Address + page); ImGui::SameLine(); if (ImGui::SmallButton("+0x100")) setInspectorAddress(state, state.Address + 0x100); ImGui::SameLine();
        if (ImGui::SmallButton("Refresh")) refreshEnhancedRuntimeMemoryInspector(state); ImGui::SameLine(); ImGui::Checkbox("Synchronize address", &ui.SynchronizeAddress);
        if (ui.SynchronizeAddress) { ImGui::SameLine(); ImGui::TextDisabled("synced %s", runtimeHexAddress(ui.SyncedAddress).c_str()); }
        if (!state.Status.empty()) ImGui::TextDisabled("%s", state.Status.c_str());
        if (state.Original.empty()) return;

        ImGui::SeparatorText("Disassembly");
        if (probeRunning && probePid == state.Pid)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.30f, 0.30f, 1.0f), "ARMED %s — armed for next execution", runtimeHexAddress(probeAddress).c_str()); ImGui::SameLine(); if (ImGui::SmallButton("Cancel probe")) { probe.stop(); ui.DisassemblyDirty = true; }
        }
        else if (ui.HasCapturedHit && ui.CapturedHit.Pid == state.Pid)
        {
            ImGui::TextDisabled("Last one-shot capture: %s on TID %d", runtimeHexAddress(ui.CapturedHit.Address).c_str(), ui.CapturedHit.Tid); ImGui::SameLine();
            if (ImGui::SmallButton("Arm again")) { std::string error; if (!probe.start(state.Pid, ui.CapturedHit.Address, error)) state.Status = error; else { state.Status = "armed for next execution"; ui.DisassemblyDirty = true; } }
        }
        ImGui::TextDisabled("Right-click an instruction to mark/tag/recolor it or arm a one-shot hardware execution probe. User markers default to cyan; an armed probe is red.");
        state.Disassembly.SetTextContextMenuCallback([&](TextEditor::PopupData& data)
        {
            if (data.pos.line >= ui.DisassemblyLines.size()) return; const std::uintptr_t address = ui.DisassemblyLines[data.pos.line]; const auto key = std::pair{state.Pid, address};
            ImGui::TextDisabled("%s", runtimeHexAddress(address).c_str());
            if (ImGui::MenuItem("Synchronize address here")) { ui.SyncedAddress = address; applyDisassemblyMarkers(state); }
            if (ImGui::MenuItem("Use as inspector base")) ui.PendingInspectorAddress = address;
            if (ImGui::MenuItem("Copy address")) { const std::string value = runtimeHexAddress(address); ImGui::SetClipboardText(value.c_str()); }
            if (ImGui::MenuItem("Copy instruction line")) { const std::string value = state.Disassembly.GetLineText(data.pos.line); ImGui::SetClipboardText(value.c_str()); }
            const bool hasSelection = state.Disassembly.CurrentCursorHasSelection();
            if (ImGui::MenuItem(hasSelection ? "Create signature from selection" : "Create signature here"))
            {
                std::size_t firstLine = data.pos.line, lastLine = data.pos.line;
                if (hasSelection)
                {
                    const auto selection = state.Disassembly.GetCurrentCursorSelection(); firstLine = selection.start.line; lastLine = selection.end.line;
                    if (lastLine > firstLine && selection.end.index == 0) --lastLine;
                }
                if (!ui.DisassemblyLines.empty())
                {
                    firstLine = std::min(firstLine, ui.DisassemblyLines.size() - 1); lastLine = std::min(std::max(lastLine, firstLine), ui.DisassemblyLines.size() - 1);
                    ui::requestSignatureMaker(state.Pid, ui.DisassemblyLines[firstLine], static_cast<int>(lastLine - firstLine + 1));
                }
            }
            ImGui::Separator();
            const bool thisProbe = probe.running() && probe.pid() == state.Pid && probe.address() == address;
            if (thisProbe)
            {
                if (ImGui::MenuItem("Cancel execution probe")) { probe.stop(); ui.DisassemblyDirty = true; state.Status = "execution probe cancelled"; }
            }
            else
            {
                ImGui::BeginDisabled(probe.running());
                if (ImGui::MenuItem("Capture registers when executed")) { std::string error; if (!probe.start(state.Pid, address, error)) state.Status = error; else { ui.SyncedAddress = address; ui.DisassemblyDirty = true; state.Status = "armed for next execution"; } }
                ImGui::EndDisabled();
            }
            ImGui::Separator();
            auto marker = ui.Markers.find(key);
            if (marker == ui.Markers.end())
            {
                if (ImGui::MenuItem("Add marker")) { ui.Markers.emplace(key, DisassemblyMarker{}); ui.DisassemblyDirty = true; }
            }
            else
            {
                ImGui::SeparatorText("Marker");
                if (ImGui::InputText("Tag", marker->second.Tag.data(), marker->second.Tag.size())) ui.DisassemblyDirty = true;
                if (ImGui::ColorEdit4("Color", &marker->second.Color.x, ImGuiColorEditFlags_AlphaBar)) { applyDisassemblyMarkers(state); ui.DisassemblyDirty = true; }
                if (ImGui::MenuItem("Remove marker")) { ui.Markers.erase(marker); ui.DisassemblyDirty = true; }
            }
        });
        state.Disassembly.Render("##memory-disassembly", ImVec2(-1.0f, 390.0f), ImGuiChildFlags_Borders);
        if (ui.SynchronizeAddress && state.Disassembly.IsMousePosOverTextArea(ImGui::GetMousePos()) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const auto pos = state.Disassembly.GetDocPosAtMousePos(ImGui::GetMousePos()); if (pos.line < ui.DisassemblyLines.size()) { ui.SyncedAddress = ui.DisassemblyLines[pos.line]; applyDisassemblyMarkers(state); }
        }
        if (ui.DisassemblyDirty) rebuildDisassembly(state);

        if (ImGui::BeginTabBar("MemoryInspectorLowerTabs"))
        {
            if (ImGui::BeginTabItem("Raw bytes"))
            {
                ImGui::TextDisabled("16 bytes per row, grouped 8 + 8. Click rows to synchronize; right-click for address/byte actions.");
                if (ImGui::BeginChild("##GroupedRawMemory", ImVec2(0.0f, 235.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
                {
                    const int lines = static_cast<int>((state.Original.size() + 15) / 16); ImGuiListClipper clipper; clipper.Begin(lines);
                    while (clipper.Step()) for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line)
                    {
                        const std::size_t offset = static_cast<std::size_t>(line) * 16; const std::size_t count = std::min<std::size_t>(16, state.Original.size() - offset); const std::uintptr_t address = state.Address + offset; const bool active = ui.SynchronizeAddress && ui.SyncedAddress >= address && ui.SyncedAddress < address + count; const std::string text = groupedBytesLine(state, offset, count); ImGui::PushID(line);
                        if (ImGui::Selectable("##raw-line", active, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeight())) && ui.SynchronizeAddress) { ui.SyncedAddress = address; applyDisassemblyMarkers(state); }
                        ImGui::SameLine(); ImGui::TextUnformatted(text.c_str());
                        if (ImGui::BeginPopupContextItem("RawLineContext"))
                        {
                            if (ImGui::MenuItem("Synchronize address here")) { ui.SyncedAddress = address; applyDisassemblyMarkers(state); }
                            if (ImGui::MenuItem("Use row as inspector base")) ui.PendingInspectorAddress = address;
                            if (ImGui::MenuItem("Copy address")) { const std::string value = runtimeHexAddress(address); ImGui::SetClipboardText(value.c_str()); }
                            if (ImGui::MenuItem("Copy 16-byte group")) { const std::string value = runtimeFormatHexBytes(std::span<const std::uint8_t>(state.Original).subspan(offset, count)); ImGui::SetClipboardText(value.c_str()); }
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild(); ImGui::EndTabItem();
            }
            const ImGuiTabItemFlags registerFlags = ui.SelectRegistersTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Registers", nullptr, registerFlags)) { ui.SelectRegistersTab = false; drawRegisterSnapshot(state); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Patch bytes")) { drawPatchBytes(state); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        if (ui.PendingInspectorAddress) { const std::uintptr_t address = *ui.PendingInspectorAddress; ui.PendingInspectorAddress.reset(); setInspectorAddress(state, address); }
    }
}
