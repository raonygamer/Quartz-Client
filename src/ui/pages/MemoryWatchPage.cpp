#include "quartz/client/ui/pages/MemoryWatchPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/native/ExecutionProbe.hpp"
#include "quartz/client/native/NativeDisassembly.hpp"

namespace quartz::client::ui
{
    namespace
    {
        struct RegisterValue { const char* Name; std::uint64_t Value; };

        bool parseWatchAddress(const char* text, std::uintptr_t& value)
        {
            if (!text || !*text) return false;
            std::string_view view(text); int base = 10;
            if (view.starts_with("0x") || view.starts_with("0X")) { view.remove_prefix(2); base = 16; }
            const auto [ptr, ec] = std::from_chars(view.data(), view.data() + view.size(), value, base); return ec == std::errc{} && ptr == view.data() + view.size();
        }

        ProcessValueType watchBindingType(const int size) noexcept
        {
            switch (std::clamp(size, 0, 3)) { case 0: return ProcessValueType::U8; case 1: return ProcessValueType::U16; case 2: return ProcessValueType::U32; default: return ProcessValueType::U64; }
        }

        std::uintptr_t hitSite(const MemoryWatchHit& hit) noexcept { return hit.InstructionAddress ? hit.InstructionAddress : hit.Rip; }

        void openWatchInspector(PageManager& manager, const pid_t pid, const std::uintptr_t address)
        {
            if (address == 0) return; auto& inspector = runtimeMemoryInspectorState(); inspector.Pid = pid; inspector.Address = address; runtimeRefreshMemoryInspector(inspector); manager.open("native");
        }

        void createWatchBinding(RuntimeBindingEngine& engine, const pid_t pid, const RuntimeProcessInfo* process, const std::uintptr_t address, const ProcessValueType type, const char* prefix)
        {
            auto& binding = engine.add(); std::snprintf(binding.Name, sizeof(binding.Name), "%s %llX", prefix, static_cast<unsigned long long>(address)); binding.Source = RuntimeSourceKind::NativeProcess; binding.ProcessId = static_cast<int>(pid); if (process) std::snprintf(binding.ProcessName, sizeof(binding.ProcessName), "%s", process->Name.c_str()); binding.AddressMode = ProcessAddressMode::AddressChain; std::snprintf(binding.Address, sizeof(binding.Address), "0x%llX", static_cast<unsigned long long>(address)); binding.ValueType = type; binding.WriteMaterial = false; binding.Normalize = false; binding.Clamp = false; binding.SmoothingHz = 0.0f; binding.UpdateHz = 30.0f; binding.AutoReattach = false;
        }

        std::vector<RegisterValue> registerValues(const MemoryWatchHit& hit, const RuntimeX86Mode mode)
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
    }

    void MemoryWatchPage::render(PageContext& context, PageManager& manager)
    {
        if (_processes.empty()) _processes = enumerateRuntimeProcesses();
        ImGui::TextWrapped("Hardware data breakpoints answer the classic 'who writes/reads this address?' question. x86 provides write-only or read/write data traps (not read-only) and only four debug-address slots per thread; Quartz uses one slot and mirrors it onto newly created threads. Click a hit to inspect its latest register snapshot; right-click hits and register values for actions.");
        if (ImGui::Button("Refresh processes")) _processes = enumerateRuntimeProcesses(); ImGui::SameLine(); ImGui::SetNextItemWidth(420.0f);
        const RuntimeProcessInfo* selected = nullptr; for (const auto& process : _processes) if (process.Pid == _pid) { selected = &process; break; }
        if (ImGui::BeginCombo("Process", selected ? runtimeProcessDisplayTitle(*selected).c_str() : "<select process>")) { for (const auto& process : _processes) { const bool active = process.Pid == _pid; const std::string label = runtimeProcessDisplayTitle(process) + "  [" + std::to_string(process.Pid) + "]"; if (ImGui::Selectable(label.c_str(), active)) { _pid = process.Pid; _selectedSite = 0; } if (active) ImGui::SetItemDefaultFocus(); } ImGui::EndCombo(); }
        ImGui::SetNextItemWidth(210.0f); ImGui::InputText("Address", _address.data(), _address.size()); ImGui::SameLine(); ImGui::SetNextItemWidth(110.0f); static constexpr const char* Sizes[] = {"1 byte", "2 bytes", "4 bytes", "8 bytes"}; ImGui::Combo("Size", &_size, Sizes, 4); ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f); ImGui::Combo("Access", &_access, "Write\0Read / write\0");
        ImGui::SetNextItemWidth(120.0f); ImGui::InputInt("Hit limit", &_maxHits); ImGui::SameLine(); ImGui::TextDisabled("0 = unlimited | address accepts decimal or 0x-prefixed hex | %s", runtimeX86ModeName(runtimeProcessX86Mode(_pid)));
        const bool watchRunning = _watch.running();
        ImGui::BeginDisabled(watchRunning);
        if (ImGui::Button("Start watch")) { std::uintptr_t address = 0; static constexpr std::size_t Widths[] = {1, 2, 4, 8}; if (!parseWatchAddress(_address.data(), address)) _status = "invalid address"; else { _selectedSite = 0; _watch.start(_pid, address, Widths[std::clamp(_size, 0, 3)], static_cast<MemoryWatchAccess>(_access), static_cast<std::size_t>(std::max(_maxHits, 0)), _status); } }
        ImGui::EndDisabled(); ImGui::SameLine(); ImGui::BeginDisabled(!watchRunning); if (ImGui::Button("Stop")) _watch.stop(); ImGui::EndDisabled(); ImGui::SameLine(); if (ImGui::Button("Clear hits")) { _watch.clearHits(); _selectedSite = 0; }
        ImGui::SameLine();
        if (ImGui::Button("Bind watched value"))
        {
            std::uintptr_t address = 0; if (!parseWatchAddress(_address.data(), address)) _status = "invalid address"; else if (_pid <= 0) _status = "select a process first"; else { createWatchBinding(context.runtimeBindings, _pid, selected, address, watchBindingType(_size), "Watch"); _status = "created binding for watched value"; }
        }
        const std::string watchStatus = _watch.status(); if (!watchStatus.empty()) ImGui::TextWrapped("%s", watchStatus.c_str()); if (!_status.empty()) ImGui::TextDisabled("%s", _status.c_str());

        const auto hits = _watch.hits();
        if (ImGui::BeginTable("MemoryWatchHits", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0.0f, 350.0f)))
        {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 42.0f); ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, 68.0f); ImGui::TableSetupColumn("Last TID", ImGuiTableColumnFlags_WidthFixed, 85.0f); ImGui::TableSetupColumn("RIP after access", ImGuiTableColumnFlags_WidthFixed, 145.0f); ImGui::TableSetupColumn("Access instruction"); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 62.0f); ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < hits.size(); ++i)
            {
                const auto& hit = hits[i]; const std::uintptr_t site = hitSite(hit); const bool active = _selectedSite == site; ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn();
                if (ImGui::Selectable("##WatchHitRow", active, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeight()))) _selectedSite = site;
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openWatchInspector(manager, _pid, site);
                if (ImGui::BeginPopupContextItem("WatchHitContext"))
                {
                    _selectedSite = site;
                    if (ImGui::MenuItem("Inspect access instruction")) openWatchInspector(manager, _pid, site);
                    if (ImGui::MenuItem("Inspect RIP after access")) openWatchInspector(manager, _pid, hit.Rip);
                    ImGui::Separator();
                    auto& probe = executionProbe(); const bool thisProbe = probe.running() && probe.pid() == _pid && probe.address() == site;
                    if (thisProbe)
                    {
                        if (ImGui::MenuItem("Cancel execution probe")) { probe.stop(); _status = "execution probe cancelled"; }
                    }
                    else
                    {
                        ImGui::BeginDisabled(probe.running());
                        if (ImGui::MenuItem("Capture registers on next execution"))
                        {
                            if (_watch.running()) _watch.stop(); std::string error;
                            if (!probe.start(_pid, site, error)) _status = error; else _status = "armed one-shot execution probe at " + runtimeHexAddress(site);
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Copy instruction address")) { const std::string text = runtimeHexAddress(site); ImGui::SetClipboardText(text.c_str()); }
                    if (ImGui::MenuItem("Copy RIP after access")) { const std::string text = runtimeHexAddress(hit.Rip); ImGui::SetClipboardText(text.c_str()); }
                    if (ImGui::MenuItem("Copy instruction")) ImGui::SetClipboardText(hit.Instruction.c_str());
                    if (ImGui::MenuItem("Copy address + instruction")) { const std::string text = runtimeHexAddress(site) + "  " + hit.Instruction; ImGui::SetClipboardText(text.c_str()); }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Create binding at access instruction")) { createWatchBinding(context.runtimeBindings, _pid, selected, site, ProcessValueType::U8, "Code"); _status = "created binding at access instruction"; }
                    ImGui::EndPopup();
                }
                ImGui::SameLine(); ImGui::Text("%zu", i + 1); ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(hit.Count)); ImGui::TableNextColumn(); ImGui::Text("%d", hit.Tid); ImGui::TableNextColumn(); ImGui::Text("0x%llX", static_cast<unsigned long long>(hit.Rip)); ImGui::TableNextColumn(); if (hit.InstructionAddress) ImGui::Text("0x%llX  %s", static_cast<unsigned long long>(hit.InstructionAddress), hit.Instruction.c_str()); else ImGui::TextUnformatted(hit.Instruction.c_str()); ImGui::TableNextColumn(); if (ImGui::SmallButton("Inspect")) openWatchInspector(manager, _pid, site); ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Hits are grouped by the instruction that accessed the watched address. The register snapshot below is replaced with the latest trap at that site; RIP is post-access because x86 data breakpoints trap after the memory access.");

        const MemoryWatchHit* selectedHit = nullptr; for (const auto& hit : hits) if (hitSite(hit) == _selectedSite) { selectedHit = &hit; break; }
        if (selectedHit && selectedHit->HasRegisters)
        {
            const RuntimeX86Mode mode = runtimeProcessX86Mode(_pid); const auto registers = registerValues(*selectedHit, mode); ImGui::SeparatorText("Register snapshot"); ImGui::TextDisabled("Latest hit at %s | TID %d | %s | right-click any register value", runtimeHexAddress(hitSite(*selectedHit)).c_str(), selectedHit->Tid, mode == RuntimeX86Mode::X86 ? "32-bit register view" : "64-bit register view");
            if (ImGui::BeginTable("WatchRegisters", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 285.0f)))
            {
                ImGui::TableSetupColumn("Register", ImGuiTableColumnFlags_WidthFixed, 90.0f); ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("Register", ImGuiTableColumnFlags_WidthFixed, 90.0f); ImGui::TableSetupColumn("Value"); ImGui::TableHeadersRow();
                for (std::size_t pair = 0; pair < (registers.size() + 1) / 2; ++pair)
                {
                    ImGui::TableNextRow();
                    for (std::size_t side = 0; side < 2; ++side)
                    {
                        const std::size_t index = pair + side * ((registers.size() + 1) / 2); ImGui::TableNextColumn();
                        if (index >= registers.size()) { ImGui::TableNextColumn(); continue; }
                        const auto& reg = registers[index]; ImGui::TextUnformatted(reg.Name); ImGui::TableNextColumn(); ImGui::PushID(static_cast<int>(index)); const std::string hex = runtimeHexAddress(static_cast<std::uintptr_t>(reg.Value));
                        ImGui::Selectable(hex.c_str(), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeight()));
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s = %s\nunsigned: %llu\nsigned: %lld", reg.Name, hex.c_str(), static_cast<unsigned long long>(reg.Value), static_cast<long long>(reg.Value));
                        if (ImGui::BeginPopupContextItem("RegisterContext"))
                        {
                            if (ImGui::MenuItem("Copy hexadecimal")) ImGui::SetClipboardText(hex.c_str());
                            if (ImGui::MenuItem("Copy decimal")) { const std::string text = std::to_string(reg.Value); ImGui::SetClipboardText(text.c_str()); }
                            if (ImGui::MenuItem("Copy register = value")) { const std::string text = std::string(reg.Name) + " = " + hex; ImGui::SetClipboardText(text.c_str()); }
                            ImGui::Separator(); ImGui::BeginDisabled(reg.Value == 0);
                            if (ImGui::MenuItem("Inspect/disassemble value as address")) openWatchInspector(manager, _pid, static_cast<std::uintptr_t>(reg.Value));
                            if (ImGui::MenuItem("Use value as watch address")) { std::snprintf(_address.data(), _address.size(), "0x%llX", static_cast<unsigned long long>(reg.Value)); _status = std::string("watch address field <- ") + reg.Name; }
                            if (ImGui::MenuItem("Create binding at value")) { createWatchBinding(context.runtimeBindings, _pid, selected, static_cast<std::uintptr_t>(reg.Value), watchBindingType(_size), reg.Name); _status = std::string("created binding from ") + reg.Name; }
                            ImGui::EndDisabled(); ImGui::EndPopup();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
        }
        else if (_selectedSite != 0) { ImGui::SeparatorText("Register snapshot"); ImGui::TextDisabled("No register snapshot is available for the selected hit."); }
    }
}
