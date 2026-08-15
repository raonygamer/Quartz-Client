
    static const char* runtimeControlConditionName(const RuntimeControlCondition condition)
    {
        static constexpr const char* Names[] = {"==", "!=", "<", "<=", ">", ">=", "between", "outside", "rising edge", "falling edge", "on change", "changed to", "changed from", "becomes true", "becomes false", "string ==", "string !=", "string contains"};
        return Names[std::clamp(static_cast<int>(condition), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    static const char* runtimeControlTargetName(const RuntimeControlTarget target)
    {
        static constexpr const char* Names[] = {"Active shader", "Binding enabled", "Global brightness", "Send framebuffer", "Base color mode", "Material parameter", "Unbound binding value", "Value bank", "Control enabled", "Refresh binding", "Force binding update", "Invalidate binding", "Reset binding state", "Retry register capture", "Rescan binding pattern", "Rebind process", "Clear binding error"};
        return Names[std::clamp(static_cast<int>(target), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    static bool drawRuntimeControlMaterialTarget(RuntimeControlRule& control, ShaderFramebuffer& shaderFramebuffer)
    {
        bool changed = false;
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::BeginCombo("Uniform id##control", control.TargetId[0] ? control.TargetId : "<select material uniform>"))
        {
            for (const auto& parameter : shaderFramebuffer.materialParameters())
            {
                const bool selected = parameter.PersistenceKey == control.TargetId;
                const std::string label = parameter.Label + "  [" + parameter.PersistenceKey + "]";
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    std::snprintf(control.TargetId, sizeof(control.TargetId), "%s", parameter.PersistenceKey.c_str());
                    control.TargetComponent = std::clamp(control.TargetComponent, 0, std::max(parameter.Components - 1, 0));
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (const auto* parameter = shaderFramebuffer.findMaterialParameter(control.TargetId); parameter && parameter->Components > 1)
        {
            ImGui::SameLine();
            changed |= ImGui::SliderInt("Component##control", &control.TargetComponent, 0, parameter->Components - 1);
        }
        return changed;
    }

    static bool drawRuntimeControlRule(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer, RuntimeControlRule& control, bool& erase)
    {
        bool changed = false;
        ImGui::PushID("RuntimeControl");
        ImGui::PushID(static_cast<int>(control.Id & 0x7fffffffULL));
        const RuntimeBinding* source = engine.findBinding(control.SourceBindingId);
        const bool ready = source && source->Enabled && source->RuntimeEnabled && source->HasValue;
        const bool runtimeEnabled = control.Enabled && control.RuntimeEnabled;
        const bool on = control.ConditionActive || control.TriggeredThisFrame;
        const ImVec4 stateColor = runtimeStateColor(runtimeEnabled, on, false, runtimeEnabled && !ready);
        drawRuntimeStateSquare("##controlState", stateColor);
        ImGui::SameLine();
        std::string header = std::string(control.Name[0] ? control.Name : "Control") + "  [p" + std::to_string(control.Priority) + "]  ";
        if (!control.Enabled) header += "DISABLED";
        else if (!control.RuntimeEnabled) header += "RUNTIME OFF";
        else if (!ready) header += "WAITING";
        else if (control.TriggeredThisFrame) header += "TRIGGERED";
        else header += control.ConditionActive ? "TRUE" : "FALSE";
        header += "###RuntimeControl" + std::to_string(control.Id);
        if (!ImGui::CollapsingHeader(header.c_str())) { ImGui::PopID(); ImGui::PopID(); return false; }
        ImGui::Indent(10.0f);
        ImGui::TextColored(stateColor, "%s", !control.Enabled ? "Disabled" : !control.RuntimeEnabled ? "Runtime disabled by control" : !ready ? "Waiting for source" : control.TriggeredThisFrame ? "Triggered this frame" : control.ConditionActive ? "Condition true" : "Condition false");
        if (source) { ImGui::SameLine(); ImGui::TextDisabled("source %.6g%s", source->Value, source->HasString ? " + string" : ""); }

        changed |= ImGui::Checkbox("Enabled", &control.Enabled);
        ImGui::SameLine(); if (ImGui::SmallButton("Remove")) erase = true;
        ImGui::SameLine(); ImGui::SetNextItemWidth(220.0f); changed |= ImGui::InputText("Name", control.Name, sizeof(control.Name));
        ImGui::SameLine(); ImGui::SetNextItemWidth(110.0f); changed |= ImGui::InputInt("Priority", &control.Priority);
        ImGui::SetNextItemWidth(160.0f); changed |= ImGui::InputText("Group", control.Group, sizeof(control.Group)); ImGui::SameLine(); ImGui::SetNextItemWidth(80.0f); changed |= ImGui::InputInt("Order", &control.Order); ImGui::SameLine(); if (ImGui::SmallButton("Up##controlOrder")) { --control.Order; changed = true; } ImGui::SameLine(); if (ImGui::SmallButton("Down##controlOrder")) { ++control.Order; changed = true; }

        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::BeginCombo("Source binding", source ? source->Name : "<select binding>"))
        {
            for (const auto& candidate : engine.bindings()) { const bool selected = candidate.Id == control.SourceBindingId; if (ImGui::Selectable(candidate.Name, selected)) { control.SourceBindingId = candidate.Id; changed = true; } if (selected) ImGui::SetItemDefaultFocus(); }
            ImGui::EndCombo();
        }

        int condition = static_cast<int>(control.Condition);
        ImGui::SetNextItemWidth(175.0f);
        if (ImGui::Combo("Condition", &condition, "==\0!=\0<\0<=\0>\0>=\0between\0outside\0rising edge\0falling edge\0on change\0changed to\0changed from\0becomes true\0becomes false\0string ==\0string !=\0string contains\0")) { control.Condition = static_cast<RuntimeControlCondition>(condition); changed = true; }
        const bool stringCondition = control.Condition == RuntimeControlCondition::StringEqual || control.Condition == RuntimeControlCondition::StringNotEqual || control.Condition == RuntimeControlCondition::StringContains;
        const bool eventCondition = control.Condition == RuntimeControlCondition::RisingEdge || control.Condition == RuntimeControlCondition::FallingEdge || control.Condition == RuntimeControlCondition::OnChange || control.Condition == RuntimeControlCondition::ChangedTo || control.Condition == RuntimeControlCondition::ChangedFrom || control.Condition == RuntimeControlCondition::BecomesTrue || control.Condition == RuntimeControlCondition::BecomesFalse;
        const bool needsValue = control.Condition <= RuntimeControlCondition::FallingEdge || control.Condition == RuntimeControlCondition::ChangedTo || control.Condition == RuntimeControlCondition::ChangedFrom;
        if (stringCondition) changed |= ImGui::InputText("Text target", control.StringCompare, sizeof(control.StringCompare));
        else if (needsValue)
        {
            const bool twoValues = control.Condition == RuntimeControlCondition::Between || control.Condition == RuntimeControlCondition::Outside;
            ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat(twoValues ? "Minimum" : "Target / threshold", &control.ValueA, 0.01f);
            if (twoValues) { ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Maximum", &control.ValueB, 0.01f); }
            if (control.Condition == RuntimeControlCondition::Equal || control.Condition == RuntimeControlCondition::NotEqual || control.Condition == RuntimeControlCondition::ChangedTo || control.Condition == RuntimeControlCondition::ChangedFrom)
            { ImGui::SameLine(); ImGui::SetNextItemWidth(135.0f); changed |= ImGui::DragFloat("Tolerance", &control.Tolerance, 0.0001f, 0.000001f, 1000.0f, "%.6f"); }
        }
        if (eventCondition) { changed |= ImGui::Checkbox("Fire on first matching sample", &control.FireOnFirstSample); ImGui::SameLine(); ImGui::TextDisabled("events are one-shot; iterative passes are capped"); }
        else if (!stringCondition) { ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Hysteresis", &control.Hysteresis, 0.001f, 0.0f, 1000.0f, "%.4f"); }

        if (ImGui::BeginTabBar("ControlOptions", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyResizeDown))
        {
            if (ImGui::BeginTabItem("Primary action"))
            {
                ImGui::TextDisabled("Compatibility action for existing configs. Extra actions below can run alongside it.");
                int target = static_cast<int>(control.Target); ImGui::SetNextItemWidth(220.0f);
                if (ImGui::Combo("Target", &target, "Active shader\0Binding enabled\0Global brightness\0Send framebuffer\0Base color mode\0Material parameter\0Unbound binding value\0Value bank\0Control enabled\0Refresh binding\0Force binding update\0Invalidate binding\0Reset binding state\0Retry register capture\0Rescan binding pattern\0Rebind process\0Clear binding error\0")) { control.Target = static_cast<RuntimeControlTarget>(target); changed = true; }
                if (control.Target == RuntimeControlTarget::ActiveShader)
                {
                    const ShaderPreset* shaderById = control.ShaderId[0] ? findShaderPresetById(control.ShaderId) : nullptr;
                    const char* preview = control.ShaderPresetIndex == -1 ? "<previous shader>" : shaderById ? shaderById->Name.c_str() : control.ShaderPresetIndex > 0 && control.ShaderPresetIndex <= static_cast<int>(ShaderPresets.size()) ? ShaderPresets[static_cast<std::size_t>(control.ShaderPresetIndex - 1)].Name.c_str() : "<select shader>";
                    ImGui::SetNextItemWidth(300.0f);
                    if (ImGui::BeginCombo("Shader", preview))
                    {
                        if (ImGui::Selectable("<previous shader>", control.ShaderPresetIndex == -1)) { control.ShaderPresetIndex = -1; control.ShaderId[0] = '\0'; changed = true; }
                        for (std::size_t i = 0; i < ShaderPresets.size(); ++i) { const bool selected = control.ShaderPresetIndex == static_cast<int>(i + 1); if (ImGui::Selectable(ShaderPresets[i].Name.c_str(), selected)) { control.ShaderPresetIndex = static_cast<int>(i + 1); std::snprintf(control.ShaderId, sizeof(control.ShaderId), "%s", ShaderPresets[i].Id.c_str()); changed = true; } }
                        ImGui::EndCombo();
                    }
                    ImGui::SetNextItemWidth(160.0f); changed |= ImGui::DragFloat("Crossfade", &control.TransitionSeconds, 0.02f, 0.0f, 10.0f, "%.2f s");
                }
                else if (control.Target == RuntimeControlTarget::BindingEnabled) { changed |= drawRuntimeBindingReferenceCombo(engine, "Target binding", control.TargetBindingId, control.SourceBindingId); changed |= ImGui::Checkbox("Runtime enabled", &control.TargetBool); }
                else if (control.Target == RuntimeControlTarget::GlobalBrightness) changed |= ImGui::SliderFloat("Brightness", &control.TargetValue, 0.0f, 1.0f, "%.3f");
                else if (control.Target == RuntimeControlTarget::SendFramebuffer) changed |= ImGui::Checkbox("Send framebuffer", &control.TargetBool);
                else if (control.Target == RuntimeControlTarget::BaseColorMode) { int mode = std::clamp(static_cast<int>(std::lround(control.TargetValue)), 0, 2); if (ImGui::Combo("Base color mode", &mode, "RGB wave\0Solid\0Shader\0")) { control.TargetValue = static_cast<float>(mode); changed = true; } }
                else if (control.Target == RuntimeControlTarget::MaterialParameter) { changed |= drawRuntimeControlMaterialTarget(control, shaderFramebuffer); ImGui::SetNextItemWidth(180.0f); changed |= ImGui::DragFloat("Target value", &control.TargetValue, 0.01f); }
                else if (control.Target == RuntimeControlTarget::BindingValue) { changed |= drawRuntimeBindingReferenceCombo(engine, "Unbound target", control.TargetBindingId, control.SourceBindingId, true); changed |= ImGui::Checkbox("Use source value", &control.TargetUseSourceValue); if (!control.TargetUseSourceValue) { ImGui::SetNextItemWidth(180.0f); changed |= ImGui::DragFloat("Written value", &control.TargetValue, 0.01f); } }
                else if (control.Target == RuntimeControlTarget::ValueBank) { changed |= drawRuntimeBankReferenceCombo(engine, "Bank target", control.TargetBankValueId); changed |= ImGui::Checkbox("Use source value", &control.TargetUseSourceValue); if (!control.TargetUseSourceValue) { ImGui::SetNextItemWidth(180.0f); changed |= ImGui::DragFloat("Written value", &control.TargetValue, 0.01f); } }
                else if (control.Target == RuntimeControlTarget::ControlEnabled) { changed |= drawRuntimeControlReferenceCombo(engine, "Target control", control.TargetControlId); changed |= ImGui::Checkbox("Runtime enabled", &control.TargetBool); }
                else if (runtimeControlTargetIsBindingOperation(control.Target)) { changed |= drawRuntimeBindingReferenceCombo(engine, "Target binding", control.TargetBindingId, control.SourceBindingId); ImGui::TextDisabled("Primary binding operations fire when the condition enters true (or an event fires). Use an additional action with While active for repeated operations."); }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Additional actions"))
            {
                ImGui::TextDisabled("Actions execute in priority order. On-trigger actions are ideal for saving/restoring shader state through the value bank.");
                changed |= drawRuntimeActionList(engine, shaderFramebuffer, control.Actions, control.SourceBindingId, true);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::TextDisabled("Triggers: %llu   %s", static_cast<unsigned long long>(control.TriggerCount), control.LastTriggerTime > 0.0 ? "has fired" : "never fired");
        if (changed) engine.markChanged();
        ImGui::Unindent(10.0f);
        ImGui::Dummy(ImVec2(0.0f, 5.0f));
        ImGui::Separator();
        ImGui::PopID(); ImGui::PopID();
        return changed;
    }

    template<typename T>
    static void runtimeSortUiNodes(std::vector<T*>& nodes)
    {
        std::ranges::sort(nodes, [](const T* a, const T* b) { if (a->Order != b->Order) return a->Order < b->Order; return a->Id < b->Id; });
    }

    static bool runtimeWriteProcessMemory(const pid_t pid, const std::uintptr_t address, const std::span<const std::uint8_t> bytes, std::string& error)
    {
        if (bytes.empty()) { error = "nothing to write"; return false; }
        iovec local{const_cast<std::uint8_t*>(bytes.data()), bytes.size()}; iovec remote{reinterpret_cast<void*>(address), bytes.size()}; errno = 0;
        const ssize_t count = ::process_vm_writev(pid, &local, 1, &remote, 1, 0);
        if (count == static_cast<ssize_t>(bytes.size())) { error.clear(); return true; }
        const std::string vmError = count < 0 ? std::string(std::strerror(errno)) : "short write (" + std::to_string(count) + "/" + std::to_string(bytes.size()) + ")";
        const std::string memPath = "/proc/" + std::to_string(pid) + "/mem"; const int fd = ::open(memPath.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) { error = "process_vm_writev: " + vmError + "; /proc/pid/mem: " + std::strerror(errno); return false; }
        errno = 0; const ssize_t written = ::pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(address)); const int savedErrno = errno; ::close(fd);
        if (written == static_cast<ssize_t>(bytes.size())) { error.clear(); return true; }
        const std::string memError = written < 0 ? std::string(std::strerror(savedErrno)) : "short write (" + std::to_string(written) + "/" + std::to_string(bytes.size()) + ")";
        error = "process_vm_writev: " + vmError + "; /proc/pid/mem: " + memError; return false;
    }

    static bool runtimeParseHexBytes(const std::string_view text, std::vector<std::uint8_t>& bytes, std::string& error)
    {
        bytes.clear(); std::istringstream stream{std::string(text)}; std::string token;
        while (stream >> token)
        {
            if (token.size() != 2 || runtimeHexNibble(token[0]) < 0 || runtimeHexNibble(token[1]) < 0) { error = "expected hexadecimal bytes such as 90 90 CC"; return false; }
            bytes.push_back(static_cast<std::uint8_t>((runtimeHexNibble(token[0]) << 4) | runtimeHexNibble(token[1])));
        }
        if (bytes.empty()) { error = "no bytes entered"; return false; } error.clear(); return true;
    }

    static std::string runtimeFormatHexBytes(const std::span<const std::uint8_t> bytes)
    {
        std::ostringstream out; out << std::hex << std::uppercase << std::setfill('0');
        for (std::size_t i = 0; i < bytes.size(); ++i) { if (i) out << ' '; out << std::setw(2) << static_cast<unsigned>(bytes[i]); } return out.str();
    }

    struct RuntimeMemoryInspectorState
    {
        pid_t Pid = 0;
        std::uintptr_t Address = 0;
        int ReadSize = 128;
        std::vector<std::uint8_t> Original;
        std::vector<std::uint8_t> Patched;
        std::array<char, 16 * 1024> HexEdit{};
        std::string Status;
        TextEditor Disassembly;
        bool EditorInitialized = false;
        int WriteConfirm = 0;
    };

    static void runtimeRefreshMemoryInspector(RuntimeMemoryInspectorState& state)
    {
        state.Original.assign(static_cast<std::size_t>(std::clamp(state.ReadSize, 1, 4096)), 0); std::string error;
        if (!readProcessMemoryBlock(state.Pid, state.Address, state.Original, error)) { state.Original.clear(); state.Status = "read failed: " + error; return; }
        const std::string formatted = runtimeFormatHexBytes(state.Original); std::snprintf(state.HexEdit.data(), state.HexEdit.size(), "%s", formatted.c_str()); state.Status = "read " + std::to_string(state.Original.size()) + " bytes"; state.WriteConfirm = 0;
#if QUARTZ_HAS_ZYDIS
        std::ostringstream disassembly; std::size_t offset = 0;
        while (offset < state.Original.size())
        {
            std::string text; std::size_t length = 0; if (!runtimeDecodeInstructionText(std::span<const std::uint8_t>(state.Original).subspan(offset), state.Address + offset, text, length) || length == 0) break;
            disassembly << runtimeHexAddress(state.Address + offset) << "  " << text << '\n'; offset += length;
        }
        if (!state.EditorInitialized) { state.Disassembly.SetPalette(shaderEditorPalette()); state.Disassembly.SetShowLineNumbersEnabled(false); state.Disassembly.SetWordWrapEnabled(false); state.EditorInitialized = true; }
        state.Disassembly.SetText(disassembly.str());
#else
        if (!state.EditorInitialized) { state.Disassembly.SetPalette(shaderEditorPalette()); state.Disassembly.SetShowLineNumbersEnabled(false); state.Disassembly.SetWordWrapEnabled(false); state.EditorInitialized = true; }
        state.Disassembly.SetText("Zydis is not available. Install/link Zydis to enable disassembly.");
#endif
    }

    static void drawRuntimeMemoryInspector(RuntimeMemoryInspectorState& state)
    {
        ImGui::SeparatorText("Memory / disassembly");
        int pidValue = static_cast<int>(state.Pid); ImGui::SetNextItemWidth(100.0f); if (ImGui::InputInt("PID##memory", &pidValue)) state.Pid = static_cast<pid_t>(std::max(pidValue, 0)); ImGui::SameLine();
        unsigned long long address = state.Address; ImGui::SetNextItemWidth(190.0f); if (ImGui::InputScalar("Address##memory", ImGuiDataType_U64, &address, nullptr, nullptr, "0x%llX", ImGuiInputTextFlags_CharsHexadecimal)) state.Address = static_cast<std::uintptr_t>(address); ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f); ImGui::InputInt("Bytes", &state.ReadSize); state.ReadSize = std::clamp(state.ReadSize, 1, 4096); ImGui::SameLine(); if (ImGui::Button("Read / disassemble")) runtimeRefreshMemoryInspector(state);
        if (!state.Status.empty()) ImGui::TextDisabled("%s", state.Status.c_str());
        if (!state.Original.empty())
        {
            ImGui::TextDisabled("Editable bytes (snapshot is retained separately for verification/restore)"); ImGui::InputTextMultiline("##hexPatch", state.HexEdit.data(), state.HexEdit.size(), ImVec2(-1.0f, 80.0f));
            ImGui::TextDisabled("Disassembly"); state.Disassembly.Render("##disassembly", ImVec2(-1.0f, 220.0f));
            std::vector<std::uint8_t> patch; std::string error; const bool patchValid = runtimeParseHexBytes(state.HexEdit.data(), patch, error) && patch.size() <= state.Original.size();
            if (!patchValid) ImGui::TextColored(ImVec4(1.0f,0.55f,0.2f,1.0f), "%s", error.empty() ? "patch is larger than the read window" : error.c_str());
            if (state.WriteConfirm == 0) { if (ImGui::Button("WRITE") && patchValid) state.WriteConfirm = 1; }
            else if (state.WriteConfirm == 1) { ImGui::TextColored(ImVec4(1.0f,0.65f,0.2f,1.0f), "Are you sure? PID %d @ %s", state.Pid, runtimeHexAddress(state.Address).c_str()); if (ImGui::Button("Nope")) state.WriteConfirm = 0; ImGui::SameLine(); if (ImGui::Button("Yes, continue")) state.WriteConfirm = 2; }
            else if (state.WriteConfirm == 2) { ImGui::TextColored(ImVec4(1.0f,0.45f,0.2f,1.0f), "Hmm... really really sure?"); if (ImGui::Button("Abort")) state.WriteConfirm = 0; ImGui::SameLine(); if (ImGui::Button("REALLY WRITE")) state.WriteConfirm = 3; }
            else
            {
                ImGui::TextColored(ImVec4(1.0f,0.25f,0.25f,1.0f), "Final check: this writes directly into another process.");
                if (ImGui::Button("I changed my mind")) state.WriteConfirm = 0; ImGui::SameLine();
                if (ImGui::Button("DO IT"))
                {
                    std::vector<std::uint8_t> current(patch.size());
                    if (!readProcessMemoryBlock(state.Pid, state.Address, current, error)) state.Status = "write refused: verification read failed: " + error;
                    else if (!std::equal(current.begin(), current.end(), state.Original.begin())) state.Status = "write refused: target bytes changed since preview";
                    else if (runtimeWriteProcessMemory(state.Pid, state.Address, patch, error)) { state.Patched = patch; state.Status = "it somehow worked"; state.WriteConfirm = 0; }
                    else state.Status = "write failed: " + error;
                }
            }
            if (!state.Patched.empty())
            {
                if (ImGui::Button("Restore original bytes if not ded to hell"))
                {
                    std::vector<std::uint8_t> current(state.Patched.size());
                    if (!runtimeProcessIsAlive(state.Pid)) state.Status = "target process no longer exists: ded to hell";
                    else if (!readProcessMemoryBlock(state.Pid, state.Address, current, error)) state.Status = "restore refused: read failed: " + error;
                    else if (current != state.Patched) state.Status = "restore refused: memory no longer matches patched bytes";
                    else if (runtimeWriteProcessMemory(state.Pid, state.Address, std::span<const std::uint8_t>(state.Original).first(state.Patched.size()), error)) { state.Status = "original bytes restored"; state.Patched.clear(); runtimeRefreshMemoryInspector(state); }
                    else state.Status = "restore failed: " + error;
                }
            }
        }
    }

    static void drawRuntimePointers(RuntimeBindingEngine& engine)
    {
        static RuntimeMemoryInspectorState inspector;
        ImGui::TextUnformatted("Pointer assignments"); ImGui::SameLine(); if (ImGui::Button("+ Add pointer")) engine.addPointer();
        ImGui::SameLine(); ImGui::TextDisabled("Descriptors are models; pointers assign a model to a real process address and retain provenance.");
        std::optional<std::size_t> erase;
        std::vector<RuntimeObjectPointer*> order; for (auto& pointer : engine.pointers()) order.push_back(&pointer); runtimeSortUiNodes(order);
        auto renderPointer = [&](RuntimeObjectPointer* pointer)
        {
            const std::size_t index = static_cast<std::size_t>(pointer - engine.pointers().data()); ImGui::PushID(static_cast<int>(pointer->Id & 0x7fffffffULL));
            const std::string header = std::string(pointer->Name) + (pointer->Resolved ? "  " + runtimeHexAddress(pointer->Address) : "  UNRESOLVED") + "###Pointer" + std::to_string(pointer->Id);
            if (ImGui::CollapsingHeader(header.c_str()))
            {
                bool changed = false; changed |= ImGui::Checkbox("Enabled", &pointer->Enabled); ImGui::SameLine(); if (ImGui::SmallButton("Remove")) erase = index; ImGui::SameLine(); ImGui::SetNextItemWidth(220.0f); changed |= ImGui::InputText("Name", pointer->Name, sizeof(pointer->Name));
                ImGui::SetNextItemWidth(170.0f); changed |= ImGui::InputText("Group", pointer->Group, sizeof(pointer->Group)); ImGui::SameLine(); ImGui::SetNextItemWidth(80.0f); changed |= ImGui::InputInt("Order", &pointer->Order); ImGui::SameLine(); if (ImGui::SmallButton("Up##pointerOrder")) { --pointer->Order; changed = true; } ImGui::SameLine(); if (ImGui::SmallButton("Down##pointerOrder")) { ++pointer->Order; changed = true; }
                RuntimeObjectDescriptor* descriptor = engine.findObject(pointer->DescriptorId); ImGui::SetNextItemWidth(320.0f);
                if (ImGui::BeginCombo("Descriptor model", descriptor ? descriptor->Name : "<select model>")) { for (auto& model : engine.objects()) { const bool selected = model.Id == pointer->DescriptorId; if (ImGui::Selectable(model.Name, selected)) { pointer->DescriptorId = model.Id; changed = true; } if (selected) ImGui::SetItemDefaultFocus(); } ImGui::EndCombo(); }
                changed |= drawRuntimeBindingReferenceCombo(engine, "Base address binding", pointer->BaseBindingId); const RuntimeBinding* process = pointer->ProcessBindingId ? engine.findBinding(pointer->ProcessBindingId) : engine.findBinding(pointer->BaseBindingId); ImGui::SetNextItemWidth(320.0f);
                if (ImGui::BeginCombo("Process binding", pointer->ProcessBindingId == 0 ? "<same as base>" : process ? process->Name : "<missing>")) { if (ImGui::Selectable("<same as base>", pointer->ProcessBindingId == 0)) { pointer->ProcessBindingId = 0; changed = true; } for (const auto& candidate : engine.bindings()) { if (candidate.ProcessId <= 0) continue; const bool selected = candidate.Id == pointer->ProcessBindingId; if (ImGui::Selectable(candidate.Name, selected)) { pointer->ProcessBindingId = candidate.Id; changed = true; } } ImGui::EndCombo(); }
                const int step = 1, fast = 16; ImGui::SetNextItemWidth(150.0f); changed |= ImGui::InputScalar("Base offset", ImGuiDataType_S32, &pointer->BaseOffset, &step, &fast, "0x%X", ImGuiInputTextFlags_CharsHexadecimal);
                ImGui::Text("Resolved: %s   PID: %d", pointer->Resolved ? runtimeHexAddress(pointer->Address).c_str() : "<none>", static_cast<int>(pointer->ProcessId)); if (!pointer->Status.empty()) ImGui::TextDisabled("%s", pointer->Status.c_str());
                if (ImGui::TreeNode("Where did this pointer come from?")) { if (pointer->Provenance.empty()) ImGui::TextDisabled("Waiting for provenance..."); for (const auto& stepText : pointer->Provenance) ImGui::BulletText("%s", stepText.c_str()); ImGui::TreePop(); }
                if (pointer->Resolved && ImGui::Button("Inspect this address")) { inspector.Pid = pointer->ProcessId; inspector.Address = pointer->Address; runtimeRefreshMemoryInspector(inspector); }
                if (descriptor && pointer->Resolved)
                {
                    ImGui::SeparatorText("Fields at this instance");
                    if (ImGui::BeginTable("PointerFields", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable))
                    {
                        ImGui::TableSetupColumn("Field"); ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 80); ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 150); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110); ImGui::TableSetupColumn("Raw", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 65); ImGui::TableHeadersRow();
                        for (auto& field : descriptor->Fields)
                        {
                            if (!field.Enabled || runtimeObjectFieldIsFiller(field.Type)) continue; const auto offset = runtimeObjectFieldOffset(*descriptor, field.Id); const auto addressValue = pointer->Address + offset; ImGui::PushID(static_cast<int>(field.Id & 0x7fffffffULL)); ImGui::TableNextRow();
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(field.Name); ImGui::TableNextColumn(); ImGui::Text("+0x%zX", offset); ImGui::TableNextColumn(); ImGui::Text("0x%llX", static_cast<unsigned long long>(addressValue)); ImGui::TableNextColumn(); ImGui::TextUnformatted(runtimeObjectFieldTypeName(field.Type)); ImGui::TableNextColumn();
                            const std::size_t size = std::min<std::size_t>(runtimeObjectFieldSize(field), 32); std::array<std::uint8_t, 32> raw{}; std::string error; const bool rawOk = readProcessMemoryBlock(pointer->ProcessId, addressValue, std::span<std::uint8_t>(raw).first(size), error); if (rawOk) ImGui::TextUnformatted(runtimeFormatHexBytes(std::span<const std::uint8_t>(raw).first(size)).c_str()); else ImGui::TextDisabled("%s", error.c_str());
                            ImGui::TableNextColumn();
                            const bool pointerLike = field.Type == RuntimeObjectFieldType::Pointer || field.Type == RuntimeObjectFieldType::CStringPointer || field.Type == RuntimeObjectFieldType::WStringPointer;
                            if (pointerLike) { std::uintptr_t target = 0; if (readProcessMemoryValue(pointer->ProcessId, addressValue, target, error)) { ImGui::Text("0x%llX", static_cast<unsigned long long>(target)); if (target != 0 && ImGui::IsItemHovered()) ImGui::SetTooltip("Dereferenced pointer target"); } else ImGui::TextDisabled("%s", error.c_str()); }
                            else ImGui::TextDisabled("-");
                            ImGui::TableNextColumn(); if (ImGui::SmallButton("Inspect")) { inspector.Pid = pointer->ProcessId; inspector.Address = addressValue; runtimeRefreshMemoryInspector(inspector); } ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
                if (changed) engine.markChanged();
            }
            ImGui::Separator(); ImGui::PopID();
                };
        for (auto* pointer : order) if (pointer->Group[0] == '\0') renderPointer(pointer);
        std::vector<std::string> pointerGroups;
        for (const auto* pointer : order) if (pointer->Group[0] != '\0' && std::ranges::find(pointerGroups, std::string(pointer->Group)) == pointerGroups.end()) pointerGroups.emplace_back(pointer->Group);
        for (const auto& group : pointerGroups)
        {
            if (!ImGui::CollapsingHeader((group + "###PointerGroup" + group).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
            ImGui::Indent(8.0f);
            for (auto* pointer : order) if (group == pointer->Group) renderPointer(pointer);
            ImGui::Unindent(8.0f);
        }
        if (erase) engine.erasePointer(*erase); if (engine.pointers().empty()) ImGui::TextDisabled("No pointer assignments yet. Add one and point it at a binding with an exact address.");
        drawRuntimeMemoryInspector(inspector);
    }

    static void drawRuntimeObjectDescriptors(RuntimeBindingEngine& engine)
    {
        ImGui::SeparatorText("Object descriptors");
        ImGui::TextWrapped("Object descriptors are reusable layout models only. They do not own a runtime address; assign a model to a real address in the Pointers tab, then bind fields from that pointer instance.");
        if (ImGui::Button("Add object descriptor")) engine.addObject();
        ImGui::SameLine();
        ImGui::TextDisabled("Use filler fields and packing/alignment controls to mirror native layouts, including unwanted padding.");

        constexpr int offsetStep = 1, offsetFastStep = 16;
        std::optional<std::size_t> eraseObject;
        std::vector<RuntimeObjectDescriptor*> objectOrder; for (auto& model : engine.objects()) objectOrder.push_back(&model); runtimeSortUiNodes(objectOrder);
        auto renderObject = [&](RuntimeObjectDescriptor* objectPtr)
        {
            auto& object = *objectPtr; const std::size_t objectIndex = static_cast<std::size_t>(objectPtr - engine.objects().data());
            ImGui::PushID("RuntimeObject");
            ImGui::PushID(static_cast<int>(object.Id & 0x7fffffffULL));
            const std::string header = std::string(object.Name[0] ? object.Name : "Object") + "###RuntimeObject" + std::to_string(object.Id);
            if (!ImGui::CollapsingHeader(header.c_str())) { ImGui::PopID(); ImGui::PopID(); return; }
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled", &object.Enabled);
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove object")) eraseObject = objectIndex;
            ImGui::SameLine(); ImGui::SetNextItemWidth(220.0f); changed |= ImGui::InputText("Name", object.Name, sizeof(object.Name));
            ImGui::SetNextItemWidth(690.0f); changed |= ImGui::InputText("Description", object.Description, sizeof(object.Description));
            ImGui::SetNextItemWidth(180.0f); changed |= ImGui::InputText("Group", object.Group, sizeof(object.Group)); ImGui::SameLine(); ImGui::SetNextItemWidth(80.0f); changed |= ImGui::InputInt("Order", &object.Order); ImGui::SameLine(); if (ImGui::SmallButton("Up##objectOrder")) { --object.Order; changed = true; } ImGui::SameLine(); if (ImGui::SmallButton("Down##objectOrder")) { ++object.Order; changed = true; }

            /* v9 compatibility assignment UI removed: address assignment now lives in the Pointers tab. */
            if (false) changed |= drawRuntimeBindingReferenceCombo(engine, "Base address binding", object.BaseBindingId);
            int packing = static_cast<int>(object.Packing); ImGui::SetNextItemWidth(150.0f);
            if (ImGui::Combo("Packing", &packing, "Natural\0Pack 1\0Pack 2\0Pack 4\0Pack 8\0Pack 16\0")) { object.Packing = static_cast<RuntimeObjectPacking>(packing); changed = true; }
            std::size_t objectSize = 0; runtimeObjectFieldOffset(object, 0, &objectSize);
            ImGui::SameLine(); ImGui::TextDisabled("Model size: %zu B | pointer instances: %zu", objectSize, static_cast<std::size_t>(std::count_if(engine.pointers().begin(), engine.pointers().end(), [&](const RuntimeObjectPointer& p) { return p.DescriptorId == object.Id; })));

            if (ImGui::SmallButton("Add field")) { engine.addObjectField(object); changed = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Add 4-byte filler"))
            {
                auto& field = engine.addObjectField(object);
                std::snprintf(field.Name, sizeof(field.Name), "Padding");
                field.Type = RuntimeObjectFieldType::Filler4;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Add 8-byte filler"))
            {
                auto& field = engine.addObjectField(object);
                std::snprintf(field.Name, sizeof(field.Name), "Padding");
                field.Type = RuntimeObjectFieldType::Filler8;
                changed = true;
            }

            std::optional<std::size_t> eraseField;
            if (ImGui::BeginTable("ObjectFields", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 32.0f);
                ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Align", ImGuiTableColumnFlags_WidthFixed, 75.0f);
                ImGui::TableSetupColumn("Manual", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("String", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 58.0f);
                ImGui::TableHeadersRow();
                for (std::size_t fieldIndex = 0; fieldIndex < object.Fields.size(); ++fieldIndex)
                {
                    auto& field = object.Fields[fieldIndex];
                    ImGui::PushID(static_cast<int>(field.Id & 0x7fffffffULL));
                    const std::size_t offset = runtimeObjectFieldOffset(object, field.Id, &objectSize);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); changed |= ImGui::Checkbox("##on", &field.Enabled);
                    ImGui::TableNextColumn(); ImGui::Text("0x%zX", offset == std::numeric_limits<std::size_t>::max() ? 0 : offset);
                    ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f); changed |= ImGui::InputText("##name", field.Name, sizeof(field.Name));
                    ImGui::TableNextColumn();
                    int type = static_cast<int>(field.Type);
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::Combo("##type", &type, "u8\0i8\0u16\0i16\0u32\0i32\0u64\0i64\0float\0double\0bool\0pointer\0filler 1\0filler 2\0filler 4\0filler 8\0filler 16\0filler 32\0filler custom\0const char*\0const wchar_t*\0char[N]\0wchar_t[N]\0")) { field.Type = static_cast<RuntimeObjectFieldType>(type); changed = true; }
                    ImGui::TableNextColumn();
                    int alignment = static_cast<int>(field.Alignment);
                    ImGui::SetNextItemWidth(-1.0f);
                    static constexpr const char* Alignments[] = {"Auto", "1", "2", "4", "8", "16"};
                    if (ImGui::Combo("##alignment", &alignment, Alignments, static_cast<int>(std::size(Alignments)))) { field.Alignment = static_cast<RuntimeObjectAlignment>(alignment); changed = true; }
                    ImGui::TableNextColumn();
                    changed |= ImGui::Checkbox("##manual", &field.ManualOffset);
                    if (field.ManualOffset) { ImGui::SameLine(); ImGui::SetNextItemWidth(65.0f); changed |= ImGui::InputScalar("##offset", ImGuiDataType_S32, &field.Offset, &offsetStep, &offsetFastStep, "0x%X", ImGuiInputTextFlags_CharsHexadecimal); }
                    ImGui::TableNextColumn();
                    if (field.Type == RuntimeObjectFieldType::FillerCustom)
                    {
                        ImGui::SetNextItemWidth(-1.0f);
                        changed |= ImGui::InputInt("##customSize", &field.CustomFillerBytes);
                        field.CustomFillerBytes = std::max(field.CustomFillerBytes, 1);
                    }
                    else ImGui::Text("%zu", runtimeObjectFieldSize(field));
                    ImGui::TableNextColumn();
                    if (field.Type == RuntimeObjectFieldType::CStringPointer || field.Type == RuntimeObjectFieldType::WStringPointer) { ImGui::SetNextItemWidth(-1.0f); changed |= ImGui::InputInt("##stringMax", &field.StringMaxLength); field.StringMaxLength = std::clamp(field.StringMaxLength, 1, 4096); }
                    else if (field.Type == RuntimeObjectFieldType::FixedCString || field.Type == RuntimeObjectFieldType::FixedWString) { ImGui::SetNextItemWidth(-1.0f); changed |= ImGui::InputInt("##elements", &field.FixedElementCount); field.FixedElementCount = std::clamp(field.FixedElementCount, 1, 4096); }
                    else ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    if (!runtimeObjectFieldIsFiller(field.Type))
                    {
                        if (ImGui::SmallButton("+"))
                        {
                            auto& binding = engine.add();
                            std::snprintf(binding.Name, sizeof(binding.Name), "%s.%s", object.Name, field.Name);
                            binding.Source = RuntimeSourceKind::ObjectField;
                            binding.ObjectId = object.Id;
                            if (const auto it = std::ranges::find_if(engine.pointers(), [&](const RuntimeObjectPointer& p) { return p.DescriptorId == object.Id; }); it != engine.pointers().end()) binding.ObjectPointerId = it->Id;
                            binding.ObjectFieldId = field.Id;
                            binding.WriteMaterial = false;
                            binding.Clamp = false;
                            binding.SmoothingHz = 0.0f;
                            binding.Priority = 10;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create an Object field binding for this field. Pointer fields preserve the exact pointer for chaining another descriptor.");
                        ImGui::SameLine();
                    }
                    if (ImGui::SmallButton("x")) eraseField = fieldIndex;
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (eraseField) { engine.eraseObjectField(object, *eraseField); changed = true; }
            runtimeObjectFieldOffset(object, 0, &object.Size);
            if (changed) engine.markChanged();
            ImGui::PopID(); ImGui::PopID();
                };
        for (auto* object : objectOrder) if (object->Group[0] == '\0') renderObject(object);
        std::vector<std::string> objectGroups;
        for (const auto* object : objectOrder) if (object->Group[0] != '\0' && std::ranges::find(objectGroups, std::string(object->Group)) == objectGroups.end()) objectGroups.emplace_back(object->Group);
        for (const auto& group : objectGroups)
        {
            if (!ImGui::CollapsingHeader((group + "###ObjectGroup" + group).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
            ImGui::Indent(8.0f);
            for (auto* object : objectOrder) if (group == object->Group) renderObject(object);
            ImGui::Unindent(8.0f);
        }
        if (eraseObject) engine.eraseObject(*eraseObject);
        if (engine.objects().empty()) ImGui::TextDisabled("No object descriptors yet. Create a Native process address binding first, capture the object pointer, then describe its fields here.");
    }
