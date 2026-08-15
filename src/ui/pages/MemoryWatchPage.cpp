#include "quartz/client/ui/pages/MemoryWatchPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    namespace
    {
        bool parseWatchAddress(const char* text, std::uintptr_t& value)
        {
            if (!text || !*text) return false;
            std::string_view view(text); int base = 10;
            if (view.starts_with("0x") || view.starts_with("0X")) { view.remove_prefix(2); base = 16; }
            const auto [ptr, ec] = std::from_chars(view.data(), view.data() + view.size(), value, base); return ec == std::errc{} && ptr == view.data() + view.size();
        }

        ProcessValueType watchBindingType(const int size) noexcept
        {
            switch (std::clamp(size, 0, 3))
            {
            case 0: return ProcessValueType::U8;
            case 1: return ProcessValueType::U16;
            case 2: return ProcessValueType::U32;
            default: return ProcessValueType::U64;
            }
        }
    }

    void MemoryWatchPage::render(PageContext& context, PageManager& manager)
    {
        if (_processes.empty()) _processes = enumerateRuntimeProcesses();
        ImGui::TextWrapped("Hardware data breakpoints answer the classic 'who writes/reads this address?' question. x86 provides write-only or read/write data traps (not read-only) and only four debug-address slots per thread; Quartz uses one slot and mirrors it onto newly created threads.");
        if (ImGui::Button("Refresh processes")) _processes = enumerateRuntimeProcesses(); ImGui::SameLine(); ImGui::SetNextItemWidth(420.0f);
        const RuntimeProcessInfo* selected = nullptr; for (const auto& process : _processes) if (process.Pid == _pid) { selected = &process; break; }
        if (ImGui::BeginCombo("Process", selected ? runtimeProcessDisplayTitle(*selected).c_str() : "<select process>")) { for (const auto& process : _processes) { const bool active = process.Pid == _pid; const std::string label = runtimeProcessDisplayTitle(process) + "  [" + std::to_string(process.Pid) + "]"; if (ImGui::Selectable(label.c_str(), active)) _pid = process.Pid; } ImGui::EndCombo(); }
        ImGui::SetNextItemWidth(210.0f); ImGui::InputText("Address", _address.data(), _address.size());
        ImGui::SameLine(); ImGui::SetNextItemWidth(110.0f); static constexpr const char* Sizes[] = {"1 byte", "2 bytes", "4 bytes", "8 bytes"}; ImGui::Combo("Size", &_size, Sizes, 4);
        ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f); ImGui::Combo("Access", &_access, "Write\0Read / write\0");
        ImGui::SetNextItemWidth(120.0f); ImGui::InputInt("Hit limit", &_maxHits); ImGui::SameLine(); ImGui::TextDisabled("0 = unlimited");
        const bool watchRunning = _watch.running();
        ImGui::BeginDisabled(watchRunning);
        if (ImGui::Button("Start watch")) { std::uintptr_t address = 0; static constexpr std::size_t Widths[] = {1, 2, 4, 8}; if (!parseWatchAddress(_address.data(), address)) _status = "invalid address"; else _watch.start(_pid, address, Widths[std::clamp(_size, 0, 3)], static_cast<MemoryWatchAccess>(_access), static_cast<std::size_t>(std::max(_maxHits, 0)), _status); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!watchRunning);
        if (ImGui::Button("Stop")) _watch.stop();
        ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button("Clear hits")) _watch.clearHits();
        ImGui::SameLine();
        if (ImGui::Button("Bind watched value"))
        {
            std::uintptr_t address = 0;
            if (!parseWatchAddress(_address.data(), address)) _status = "invalid address";
            else if (_pid <= 0) _status = "select a process first";
            else
            {
                auto& binding = context.runtimeBindings.add();
                std::snprintf(binding.Name, sizeof(binding.Name), "Watch %llX", static_cast<unsigned long long>(address));
                binding.Source = RuntimeSourceKind::NativeProcess;
                binding.ProcessId = static_cast<int>(_pid);
                if (selected) std::snprintf(binding.ProcessName, sizeof(binding.ProcessName), "%s", selected->Name.c_str());
                binding.AddressMode = ProcessAddressMode::AddressChain;
                std::snprintf(binding.Address, sizeof(binding.Address), "0x%llX", static_cast<unsigned long long>(address));
                binding.ValueType = watchBindingType(_size);
                binding.WriteMaterial = false;
                binding.Normalize = false;
                binding.Clamp = false;
                binding.SmoothingHz = 0.0f;
                binding.UpdateHz = 30.0f;
                binding.AutoReattach = false;
                _status = "created binding for watched value";
            }
        }
        const std::string watchStatus = _watch.status(); if (!watchStatus.empty()) ImGui::TextWrapped("%s", watchStatus.c_str()); if (!_status.empty()) ImGui::TextDisabled("%s", _status.c_str());
        const auto hits = _watch.hits();
        if (ImGui::BeginTable("MemoryWatchHits", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0.0f, 360.0f)))
        {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 42.0f); ImGui::TableSetupColumn("TID", ImGuiTableColumnFlags_WidthFixed, 75.0f); ImGui::TableSetupColumn("RIP after access", ImGuiTableColumnFlags_WidthFixed, 145.0f); ImGui::TableSetupColumn("Best-effort previous instruction"); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 62.0f); ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < hits.size(); ++i) { const auto& hit = hits[i]; ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%zu", i + 1); ImGui::TableNextColumn(); ImGui::Text("%d", hit.Tid); ImGui::TableNextColumn(); ImGui::Text("0x%llX", static_cast<unsigned long long>(hit.Rip)); ImGui::TableNextColumn(); if (hit.InstructionAddress) ImGui::Text("0x%llX  %s", static_cast<unsigned long long>(hit.InstructionAddress), hit.Instruction.c_str()); else ImGui::TextUnformatted(hit.Instruction.c_str()); ImGui::TableNextColumn(); if (ImGui::SmallButton("Inspect")) { auto& inspector = runtimeMemoryInspectorState(); inspector.Pid = _pid; inspector.Address = hit.InstructionAddress ? hit.InstructionAddress : hit.Rip; runtimeRefreshMemoryInspector(inspector); manager.open("native"); } ImGui::PopID(); }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Data breakpoints trap after the memory access. Quartz backtracks up to 15 bytes and shows a best-effort preceding instruction; RIP itself is the post-access instruction pointer.");
    }
}
