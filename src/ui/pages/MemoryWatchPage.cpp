#include "quartz/client/ui/pages/MemoryWatchPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/AddressInput.hpp"
#include "quartz/client/ui/I18n.hpp"
#include "quartz/client/ui/ProcessPicker.hpp"
#include "quartz/client/ui/ReverseEngineeringNavigation.hpp"
#include "quartz/client/ui/SignatureMaker.hpp"
#include "quartz/client/native/ExecutionProbe.hpp"
#include "quartz/client/native/NativeDisassembly.hpp"

namespace quartz::client::ui
{
    namespace
    {
        struct RegisterValue { const char* Name; std::uint64_t Value; };
        std::uintptr_t hitSite(const MemoryWatchHit& hit) noexcept { return hit.InstructionAddress ? hit.InstructionAddress : hit.Rip; }

        void openWatchInspector(PageManager& manager, const pid_t pid, const std::uintptr_t address)
        {
            if (address == 0) return; requestMemoryInspector(pid, address); manager.open("native");
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
        (void)context;
        if (_processes.empty()) _processes = enumerateRuntimeProcesses();
        ImGui::TextWrapped("%s", i18n::tr("re.watchDescription"));
        drawProcessPicker("MemoryWatchProcess", _processes, _pid, _processSearch.data(), _processSearch.size(), 520.0f);
        drawAddressInput(i18n::tr("re.address"), _address.data(), _address.size(), _pid, 310.0f); ImGui::SameLine(); ImGui::SetNextItemWidth(110.0f); static constexpr const char* Sizes[] = {"1 byte", "2 bytes", "4 bytes", "8 bytes"}; ImGui::Combo(i18n::tr("re.size"), &_size, Sizes, 4); ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f);
        const char* accessPreview = _access == static_cast<int>(MemoryWatchAccess::ReadWrite) ? i18n::tr("re.readWriteAccess") : i18n::tr("re.writeAccess");
        if (ImGui::BeginCombo(i18n::tr("re.access"), accessPreview))
        {
            const bool writeSelected = _access == static_cast<int>(MemoryWatchAccess::Write); if (ImGui::Selectable(i18n::tr("re.writeAccess"), writeSelected)) _access = static_cast<int>(MemoryWatchAccess::Write); if (writeSelected) ImGui::SetItemDefaultFocus();
            const bool readWriteSelected = _access == static_cast<int>(MemoryWatchAccess::ReadWrite); if (ImGui::Selectable(i18n::tr("re.readWriteAccess"), readWriteSelected)) _access = static_cast<int>(MemoryWatchAccess::ReadWrite); if (readWriteSelected) ImGui::SetItemDefaultFocus();
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(120.0f); ImGui::InputInt(i18n::tr("re.hitLimit"), &_maxHits); ImGui::SameLine(); ImGui::TextDisabled(i18n::tr("re.watchAddressHint"), runtimeX86ModeName(runtimeProcessX86Mode(_pid)));
        const bool watchRunning = _watch.running();
        ImGui::BeginDisabled(watchRunning);
        if (ImGui::Button(i18n::tr("re.startWatch")))
        {
            std::uintptr_t address = 0; std::string error; static constexpr std::size_t Widths[] = {1, 2, 4, 8};
            if (!evaluateAddressExpression(_pid, _address.data(), address, error) || address == 0) _status = error.empty() ? i18n::tr("re.invalidAddress") : error;
            else { _selectedSite = 0; _watch.start(_pid, address, Widths[std::clamp(_size, 0, 3)], static_cast<MemoryWatchAccess>(_access), static_cast<std::size_t>(std::max(_maxHits, 0)), _status); }
        }
        ImGui::EndDisabled(); ImGui::SameLine(); ImGui::BeginDisabled(!watchRunning); if (ImGui::Button(i18n::tr("re.stopWatch"))) _watch.stop(); ImGui::EndDisabled(); ImGui::SameLine(); if (ImGui::Button(i18n::tr("re.clearHits"))) { _watch.clearHits(); _selectedSite = 0; }
        const std::string watchStatus = _watch.status(); if (!watchStatus.empty()) ImGui::TextWrapped("%s", watchStatus.c_str()); if (!_status.empty()) ImGui::TextDisabled("%s", _status.c_str());

        const auto hits = _watch.hits();
        if (ImGui::BeginTable("MemoryWatchHits", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0.0f, 350.0f)))
        {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 42.0f); ImGui::TableSetupColumn(i18n::tr("re.hits"), ImGuiTableColumnFlags_WidthFixed, 68.0f); ImGui::TableSetupColumn(i18n::tr("re.lastTid"), ImGuiTableColumnFlags_WidthFixed, 85.0f); ImGui::TableSetupColumn(i18n::tr("re.ripAfter"), ImGuiTableColumnFlags_WidthFixed, 175.0f); ImGui::TableSetupColumn(i18n::tr("re.accessInstruction")); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 72.0f); ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < hits.size(); ++i)
            {
                const auto& hit = hits[i]; const std::uintptr_t site = hitSite(hit); const bool active = _selectedSite == site; ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn();
                if (ImGui::Selectable("##WatchHitRow", active, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeight()))) _selectedSite = site;
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openWatchInspector(manager, _pid, site);
                if (ImGui::BeginPopupContextItem("WatchHitContext"))
                {
                    _selectedSite = site;
                    if (ImGui::MenuItem(i18n::tr("re.inspectAccessInstruction"))) openWatchInspector(manager, _pid, site);
                    if (ImGui::MenuItem(i18n::tr("re.signatureFromAccess"))) { requestSignatureMaker(_pid, site); manager.open("native"); }
                    if (ImGui::MenuItem(i18n::tr("re.inspectRipAfter"))) openWatchInspector(manager, _pid, hit.Rip);
                    ImGui::Separator();
                    auto& probe = executionProbe(); const bool thisProbe = probe.running() && probe.pid() == _pid && probe.address() == site;
                    if (thisProbe)
                    {
                        if (ImGui::MenuItem(i18n::tr("re.cancelProbe"))) { probe.stop(); _status = i18n::tr("re.executionProbeCancelled"); }
                    }
                    else
                    {
                        ImGui::BeginDisabled(probe.running());
                        if (ImGui::MenuItem(i18n::tr("re.captureRegistersNext")))
                        {
                            if (_watch.running()) _watch.stop(); std::string error;
                            if (!probe.start(_pid, site, error)) _status = error; else char message[256]{}; std::snprintf(message,sizeof(message),i18n::tr("re.armedProbeAt"),runtimeFormatProcessAddress(_pid,site).c_str()); _status=message;
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(i18n::tr("re.copyInstructionAddress"))) { const std::string text = runtimeFormatProcessAddress(_pid,site); ImGui::SetClipboardText(text.c_str()); }
                    if (ImGui::MenuItem(i18n::tr("re.copyRipAfter"))) { const std::string text = runtimeFormatProcessAddress(_pid,hit.Rip); ImGui::SetClipboardText(text.c_str()); }
                    if (ImGui::MenuItem(i18n::tr("re.copyInstruction"))) ImGui::SetClipboardText(hit.Instruction.c_str());
                    if (ImGui::MenuItem(i18n::tr("re.copyAddressInstruction"))) { const std::string text = runtimeFormatProcessAddress(_pid,site) + "  " + hit.Instruction; ImGui::SetClipboardText(text.c_str()); }
                    ImGui::EndPopup();
                }
                ImGui::SameLine(); ImGui::Text("%zu", i + 1); ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(hit.Count)); ImGui::TableNextColumn(); ImGui::Text("%d", hit.Tid); ImGui::TableNextColumn(); ImGui::TextUnformatted(runtimeFormatProcessAddress(_pid,hit.Rip).c_str()); ImGui::TableNextColumn(); if (hit.InstructionAddress) ImGui::Text("%s  %s", runtimeFormatProcessAddress(_pid,hit.InstructionAddress).c_str(), hit.Instruction.c_str()); else ImGui::TextUnformatted(hit.Instruction.c_str()); ImGui::TableNextColumn(); if (ImGui::SmallButton(i18n::tr("re.inspect"))) openWatchInspector(manager, _pid, site); ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("%s", i18n::tr("re.hitsDescription"));

        const MemoryWatchHit* selectedHit = nullptr; for (const auto& hit : hits) if (hitSite(hit) == _selectedSite) { selectedHit = &hit; break; }
        if (selectedHit && selectedHit->HasRegisters)
        {
            const RuntimeX86Mode mode = runtimeProcessX86Mode(_pid); const auto registers = registerValues(*selectedHit, mode); ImGui::SeparatorText(i18n::tr("re.registerSnapshot")); ImGui::TextDisabled(i18n::tr("re.latestHit"), runtimeFormatProcessAddress(_pid,hitSite(*selectedHit)).c_str(), selectedHit->Tid, mode == RuntimeX86Mode::X86 ? "32-bit" : "64-bit");
            if (ImGui::BeginTable("WatchRegisters", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 285.0f)))
            {
                ImGui::TableSetupColumn(i18n::tr("re.register"), ImGuiTableColumnFlags_WidthFixed, 90.0f); ImGui::TableSetupColumn(i18n::tr("re.value")); ImGui::TableSetupColumn(i18n::tr("re.register"), ImGuiTableColumnFlags_WidthFixed, 90.0f); ImGui::TableSetupColumn(i18n::tr("re.value")); ImGui::TableHeadersRow();
                for (std::size_t pair = 0; pair < (registers.size() + 1) / 2; ++pair)
                {
                    ImGui::TableNextRow();
                    for (std::size_t side = 0; side < 2; ++side)
                    {
                        const std::size_t index = pair + side * ((registers.size() + 1) / 2); ImGui::TableNextColumn();
                        if (index >= registers.size()) { ImGui::TableNextColumn(); continue; }
                        const auto& reg = registers[index]; ImGui::TextUnformatted(reg.Name); ImGui::TableNextColumn(); ImGui::PushID(static_cast<int>(index)); const std::string symbolic = runtimeFormatProcessAddress(_pid,static_cast<std::uintptr_t>(reg.Value));
                        ImGui::Selectable(symbolic.c_str(), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeight()));
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip(i18n::tr("re.registerValueTooltip"), reg.Name, symbolic.c_str(), static_cast<unsigned long long>(reg.Value), static_cast<long long>(reg.Value));
                        if (ImGui::BeginPopupContextItem("RegisterContext"))
                        {
                            if (ImGui::MenuItem(i18n::tr("re.copySymbolicAddress"))) ImGui::SetClipboardText(symbolic.c_str());
                            if (ImGui::MenuItem(i18n::tr("re.copyDecimal"))) { const std::string text = std::to_string(reg.Value); ImGui::SetClipboardText(text.c_str()); }
                            if (ImGui::MenuItem(i18n::tr("re.copyRegisterValue"))) { const std::string text = std::string(reg.Name) + " = " + symbolic; ImGui::SetClipboardText(text.c_str()); }
                            ImGui::Separator(); ImGui::BeginDisabled(reg.Value == 0);
                            if (ImGui::MenuItem(i18n::tr("re.inspectValueAddress"))) openWatchInspector(manager, _pid, static_cast<std::uintptr_t>(reg.Value));
                            if (ImGui::MenuItem(i18n::tr("re.useValueAsWatch"))) { std::snprintf(_address.data(), _address.size(), "0x%llX", static_cast<unsigned long long>(reg.Value)); char message[160]{}; std::snprintf(message,sizeof(message),i18n::tr("re.watchAddressFromRegister"),reg.Name); _status=message; }
                            ImGui::EndDisabled(); ImGui::EndPopup();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
        }
        else if (_selectedSite != 0) { ImGui::SeparatorText(i18n::tr("re.registerSnapshot")); ImGui::TextDisabled("%s",i18n::tr("re.noRegisterSnapshot")); }
    }
}
