from pathlib import Path
import re


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"{label}: anchor not found")
    return text.replace(old, new, 1)


def remove_range(text: str, start: str, end: str, label: str) -> str:
    begin = text.find(start)
    if begin < 0:
        raise RuntimeError(f"{label}: start not found")
    finish = text.find(end, begin)
    if finish < 0:
        raise RuntimeError(f"{label}: end not found")
    return text[:begin] + text[finish:]


# Signature maker: selected disassembly becomes the minimum signature seed, then we extend until unique.
path = Path("src/ui/SignatureMaker.cpp")
text = path.read_text()
text = replace_once(text, "            int MaxInstructions = 32;\n", "            int MinimumInstructions = 1;\n            int MaxInstructions = 32;\n", "signature state")
text = replace_once(text,
    "        void runSignatureMaker(SignatureMakerJob& job, const pid_t pid, const std::uintptr_t address, const int maxInstructions, const std::stop_token stop)\n",
    "        void runSignatureMaker(SignatureMakerJob& job, const pid_t pid, const std::uintptr_t address, const int minimumInstructions, const int maxInstructions, const std::stop_token stop)\n",
    "signature worker signature")
text = replace_once(text,
    "                if (signature.size() < 4) continue;\n                job.Progress.store(0.40f + static_cast<float>(instructionCount) / static_cast<float>(std::max(maxInstructions, 1)) * 0.58f, std::memory_order_relaxed);\n",
    "                if (signature.size() < 4 || instructionCount < static_cast<std::size_t>(minimumInstructions)) continue;\n                job.Progress.store(0.40f + static_cast<float>(instructionCount) / static_cast<float>(std::max(maxInstructions, 1)) * 0.58f, std::memory_order_relaxed);\n",
    "signature minimum range")
text = replace_once(text,
    "            const pid_t pid = ui.Pid; const int maxInstructions = ui.MaxInstructions;\n            job->Worker = std::jthread([job, pid, address, maxInstructions](const std::stop_token stop) { runSignatureMaker(*job, pid, address, maxInstructions, stop); });\n",
    "            const pid_t pid = ui.Pid; const int minimumInstructions = ui.MinimumInstructions; const int maxInstructions = std::max(ui.MaxInstructions, minimumInstructions);\n            job->Worker = std::jthread([job, pid, address, minimumInstructions, maxInstructions](const std::stop_token stop) { runSignatureMaker(*job, pid, address, minimumInstructions, maxInstructions, stop); });\n",
    "signature worker launch")
text = replace_once(text,
    "    void requestSignatureMaker(const pid_t pid, const std::uintptr_t address) noexcept\n    {\n        auto& ui = state(); ui.Pid = pid; ui.Address = address; ui.FocusRequested = true;\n        std::snprintf(ui.AddressText.data(), ui.AddressText.size(), \"0x%llX\", static_cast<unsigned long long>(address));\n    }\n",
    "    void requestSignatureMaker(const pid_t pid, const std::uintptr_t address, const int minimumInstructions) noexcept\n    {\n        auto& ui = state(); ui.Pid = pid; ui.Address = address; ui.FocusRequested = true; ui.MinimumInstructions = std::clamp(minimumInstructions, 1, 64); ui.MaxInstructions = std::max(ui.MaxInstructions, ui.MinimumInstructions);\n        std::snprintf(ui.AddressText.data(), ui.AddressText.size(), \"0x%llX\", static_cast<unsigned long long>(address));\n    }\n",
    "signature request")
text = replace_once(text,
    "        ImGui::SetNextItemWidth(150.0f); ImGui::SliderInt(\"Max instructions\", &ui.MaxInstructions, 4, 64);\n",
    "        ImGui::SetNextItemWidth(145.0f); ImGui::SliderInt(i18n::tr(\"re.signatureMinimumInstructions\"), &ui.MinimumInstructions, 1, 64); ImGui::SameLine();\n        ui.MaxInstructions = std::max(ui.MaxInstructions, ui.MinimumInstructions); ImGui::SetNextItemWidth(145.0f); ImGui::SliderInt(i18n::tr(\"re.signatureMaximumInstructions\"), &ui.MaxInstructions, ui.MinimumInstructions, 64);\n",
    "signature sliders")
text = replace_once(text,
    "        ImGui::TextWrapped(\"Build a signature from decoded instructions. RIP/EIP-relative displacements, relative branches/calls and encoded addresses are wildcarded automatically; libhat scans every readable executable mapping after each instruction until the target becomes unique.\");\n",
    "        ImGui::TextWrapped(\"%s\", i18n::tr(\"re.signatureDescription\"));\n",
    "signature description")
text = replace_once(text, '            ImGui::SeparatorText("Matches");\n', '            ImGui::SeparatorText(i18n::tr("re.signatureMatches"));\n', "signature matches heading")
text = replace_once(text, '                if (ImGui::SmallButton("Inspect")) { requestMemoryInspector(ui.Pid, matches[i]); manager.open("native"); }\n', '                if (ImGui::SmallButton(i18n::tr("re.inspect"))) { requestMemoryInspector(ui.Pid, matches[i]); manager.open("native"); }\n', "signature inspect")
path.write_text(text)


# Memory/disassembly: selected instructions can directly seed the signature maker.
path = Path("src/ui/MemoryInspector.cpp")
text = path.read_text()
include = '#include "quartz/client/ui/MemoryInspector.hpp"\n'
if '#include "quartz/client/ui/SignatureMaker.hpp"' not in text:
    text = replace_once(text, include, include + '#include "quartz/client/ui/SignatureMaker.hpp"\n', "memory inspector include")
anchor = '            if (ImGui::MenuItem("Copy instruction line")) { const std::string value = state.Disassembly.GetLineText(data.pos.line); ImGui::SetClipboardText(value.c_str()); }\n            ImGui::Separator();\n'
insert = '''            if (ImGui::MenuItem("Copy instruction line")) { const std::string value = state.Disassembly.GetLineText(data.pos.line); ImGui::SetClipboardText(value.c_str()); }
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
                    requestSignatureMaker(state.Pid, ui.DisassemblyLines[firstLine], static_cast<int>(lastLine - firstLine + 1));
                }
            }
            ImGui::Separator();
'''
text = replace_once(text, anchor, insert, "disassembly signature action")
path.write_text(text)


# Memory scanner: remove legacy binding creation and use shared navigation/signature actions.
path = Path("src/ui/pages/MemoryScannerPage.cpp")
text = path.read_text()
include = '#include "quartz/client/ui/PageManager.hpp"\n'
if '#include "quartz/client/ui/ReverseEngineeringNavigation.hpp"' not in text:
    text = replace_once(text, include, include + '#include "quartz/client/ui/ReverseEngineeringNavigation.hpp"\n#include "quartz/client/ui/SignatureMaker.hpp"\n', "memory scanner includes")
text = remove_range(text, "        std::optional<ProcessValueType> bindingValueType", "        void openInspector", "memory scanner binding helpers")
text = replace_once(text,
    '        void openInspector(PageManager& manager, const pid_t pid, const std::uintptr_t address)\n        {\n            auto& inspector = runtimeMemoryInspectorState(); inspector.Pid = pid; inspector.Address = address; runtimeRefreshMemoryInspector(inspector); manager.open("native");\n        }\n',
    '        void openInspector(PageManager& manager, const pid_t pid, const std::uintptr_t address)\n        {\n            requestMemoryInspector(pid, address); manager.open("native");\n        }\n',
    "memory scanner inspector navigation")
text = text.replace('        const auto bindType = bindingValueType(scanType);\n', '')
text, count = re.subn(r'\s*ImGui::BeginDisabled\(!bindType\.has_value\(\)\);\s*if \(ImGui::MenuItem\("Create binding"\).*?ImGui::EndDisabled\(\);', '', text, count=1, flags=re.S)
if count != 1:
    raise RuntimeError(f"memory scanner result binding action: removed {count}")
text, count = re.subn(r'\s*ImGui::SameLine\(\); ImGui::BeginDisabled\(!bindType\.has_value\(\)\); if \(ImGui::SmallButton\("Bind"\).*?ImGui::EndDisabled\(\);', '', text, count=1, flags=re.S)
if count != 1:
    raise RuntimeError(f"memory scanner bind button: removed {count}")
text, count = re.subn(r'\s*const auto watchBindType = bindingValueType\(watch\.Type\);.*?ImGui::EndDisabled\(\);', '', text, count=1, flags=re.S)
if count != 1:
    raise RuntimeError(f"memory scanner watch binding action: removed {count}")
text = replace_once(text,
    '                    if (ImGui::MenuItem("Disassemble")) openInspector(manager, _scanner.pid(), row.Address);\n',
    '                    if (ImGui::MenuItem("Disassemble")) openInspector(manager, _scanner.pid(), row.Address);\n                    if (ImGui::MenuItem("Create signature here")) { requestSignatureMaker(_scanner.pid(), row.Address); manager.open("native"); }\n',
    "scanner result signature")
text = replace_once(text,
    '                    if (ImGui::MenuItem("Disassemble")) openInspector(manager, watch.Pid, watch.Address);\n',
    '                    if (ImGui::MenuItem("Disassemble")) openInspector(manager, watch.Pid, watch.Address);\n                    if (ImGui::MenuItem("Create signature here")) { requestSignatureMaker(watch.Pid, watch.Address); manager.open("native"); }\n',
    "scanner watch signature")
text = text.replace('create a binding, or send it to the hardware access watcher.', 'send it to the signature maker, or open the hardware access watcher.')
path.write_text(text)


# Hardware memory watch: same connected navigation, no legacy binding creation.
path = Path("src/ui/pages/MemoryWatchPage.cpp")
text = path.read_text()
include = '#include "quartz/client/ui/PageManager.hpp"\n'
if '#include "quartz/client/ui/ReverseEngineeringNavigation.hpp"' not in text:
    text = replace_once(text, include, include + '#include "quartz/client/ui/ReverseEngineeringNavigation.hpp"\n#include "quartz/client/ui/SignatureMaker.hpp"\n', "memory watch includes")
text = remove_range(text, "        ProcessValueType watchBindingType", "        std::uintptr_t hitSite", "memory watch type helper")
text = replace_once(text,
    '        void openWatchInspector(PageManager& manager, const pid_t pid, const std::uintptr_t address)\n        {\n            if (address == 0) return; auto& inspector = runtimeMemoryInspectorState(); inspector.Pid = pid; inspector.Address = address; runtimeRefreshMemoryInspector(inspector); manager.open("native");\n        }\n',
    '        void openWatchInspector(PageManager& manager, const pid_t pid, const std::uintptr_t address)\n        {\n            if (address == 0) return; requestMemoryInspector(pid, address); manager.open("native");\n        }\n',
    "memory watch inspector navigation")
text = remove_range(text, "        void createWatchBinding", "        std::vector<RegisterValue> registerValues", "memory watch binding helper")
text, count = re.subn(r'\s*ImGui::SameLine\(\);\s*if \(ImGui::Button\("Bind watched value"\)\)\s*\{.*?\n        \}', '', text, count=1, flags=re.S)
if count != 1:
    raise RuntimeError(f"memory watch bind watched value: removed {count}")
text = re.sub(r'\s*if \(ImGui::MenuItem\("Create binding at access instruction"\)\).*?\n', '\n', text, count=1)
text = re.sub(r'\s*if \(ImGui::MenuItem\("Create binding at value"\)\).*?\n', '\n', text, count=1)
text = replace_once(text,
    '                    if (ImGui::MenuItem("Inspect access instruction")) openWatchInspector(manager, _pid, site);\n',
    '                    if (ImGui::MenuItem("Inspect access instruction")) openWatchInspector(manager, _pid, site);\n                    if (ImGui::MenuItem("Create signature from access instruction")) { requestSignatureMaker(_pid, site); manager.open("native"); }\n',
    "memory watch signature action")
text = replace_once(text, "    void MemoryWatchPage::render(PageContext& context, PageManager& manager)\n    {\n", "    void MemoryWatchPage::render(PageContext& context, PageManager& manager)\n    {\n        (void)context;\n", "memory watch unused context")
path.write_text(text)


# Shared RE helpers: navigate through the shared inspector, not direct state mutation.
path = Path("src/ui/ReverseEngineeringTools.cpp")
text = path.read_text()
include = '#include "quartz/client/ui/MemoryInspector.hpp"\n'
if '#include "quartz/client/ui/ReverseEngineeringNavigation.hpp"' not in text:
    text = replace_once(text, include, include + '#include "quartz/client/ui/ReverseEngineeringNavigation.hpp"\n#include "quartz/client/ui/SignatureMaker.hpp"\n', "RE tools includes")
text = replace_once(text,
    '        void openInspector(PageManager& manager, const pid_t pid, const std::uintptr_t address)\n        {\n            auto& inspector = runtimeMemoryInspectorState(); inspector.Pid = pid; inspector.Address = address; runtimeRefreshMemoryInspector(inspector); manager.open("native");\n        }\n',
    '        void openInspector(PageManager& manager, const pid_t pid, const std::uintptr_t address)\n        {\n            requestMemoryInspector(pid, address); manager.open("native");\n        }\n',
    "RE tools inspector navigation")
# The object-model debugger is no longer part of the new product navigation; remove its old binding action if the code remains compiled.
text = re.sub(r'\s*const bool bindable = processValueType\(watchType\)\.has_value\(\); ImGui::BeginDisabled\(!bindable\); if \(ImGui::MenuItem\("Create direct field binding"\).*?ImGui::EndDisabled\(\);', '', text, count=1, flags=re.S)
path.write_text(text)


# I18n catalog for the new connected RE workflow.
path = Path("src/ui/I18n.cpp")
text = path.read_text()
anchor = '            {"re.signatureStatus", "Status", "Status"}\n'
entries = '''            {"re.signatureStatus", "Status", "Status"},
            {"re.signatureDescription", "Build a signature from decoded instructions. Relocation-sensitive operands are wildcarded automatically and libhat extends the pattern until the target is unique.", "Gera uma assinatura a partir das instruções decodificadas. Operandos sensíveis a relocação recebem curingas automaticamente e o libhat estende o padrão até o alvo ser único."},
            {"re.signatureMinimumInstructions", "Minimum instructions", "Instruções mínimas"},
            {"re.signatureMaximumInstructions", "Maximum instructions", "Instruções máximas"},
            {"re.signatureMatches", "Matches", "Correspondências"},
            {"re.inspect", "Inspect", "Inspecionar"}
'''
text = replace_once(text, anchor, entries, "i18n RE entries")
path.write_text(text)

print("Reverse engineering refresh applied")
