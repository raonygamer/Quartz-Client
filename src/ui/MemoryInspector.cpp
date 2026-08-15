#include "quartz/client/ui/MemoryInspector.hpp"
#include "quartz/client/Model.hpp"
#include "quartz/client/native/NativeDisassembly.hpp"
#include "quartz/client/native/OpcodePatternEditor.hpp"

namespace quartz::client
{
    namespace
    {
        struct EnhancedMemoryInspectorState
        {
            pid_t LastPid = 0;
            std::uintptr_t LastAddress = 0;
            int LastReadSize = 0;
            std::uintptr_t AddressTextValue = std::numeric_limits<std::uintptr_t>::max();
            std::array<char, 32> AddressText{};
        };

        EnhancedMemoryInspectorState& enhancedMemoryInspectorState() { static EnhancedMemoryInspectorState state; return state; }

        bool parseInspectorAddress(const char* text, std::uintptr_t& address)
        {
            if (!text || !*text) return false;
            std::string_view value(text); int base = 10; bool negative = false;
            if (value.front() == '+') value.remove_prefix(1); else if (value.front() == '-') { negative = true; value.remove_prefix(1); }
            if (negative || value.empty()) return false;
            if (value.starts_with("0x") || value.starts_with("0X")) { value.remove_prefix(2); base = 16; }
            if (value.empty()) return false;
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), address, base); return ec == std::errc{} && ptr == value.data() + value.size();
        }

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

        void setInspectorAddress(RuntimeMemoryInspectorState& state, const std::uintptr_t address)
        {
            state.Address = address; enhancedMemoryInspectorState().AddressTextValue = std::numeric_limits<std::uintptr_t>::max(); syncInspectorAddressText(state); refreshEnhancedRuntimeMemoryInspector(state);
        }
    }

    void refreshEnhancedRuntimeMemoryInspector(RuntimeMemoryInspectorState& state)
    {
        auto& ui = enhancedMemoryInspectorState(); state.ReadSize = std::clamp(state.ReadSize, 1, 4096); state.Original.assign(static_cast<std::size_t>(state.ReadSize), 0); state.Patched.clear(); state.WriteConfirm = 0;
        if (state.Pid <= 0 || state.Address == 0) { state.Original.clear(); state.Disassembly.SetText({}); state.Status = "select a process and enter an address"; return; }
        std::string error;
        if (!readProcessMemoryBlock(state.Pid, state.Address, std::span<std::uint8_t>(state.Original.data(), state.Original.size()), error)) { state.Original.clear(); state.Disassembly.SetText({}); state.Status = "memory read failed: " + error; return; }
        state.Patched = state.Original; const std::string formatted = runtimeFormatHexBytes(state.Original); std::snprintf(state.HexEdit.data(), state.HexEdit.size(), "%s", formatted.c_str()); configureDisassemblyEditor(state);
        std::ostringstream disassembly; std::size_t offset = 0;
        while (offset < state.Original.size())
        {
            std::string text; std::size_t length = 0;
            if (runtimeDecodeProcessInstructionText(state.Pid, std::span<const std::uint8_t>(state.Original).subspan(offset), state.Address + offset, text, length) && length != 0) { disassembly << runtimeHexAddress(state.Address + offset) << "  " << text << '\n'; offset += length; }
            else { disassembly << runtimeHexAddress(state.Address + offset) << "  db 0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<unsigned>(state.Original[offset]) << std::dec << '\n'; ++offset; }
        }
        state.Disassembly.SetText(disassembly.str()); ui.LastPid = state.Pid; ui.LastAddress = state.Address; ui.LastReadSize = state.ReadSize; state.Status = "read " + std::to_string(state.Original.size()) + " bytes as " + runtimeX86ModeName(runtimeProcessX86Mode(state.Pid)); syncInspectorAddressText(state);
    }

    void drawEnhancedRuntimeMemoryInspector(RuntimeMemoryInspectorState& state)
    {
        auto& ui = enhancedMemoryInspectorState(); syncInspectorAddressText(state);
        if ((ui.LastPid != state.Pid || ui.LastAddress != state.Address || ui.LastReadSize != state.ReadSize) && state.Pid > 0 && state.Address != 0) refreshEnhancedRuntimeMemoryInspector(state);
        configureDisassemblyEditor(state);

        ImGui::SeparatorText("Memory / disassembly");
        int pidValue = static_cast<int>(state.Pid); ImGui::SetNextItemWidth(105.0f); if (ImGui::InputInt("PID##memory2", &pidValue)) { state.Pid = static_cast<pid_t>(std::max(pidValue, 0)); ui.LastPid = 0; } ImGui::SameLine();
        ImGui::SetNextItemWidth(190.0f); const bool addressEnter = ImGui::InputText("Address##memory2", ui.AddressText.data(), ui.AddressText.size(), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(105.0f); if (ImGui::InputInt("Bytes##memory2", &state.ReadSize)) state.ReadSize = std::clamp(state.ReadSize, 1, 4096); ImGui::SameLine();
        const bool readPressed = ImGui::Button("Read / disassemble");
        if (addressEnter || readPressed)
        {
            std::uintptr_t address = 0;
            if (!parseInspectorAddress(ui.AddressText.data(), address)) state.Status = "invalid memory address";
            else { state.Address = address; ui.AddressTextValue = address; refreshEnhancedRuntimeMemoryInspector(state); }
        }
        ImGui::SameLine(); ImGui::TextDisabled("%s", runtimeX86ModeName(runtimeProcessX86Mode(state.Pid)));

        const std::uintptr_t page = static_cast<std::uintptr_t>(std::max(state.ReadSize, 1));
        ImGui::BeginDisabled(state.Address < 0x100); if (ImGui::SmallButton("-0x100")) setInspectorAddress(state, state.Address - 0x100); ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::BeginDisabled(state.Address < page); if (ImGui::SmallButton("Previous block")) setInspectorAddress(state, state.Address - page); ImGui::EndDisabled(); ImGui::SameLine();
        if (ImGui::SmallButton("Next block")) setInspectorAddress(state, state.Address + page); ImGui::SameLine(); if (ImGui::SmallButton("+0x100")) setInspectorAddress(state, state.Address + 0x100); ImGui::SameLine();
        if (ImGui::SmallButton("Refresh")) refreshEnhancedRuntimeMemoryInspector(state);
        if (!state.Status.empty()) ImGui::TextDisabled("%s", state.Status.c_str());
        if (state.Original.empty()) return;

        ImGui::SeparatorText("Raw bytes"); ImGui::TextDisabled("16 bytes per row, grouped 8 + 8. Right-click a row for address/byte actions.");
        std::optional<std::uintptr_t> pendingAddress;
        if (ImGui::BeginChild("##GroupedRawMemory", ImVec2(0.0f, 235.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
        {
            const int lines = static_cast<int>((state.Original.size() + 15) / 16); ImGuiListClipper clipper; clipper.Begin(lines);
            while (clipper.Step()) for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line)
            {
                const std::size_t offset = static_cast<std::size_t>(line) * 16; const std::size_t count = std::min<std::size_t>(16, state.Original.size() - offset); const std::string text = groupedBytesLine(state, offset, count); ImGui::PushID(line);
                ImGui::Selectable("##raw-line", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeight())); ImGui::SameLine(); ImGui::TextUnformatted(text.c_str());
                if (ImGui::BeginPopupContextItem("RawLineContext"))
                {
                    const std::uintptr_t address = state.Address + offset;
                    if (ImGui::MenuItem("Use row as inspector address")) pendingAddress = address;
                    if (ImGui::MenuItem("Copy address")) { const std::string value = runtimeHexAddress(address); ImGui::SetClipboardText(value.c_str()); }
                    if (ImGui::MenuItem("Copy 16-byte group")) { const std::string value = runtimeFormatHexBytes(std::span<const std::uint8_t>(state.Original).subspan(offset, count)); ImGui::SetClipboardText(value.c_str()); }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild(); if (pendingAddress) setInspectorAddress(state, *pendingAddress);

        ImGui::SeparatorText("Disassembly"); ImGui::TextDisabled("Intel syntax. Mnemonics, registers, immediates and punctuation use the same assembly language definition as the opcode-pattern editor."); state.Disassembly.Render("##memory-disassembly", ImVec2(-1.0f, 330.0f), ImGuiChildFlags_Borders);

        ImGui::SeparatorText("Patch bytes"); ImGui::TextDisabled("Editable hexadecimal bytes for the current block. A write requires two clicks; Restore writes the exact block read above back to the process."); ImGui::InputTextMultiline("##hexPatch2", state.HexEdit.data(), state.HexEdit.size(), ImVec2(-1.0f, 92.0f));
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
