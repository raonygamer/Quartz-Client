
    static void drawRuntimeStateSquare(const char* id, const ImVec4 color)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
        ImGui::Button(id, ImVec2(11.0f, 11.0f));
        ImGui::PopStyleColor(3);
    }

    static bool drawRuntimeActionList(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer, std::vector<RuntimeAction>& actions, const std::uint64_t sourceBindingId, const bool controlOwner)
    {
        bool changed = false;
        if (ImGui::SmallButton("Add action")) { actions.emplace_back(); actions.back().When = controlOwner ? RuntimeActionWhen::OnTrigger : RuntimeActionWhen::OnUpdate; changed = true; }
        std::optional<std::size_t> erase;
        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            auto& action = actions[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Separator();
            changed |= ImGui::Checkbox("##actionEnabled", &action.Enabled);
            ImGui::SameLine(); ImGui::TextDisabled("Action %zu", i + 1);
            ImGui::SameLine(); int when = static_cast<int>(action.When); ImGui::SetNextItemWidth(125.0f);
            if (ImGui::Combo("##actionWhen", &when, "While active\0On trigger\0On update\0On change\0While truthy\0While falsy\0")) { action.When = static_cast<RuntimeActionWhen>(when); changed = true; }
            ImGui::SameLine(); int target = static_cast<int>(action.Target); ImGui::SetNextItemWidth(170.0f);
            if (ImGui::Combo("##actionTarget", &target, "Active shader\0Binding enabled\0Global brightness\0Send framebuffer\0Base color mode\0Material parameter\0Unbound binding value\0Value bank\0Control enabled\0Refresh binding\0Force binding update\0Invalidate binding\0Reset binding state\0Retry register capture\0Rescan binding pattern\0Rebind process\0Clear binding error\0")) { action.Target = static_cast<RuntimeActionTarget>(target); changed = true; }
            ImGui::SameLine(); if (ImGui::SmallButton("Remove action")) erase = i;

            const bool valueUseful = !runtimeActionTargetIsBindingOperation(action.Target);
            if (valueUseful)
            {
                int mode = static_cast<int>(action.ValueMode); ImGui::SetNextItemWidth(180.0f);
                if (ImGui::Combo("Value source", &mode, "Constant\0Owner/source value\0Another binding\0Value bank\0")) { action.ValueMode = static_cast<RuntimeActionValueMode>(mode); changed = true; }
                if (action.ValueMode == RuntimeActionValueMode::BindingValue) changed |= drawRuntimeBindingReferenceCombo(engine, "Value binding", action.ValueBindingId, sourceBindingId);
                else if (action.ValueMode == RuntimeActionValueMode::BankValue) changed |= drawRuntimeBankReferenceCombo(engine, "Value bank source", action.BankValueId);
            }

            if (action.Target == RuntimeActionTarget::ActiveShader)
            {
                if (action.ValueMode == RuntimeActionValueMode::Constant)
                {
                    const ShaderPreset* shaderById = action.ShaderId[0] ? findShaderPresetById(action.ShaderId) : nullptr;
                    const char* preview = action.ShaderPresetIndex == -1 ? "<previous shader>" : shaderById ? shaderById->Name.c_str() : action.ShaderPresetIndex > 0 && action.ShaderPresetIndex <= static_cast<int>(ShaderPresets.size()) ? ShaderPresets[static_cast<std::size_t>(action.ShaderPresetIndex - 1)].Name.c_str() : "<select shader>";
                    ImGui::SetNextItemWidth(300.0f);
                    if (ImGui::BeginCombo("Shader", preview))
                    {
                        if (ImGui::Selectable("<previous shader>", action.ShaderPresetIndex == -1)) { action.ShaderPresetIndex = -1; action.ShaderId[0] = '\0'; changed = true; }
                        for (std::size_t p = 0; p < ShaderPresets.size(); ++p) { const bool selected = action.ShaderPresetIndex == static_cast<int>(p + 1); if (ImGui::Selectable(ShaderPresets[p].Name.c_str(), selected)) { action.ShaderPresetIndex = static_cast<int>(p + 1); std::snprintf(action.ShaderId, sizeof(action.ShaderId), "%s", ShaderPresets[p].Id.c_str()); changed = true; } }
                        ImGui::EndCombo();
                    }
                }
                ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Crossfade", &action.TransitionSeconds, 0.02f, 0.0f, 10.0f, "%.2f s");
            }
            else if (action.Target == RuntimeActionTarget::BindingEnabled)
            {
                changed |= drawRuntimeBindingReferenceCombo(engine, "Target binding", action.TargetBindingId, sourceBindingId);
                if (action.ValueMode == RuntimeActionValueMode::Constant) changed |= ImGui::Checkbox("Enabled value", &action.BoolValue);
            }
            else if (action.Target == RuntimeActionTarget::GlobalBrightness)
            {
                if (action.ValueMode == RuntimeActionValueMode::Constant) changed |= ImGui::SliderFloat("Brightness", &action.Value, 0.0f, 1.0f, "%.3f");
            }
            else if (action.Target == RuntimeActionTarget::SendFramebuffer)
            {
                if (action.ValueMode == RuntimeActionValueMode::Constant) changed |= ImGui::Checkbox("Send framebuffer", &action.BoolValue);
            }
            else if (action.Target == RuntimeActionTarget::BaseColorMode)
            {
                if (action.ValueMode == RuntimeActionValueMode::Constant) { int mode = std::clamp(static_cast<int>(std::lround(action.Value)), 0, 2); if (ImGui::Combo("Base color mode", &mode, "RGB wave\0Solid\0Shader\0")) { action.Value = static_cast<float>(mode); changed = true; } }
            }
            else if (action.Target == RuntimeActionTarget::MaterialParameter)
            {
                ImGui::SetNextItemWidth(300.0f);
                if (ImGui::BeginCombo("Material id", action.TargetId[0] ? action.TargetId : "<select material uniform>"))
                {
                    for (const auto& parameter : shaderFramebuffer.materialParameters())
                    {
                        const bool selected = parameter.PersistenceKey == action.TargetId;
                        const std::string label = parameter.Label + "  [" + parameter.PersistenceKey + "]";
                        if (ImGui::Selectable(label.c_str(), selected)) { std::snprintf(action.TargetId, sizeof(action.TargetId), "%s", parameter.PersistenceKey.c_str()); action.TargetComponent = std::clamp(action.TargetComponent, 0, std::max(parameter.Components - 1, 0)); changed = true; }
                    }
                    ImGui::EndCombo();
                }
                if (const auto* parameter = shaderFramebuffer.findMaterialParameter(action.TargetId); parameter && parameter->Components > 1) { ImGui::SameLine(); changed |= ImGui::SliderInt("Component", &action.TargetComponent, 0, parameter->Components - 1); }
                if (action.ValueMode == RuntimeActionValueMode::Constant) { ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Value", &action.Value, 0.01f); }
            }
            else if (action.Target == RuntimeActionTarget::BindingValue)
            {
                changed |= drawRuntimeBindingReferenceCombo(engine, "Unbound target", action.TargetBindingId, sourceBindingId, true);
                if (action.ValueMode == RuntimeActionValueMode::Constant) { ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Value", &action.Value, 0.01f); }
            }
            else if (action.Target == RuntimeActionTarget::ValueBank)
            {
                changed |= drawRuntimeBankReferenceCombo(engine, "Bank target", action.TargetBankValueId);
                if (action.ValueMode == RuntimeActionValueMode::Constant)
                {
                    if (const RuntimeValueBankEntry* bank = engine.findBankValue(action.TargetBankValueId); bank && bank->Type == RuntimeBankValueType::String) changed |= ImGui::InputText("String value", action.StringValue, sizeof(action.StringValue));
                    else { ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Value", &action.Value, 0.01f); }
                }
            }
            else if (action.Target == RuntimeActionTarget::ControlEnabled)
            {
                changed |= drawRuntimeControlReferenceCombo(engine, "Target control", action.TargetControlId);
                if (action.ValueMode == RuntimeActionValueMode::Constant) changed |= ImGui::Checkbox("Enabled value", &action.BoolValue);
            }
            else if (runtimeActionTargetIsBindingOperation(action.Target))
            {
                changed |= drawRuntimeBindingReferenceCombo(engine, "Target binding", action.TargetBindingId, sourceBindingId);
                ImGui::TextDisabled("Binding operation; value source is ignored.");
            }
            ImGui::PopID();
        }
        if (erase) { actions.erase(actions.begin() + static_cast<std::ptrdiff_t>(*erase)); changed = true; }
        return changed;
    }

    static bool drawRuntimeReferenceList(RuntimeBindingEngine& engine, RuntimeBinding& binding, const bool showWeights, const bool massCompare = false)
    {
        bool changed = false;
        if (ImGui::SmallButton("Add binding member")) { binding.References.push_back({RuntimeReferenceKind::Binding, 0, 0, 1.0f, true}); changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Add control member")) { binding.References.push_back({RuntimeReferenceKind::Control, 0, 0, 1.0f, true}); changed = true; }
        std::optional<std::size_t> erase;
        for (std::size_t i = 0; i < binding.References.size(); ++i)
        {
            auto& reference = binding.References[i];
            ImGui::PushID(static_cast<int>(i));
            changed |= ImGui::Checkbox("##enabled", &reference.Enabled);
            ImGui::SameLine();
            int kind = static_cast<int>(reference.Kind);
            ImGui::SetNextItemWidth(95.0f);
            if (ImGui::Combo("##kind", &kind, "Binding\0Control\0")) { reference.Kind = static_cast<RuntimeReferenceKind>(kind); reference.Id = 0; reference.Signal = 0; changed = true; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(245.0f);
            if (reference.Kind == RuntimeReferenceKind::Binding)
            {
                const RuntimeBinding* selected = engine.findBinding(reference.Id);
                if (ImGui::BeginCombo("##member", selected ? selected->Name : "<select binding>"))
                {
                    for (const auto& candidate : engine.bindings())
                    {
                        if (candidate.Id == binding.Id) continue;
                        const bool isSelected = candidate.Id == reference.Id;
                        if (ImGui::Selectable(candidate.Name, isSelected)) { reference.Id = candidate.Id; changed = true; }
                        if (isSelected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(135.0f);
                static constexpr const char* Signals[] = {"Value", "Raw", "Has value", "Read ok", "Enabled", "Priority", "Has address", "Has error", "Seconds since success"};
                reference.Signal = std::clamp(reference.Signal, 0, static_cast<int>(std::size(Signals)) - 1);
                if (ImGui::Combo("##memberSignal", &reference.Signal, Signals, static_cast<int>(std::size(Signals)))) changed = true;
            }
            else
            {
                const RuntimeControlRule* selected = engine.findControl(reference.Id);
                if (ImGui::BeginCombo("##member", selected ? selected->Name : "<select control>"))
                {
                    for (const auto& candidate : engine.controls())
                    {
                        const bool isSelected = candidate.Id == reference.Id;
                        if (ImGui::Selectable(candidate.Name, isSelected)) { reference.Id = candidate.Id; changed = true; }
                        if (isSelected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(135.0f);
                static constexpr const char* Signals[] = {"Active", "Triggered", "Enabled", "Trigger count", "Priority", "Seconds since trigger", "Source value"};
                reference.Signal = std::clamp(reference.Signal, 0, static_cast<int>(std::size(Signals)) - 1);
                if (ImGui::Combo("##memberSignal", &reference.Signal, Signals, static_cast<int>(std::size(Signals)))) changed = true;
            }
            if (showWeights)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                changed |= ImGui::DragFloat("##weight", &reference.Weight, 0.01f, -1000.0f, 1000.0f, "w %.2f");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) erase = i;

            if (massCompare)
            {
                ImGui::Indent(24.0f);
                changed |= ImGui::Checkbox("Own comparison", &reference.UseOwnComparison);
                if (reference.UseOwnComparison)
                {
                    ImGui::SameLine();
                    int condition = static_cast<int>(reference.CompareCondition);
                    ImGui::SetNextItemWidth(105.0f);
                    if (ImGui::Combo("##compareCondition", &condition, "==\0!=\0<\0<=\0>\0>=\0between\0outside\0")) { reference.CompareCondition = static_cast<RuntimeCompareCondition>(condition); changed = true; }
                    ImGui::SameLine(); ImGui::SetNextItemWidth(100.0f); changed |= ImGui::DragFloat("A##memberCompare", &reference.CompareA, 0.01f);
                    if (reference.CompareCondition == RuntimeCompareCondition::Between || reference.CompareCondition == RuntimeCompareCondition::Outside)
                    { ImGui::SameLine(); ImGui::SetNextItemWidth(100.0f); changed |= ImGui::DragFloat("B##memberCompare", &reference.CompareB, 0.01f); }
                    if (reference.CompareCondition == RuntimeCompareCondition::Equal || reference.CompareCondition == RuntimeCompareCondition::NotEqual)
                    { ImGui::SameLine(); ImGui::SetNextItemWidth(100.0f); changed |= ImGui::DragFloat("Tol##memberCompare", &reference.CompareTolerance, 0.0001f, 0.000001f, 1000.0f, "%.4g"); }
                }
                ImGui::Unindent(24.0f);
            }
            ImGui::PopID();
        }
        if (erase) { binding.References.erase(binding.References.begin() + static_cast<std::ptrdiff_t>(*erase)); changed = true; }
        return changed;
    }

    static bool drawRuntimeBinding(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer, RuntimeBinding& binding, bool& erase)
    {
        bool changed = false;
        ImGui::PushID("RuntimeBinding");
        ImGui::PushID(static_cast<int>(binding.Id & 0x7fffffffULL));
        const bool booleanState = runtimeBindingLooksBoolean(binding);
        const bool runtimeEnabled = binding.Enabled && binding.RuntimeEnabled;
        const bool waitingState = runtimeEnabled && !binding.HasValue && binding.Error.empty();
        const bool goodState = binding.HasValue && (!booleanState || binding.Value >= 0.5f);
        const ImVec4 stateColor = runtimeStateColor(runtimeEnabled, goodState, !binding.Error.empty(), waitingState);
        drawRuntimeStateSquare("##bindingState", stateColor);
        ImGui::SameLine();
        std::string bindingHeader = std::string(binding.Name[0] ? binding.Name : "Binding") + "  [p" + std::to_string(binding.Priority) + "]";
        if (!binding.Enabled) bindingHeader += "  DISABLED";
        else if (!binding.RuntimeEnabled) bindingHeader += "  RUNTIME OFF";
        else if (!binding.Error.empty()) bindingHeader += "  ERROR";
        else if (!binding.HasValue) bindingHeader += "  WAITING";
        else if (booleanState) bindingHeader += binding.Value >= 0.5f ? "  TRUE" : "  FALSE";
        else { char value[64]; std::snprintf(value, sizeof(value), "  %.5g", binding.Value); bindingHeader += value; }
        bindingHeader += "###RuntimeBinding" + std::to_string(binding.Id);
        if (!ImGui::CollapsingHeader(bindingHeader.c_str()))
        {
            ImGui::PopID();
            ImGui::PopID();
            return false;
        }
        ImGui::Indent(10.0f);
        ImGui::TextColored(stateColor, "%s", !binding.Enabled ? "Disabled" : !binding.RuntimeEnabled ? "Runtime disabled by control" : !binding.Error.empty() ? "Error" : !binding.HasValue ? "Waiting for value" : booleanState ? (binding.Value >= 0.5f ? "True / active" : "False / inactive") : "Ready");
        ImGui::SameLine();
        if (binding.HasValue) ImGui::TextDisabled("raw %.6g  value %.6g", binding.RawValue, binding.Value);
        if (binding.HasString) { ImGui::SameLine(); ImGui::TextDisabled("string: %s", binding.StringValue.c_str()); }
        if (!binding.Error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "%s", binding.Error.c_str());
        changed |= ImGui::Checkbox("Enabled", &binding.Enabled);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) erase = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        changed |= ImGui::InputText("Name", binding.Name, sizeof(binding.Name));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        changed |= ImGui::InputInt("Priority", &binding.Priority);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lower priorities run first. Higher priorities run later and win material/output conflicts.");
        ImGui::SameLine(); ImGui::SetNextItemWidth(160.0f); changed |= ImGui::InputText("Group", binding.Group, sizeof(binding.Group));
        ImGui::SameLine(); ImGui::SetNextItemWidth(80.0f); changed |= ImGui::InputInt("Order", &binding.Order); ImGui::SameLine(); if (ImGui::SmallButton("Up##bindingOrder")) { --binding.Order; changed = true; } ImGui::SameLine(); if (ImGui::SmallButton("Down##bindingOrder")) { ++binding.Order; changed = true; }

        int source = static_cast<int>(binding.Source);
        ImGui::SetNextItemWidth(250.0f);
        if (ImGui::BeginCombo("Source", runtimeSourceName(binding.Source)))
        {
            for (int i = 0; i <= static_cast<int>(RuntimeSourceKind::ProfileState); ++i)
            {
                const auto candidate = static_cast<RuntimeSourceKind>(i);
                if (ImGui::Selectable(runtimeSourceName(candidate), i == source))
                {
                    binding.Source = candidate;
                    binding.Signal = 0;
                    binding.HasValue = false;
                    binding.LastReadSucceeded = false;
                    binding.HasAddress = false;
                    binding.AddressValue = 0;
                    binding.NextUpdate = 0.0;
                    source = i;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }

        const auto signals = runtimeSignalNames(binding.Source);
        binding.Signal = std::clamp(binding.Signal, 0, std::max(static_cast<int>(signals.size()) - 1, 0));
        if (signals.size() > 1)
        {
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::BeginCombo("Signal", signals[static_cast<std::size_t>(binding.Signal)].data()))
            {
                for (int i = 0; i < static_cast<int>(signals.size()); ++i)
                    if (ImGui::Selectable(signals[static_cast<std::size_t>(i)].data(), binding.Signal == i)) { binding.Signal = i; changed = true; }
                ImGui::EndCombo();
            }
        }

        if (binding.Source == RuntimeSourceKind::Constant)
            changed |= ImGui::DragFloat("Constant value", &binding.Constant, 0.01f);
        else if (binding.Source == RuntimeSourceKind::Unbound)
        {
            ImGui::SeparatorText("Writable value");
            changed |= ImGui::DragFloat("Stored value", &binding.UnboundValue, 0.01f);
            ImGui::TextDisabled("Controls can write this binding directly. Other bindings can consume it through passthrough, aggregate, compare or parameter links.");
        }
        else if (binding.Source == RuntimeSourceKind::ValueBank)
        {
            ImGui::SeparatorText("Value bank");
            changed |= drawRuntimeBankReferenceCombo(engine, "Bank value", binding.BankValueId);
            if (const auto* value = engine.findBankValue(binding.BankValueId)) ImGui::TextDisabled("%s%s", runtimeBankValueTypeName(value->Type), value->ChangedThisFrame ? "  changed this frame" : "");
        }
        else if (binding.Source == RuntimeSourceKind::StringConstant)
        {
            ImGui::SeparatorText("String value");
            changed |= ImGui::InputText("Text", binding.StringConstant, sizeof(binding.StringConstant));
            ImGui::TextDisabled("String bindings expose a numeric presence/length signal plus a real string side-channel for string-aware controls and the value bank.");
        }
        else if (binding.Source == RuntimeSourceKind::ProfileState)
        {
            ImGui::SeparatorText("Binding profile state");
            if (binding.Signal != 0)
            {
                const RuntimeBindingProfile* profile = engine.findProfile(binding.ProfileId);
                ImGui::SetNextItemWidth(300.0f);
                if (ImGui::BeginCombo("Profile", profile ? profile->Name : "<select profile>"))
                {
                    for (const auto& candidate : engine.profiles()) { const bool selected = candidate.Id == binding.ProfileId; if (ImGui::Selectable(candidate.Name, selected)) { binding.ProfileId = candidate.Id; changed = true; } if (selected) ImGui::SetItemDefaultFocus(); }
                    ImGui::EndCombo();
                }
            }
            ImGui::TextDisabled("Profile bindings can drive indicators or automation from the currently active graph profile.");
        }
        else if (binding.Source == RuntimeSourceKind::BindingValue)
        {
            ImGui::SeparatorText("Binding passthrough");
            changed |= drawRuntimeBindingReferenceCombo(engine, "Source binding", binding.ValueBindingId, binding.Id);
        }
        else if (binding.Source == RuntimeSourceKind::ControlStatus)
        {
            ImGui::SeparatorText("Control status");
            changed |= drawRuntimeControlReferenceCombo(engine, "Control to inspect", binding.ControlStatusId);
        }
        else if (binding.Source == RuntimeSourceKind::Aggregate)
        {
            ImGui::SeparatorText("Aggregate");
            int operation = static_cast<int>(binding.AggregateOperation);
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("Operation", &operation, "Sum\0Average\0Minimum\0Maximum\0Product\0Count\0Count truthy\0Fraction truthy\0Any\0All\0")) { binding.AggregateOperation = static_cast<RuntimeAggregateOperation>(operation); changed = true; }
            changed |= drawRuntimeReferenceList(engine, binding, binding.AggregateOperation <= RuntimeAggregateOperation::Product);
            ImGui::TextDisabled("Members may be bindings or controls. Control members expose active/trigger state as numeric values.");
        }
        else if (binding.Source == RuntimeSourceKind::MassCompare)
        {
            ImGui::SeparatorText("Mass compare");
            int condition = static_cast<int>(binding.CompareCondition);
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::Combo("Compare", &condition, "==\0!=\0<\0<=\0>\0>=\0between\0outside\0")) { binding.CompareCondition = static_cast<RuntimeCompareCondition>(condition); changed = true; }
            ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("A", &binding.CompareA, 0.01f);
            if (binding.CompareCondition == RuntimeCompareCondition::Between || binding.CompareCondition == RuntimeCompareCondition::Outside) { ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("B", &binding.CompareB, 0.01f); }
            if (binding.CompareCondition == RuntimeCompareCondition::Equal || binding.CompareCondition == RuntimeCompareCondition::NotEqual) { ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Tolerance", &binding.CompareTolerance, 0.0001f, 0.000001f, 1000.0f, "%.6f"); }
            int result = static_cast<int>(binding.CompareResult);
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("Result", &result, "Any\0All\0None\0Count\0Fraction\0First match index\0")) { binding.CompareResult = static_cast<RuntimeMassCompareResult>(result); changed = true; }
            changed |= drawRuntimeReferenceList(engine, binding, false, true);
        }
        else if (binding.Source == RuntimeSourceKind::ObjectField || binding.Source == RuntimeSourceKind::ObjectStatus)
        {
            ImGui::SeparatorText(binding.Source == RuntimeSourceKind::ObjectField ? "Object field" : "Object / pointer status");
            RuntimeObjectPointer* pointer = engine.findPointer(binding.ObjectPointerId);
            RuntimeObjectDescriptor* object = pointer ? engine.findObject(pointer->DescriptorId) : engine.findObject(binding.ObjectId);
            ImGui::SetNextItemWidth(360.0f);
            if (ImGui::BeginCombo("Pointer instance", pointer ? pointer->Name : "<select pointer>"))
            {
                for (auto& candidate : engine.pointers())
                {
                    const bool selected = candidate.Id == binding.ObjectPointerId;
                    const auto* descriptor = engine.findObject(candidate.DescriptorId);
                    std::string label = std::string(candidate.Name) + (descriptor ? "  [" + std::string(descriptor->Name) + "]" : "  [missing model]");
                    if (ImGui::Selectable(label.c_str(), selected)) { binding.ObjectPointerId = candidate.Id; binding.ObjectId = candidate.DescriptorId; binding.ObjectFieldId = 0; object = engine.findObject(candidate.DescriptorId); changed = true; }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (binding.Source == RuntimeSourceKind::ObjectField && object)
            {
                const RuntimeObjectField* field = engine.findObjectField(*object, binding.ObjectFieldId);
                ImGui::SetNextItemWidth(320.0f);
                if (ImGui::BeginCombo("Field", field ? field->Name : "<select readable field>"))
                {
                    for (const auto& candidate : object->Fields)
                    {
                        if (!candidate.Enabled || runtimeObjectFieldIsFiller(candidate.Type)) continue;
                        const bool selected = candidate.Id == binding.ObjectFieldId;
                        std::size_t objectSize = 0;
                        const auto offset = runtimeObjectFieldOffset(*object, candidate.Id, &objectSize);
                        std::ostringstream label; label << candidate.Name << "  +0x" << std::hex << offset << "  [" << runtimeObjectFieldTypeName(candidate.Type) << "]";
                        if (ImGui::Selectable(label.str().c_str(), selected)) { binding.ObjectFieldId = candidate.Id; changed = true; }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            if (pointer) ImGui::TextDisabled("%s", pointer->Status.c_str()); else if (object) ImGui::TextDisabled("model: %s", object->Name);
        }

        static std::vector<RuntimeProcessInfo> processes;
        if ((binding.Source == RuntimeSourceKind::NativeProcess || binding.Source == RuntimeSourceKind::NativeAddress) && processes.empty())
            processes = enumerateRuntimeProcesses();

        if (binding.Source == RuntimeSourceKind::NativeProcess || binding.Source == RuntimeSourceKind::NativeAddress)
        {
            ImGui::SeparatorText(binding.Source == RuntimeSourceKind::NativeProcess ? "Native process memory" : "Native process address");
            changed |= drawRuntimeProcessSelector(binding, processes);
            static std::vector<RuntimeProcessModule> modules;
            static int modulePid = -1;
            if (ImGui::SmallButton("Refresh modules") || modulePid != binding.ProcessId)
            {
                modules = binding.ProcessId > 0 ? enumerateRuntimeModules(static_cast<pid_t>(binding.ProcessId)) : std::vector<RuntimeProcessModule>{};
                modulePid = binding.ProcessId;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%zu mapped modules", modules.size());
            ImGui::SetNextItemWidth(410.0f);
            const char* noModuleLabel = binding.AddressMode == ProcessAddressMode::Signature ? "<all mappings>" : "<absolute address>";
            if (ImGui::BeginCombo("Module / base", binding.Module[0] ? binding.Module : noModuleLabel))
            {
                if (ImGui::Selectable(noModuleLabel, binding.Module[0] == '\0')) { binding.Module[0] = '\0'; changed = true; }
                for (const auto& module : modules)
                {
                    std::ostringstream address;
                    address << module.Name << "  0x" << std::hex << module.Base;
                    if (ImGui::Selectable(address.str().c_str(), std::string_view(binding.Module) == module.Name))
                    {
                        std::snprintf(binding.Module, sizeof(binding.Module), "%s", module.Name.c_str());
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            static constexpr const char* AddressModes[] = {"Address / pointer chain", "Pattern scan"};
            int addressMode = static_cast<int>(binding.AddressMode);
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::Combo("Address source", &addressMode, AddressModes, static_cast<int>(std::size(AddressModes))))
            {
                binding.AddressMode = static_cast<ProcessAddressMode>(addressMode);
                resetRuntimeSignatureScan(binding);
                binding.SignatureConfigHash = 0;
                changed = true;
            }
            if (binding.AddressMode == ProcessAddressMode::AddressChain)
            {
                changed |= ImGui::InputText("Address / pointer chain", binding.Address, sizeof(binding.Address));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Examples: +0x1234   |   +0x1234 -> +0x28 -> +0x18   |   libgame.so+0x1234 -> +0x20");
            }
            else
            {
                int patternKind = static_cast<int>(binding.SignaturePatternKind);
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::Combo("Pattern kind", &patternKind, "Hexadecimal pattern\0Opcode pattern\0")) { binding.SignaturePatternKind = static_cast<RuntimeSignaturePatternKind>(patternKind); resetRuntimeSignatureScan(binding); binding.SignatureConfigHash = 0; changed = true; }
                if (binding.SignaturePatternKind == RuntimeSignaturePatternKind::HexadecimalPattern)
                {
                    const bool signatureChanged = ImGui::InputTextMultiline("Hexadecimal pattern", binding.Signature, sizeof(binding.Signature), ImVec2(-1.0f, 72.0f));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hex bytes with wildcards: DE AD ? BE EF | 48 8B 05 ?? ?? ?? ?? | nibble wildcards A? and ?F.");
                    changed |= signatureChanged;
                }
                else
                {
                    const bool signatureChanged = ImGui::InputTextMultiline("Opcode pattern", binding.Signature, sizeof(binding.Signature), ImVec2(-1.0f, 120.0f));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("One Intel-syntax instruction per line. * matches any text and ? matches one character. Example:\nmov rax, r15\nmovsxd rax, dword ptr [rax+*]\ncvtsi2ss xmm0, eax");
                    changed |= signatureChanged;
#if !QUARTZ_HAS_ZYDIS
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), "Opcode patterns require Zydis.");
#endif
                }
                changed |= ImGui::Checkbox("Executable mappings only", &binding.SignatureExecutableOnly);
                static constexpr const char* ResolveModes[] = {"Match address + offset", "x86-64 RIP-relative disp32", "Pointer stored at match + offset", "x86-64 register-relative capture", "32-bit address stored at match + offset"};
                int resolveMode = static_cast<int>(binding.SignatureResolve);
                ImGui::SetNextItemWidth(300.0f);
                if (ImGui::Combo("Pattern result", &resolveMode, ResolveModes, static_cast<int>(std::size(ResolveModes)))) { binding.SignatureResolve = static_cast<SignatureResultMode>(resolveMode); changed = true; }
                ImGui::SetNextItemWidth(180.0f);
                const char* resultOffsetLabel = binding.SignatureResolve == SignatureResultMode::RipRelative32 ? "Displacement offset" : binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture ? "Instruction offset" : "Result offset";
                changed |= ImGui::InputInt(resultOffsetLabel, &binding.SignatureResultOffset);
                if (binding.SignatureResolve == SignatureResultMode::Address32AtOffset) ImGui::TextDisabled("Reads a little-endian uint32_t at match + result offset, zero-extends it to uintptr_t, and uses that value as the resolved address.");
                if (binding.SignatureResolve == SignatureResultMode::RipRelative32)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(140.0f);
                    changed |= ImGui::InputInt("Instruction size", &binding.SignatureInstructionSize);
                    binding.SignatureInstructionSize = std::max(binding.SignatureInstructionSize, 1);
                }
                else if (binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture)
                {
                    static constexpr const char* Registers[] = {"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};
                    int reg = static_cast<int>(binding.SignatureRegister);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(110.0f);
                    if (ImGui::Combo("Base register", &reg, Registers, static_cast<int>(std::size(Registers)))) { binding.SignatureRegister = static_cast<RuntimeX64Register>(reg); changed = true; }
                    static constexpr const char* Displacements[] = {"signed disp8", "signed disp32", "manual"};
                    int displacementType = static_cast<int>(binding.SignatureDisplacementType);
                    ImGui::SetNextItemWidth(180.0f);
                    if (ImGui::Combo("Displacement", &displacementType, Displacements, static_cast<int>(std::size(Displacements)))) { binding.SignatureDisplacementType = static_cast<RuntimeDisplacementType>(displacementType); changed = true; }
                    if (binding.SignatureDisplacementType == RuntimeDisplacementType::Manual)
                    {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(160.0f);
                        changed |= ImGui::InputInt("Manual displacement", &binding.SignatureManualDisplacement);
                    }
                    else
                    {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(160.0f);
                        changed |= ImGui::InputInt("Displacement offset", &binding.SignatureRegisterDisplacementOffset);
                    }
                    ImGui::SetNextItemWidth(180.0f);
                    changed |= ImGui::DragFloat("Capture timeout", &binding.SignatureCaptureTimeoutSeconds, 0.1f, 0.1f, 120.0f, "%.1f s");
                    ImGui::TextDisabled("A temporary hardware execution breakpoint captures the selected register when the matched instruction executes. The breakpoint is removed immediately after capture; normal reads then use process_vm_readv().");
                }
                ImGui::SetNextItemWidth(180.0f);
                changed |= ImGui::DragFloat("Retry interval", &binding.SignatureRetrySeconds, 0.05f, 0.1f, 60.0f, "%.2f s");
                changed |= ImGui::InputText("Result / pointer chain", binding.Address, sizeof(binding.Address));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("The resolved pattern address becomes the base. Use +0x0 for the result itself or chains such as +0x0 -> +0x18.");
                if (ImGui::Button("Rescan pattern"))
                {
                    resetRuntimeSignatureScan(binding);
                    binding.SignatureConfigHash = 0;
                    binding.NextUpdate = 0.0;
                }
                if (binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture && binding.SignatureInstructionAddress != 0 && binding.SignatureResolvedAddress == 0)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Retry register capture"))
                    {
                        binding.SignatureRegisterCapture.reset();
                        binding.NextRegisterCapture = 0.0;
                        binding.NextUpdate = 0.0;
                    }
                }
                ImGui::SameLine();
                if (binding.SignatureResolvedAddress != 0) ImGui::TextDisabled("resolved: 0x%llX", static_cast<unsigned long long>(binding.SignatureResolvedAddress));
                else if (!binding.SignatureStatus.empty()) ImGui::TextDisabled("%s", binding.SignatureStatus.c_str());
                if (binding.SignatureResolvedAddress == 0 && binding.SignatureProgress > 0.0f && binding.SignatureInstructionAddress == 0) ImGui::ProgressBar(binding.SignatureProgress, ImVec2(320.0f, 0.0f));
                if (binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture)
                {
                    if (binding.SignatureMatchAddress != 0) ImGui::TextDisabled("Match 0x%llX   instruction 0x%llX", static_cast<unsigned long long>(binding.SignatureMatchAddress), static_cast<unsigned long long>(binding.SignatureInstructionAddress));
                    if (binding.SignatureCapturedRegister != 0 || binding.SignatureResolvedAddress != 0) ImGui::TextDisabled("%s 0x%llX   displacement %lld", runtimeX64RegisterName(binding.SignatureRegister), static_cast<unsigned long long>(binding.SignatureCapturedRegister), static_cast<long long>(binding.SignatureCapturedDisplacement));
                }
                else ImGui::TextDisabled("The pattern locates a stable instruction or data reference. Hexadecimal patterns match bytes; opcode patterns match readable Intel-syntax instructions through Zydis.");
            }
            if (binding.Source == RuntimeSourceKind::NativeProcess)
            {
                static constexpr const char* Types[] = {"u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "float", "double", "bool"};
                int valueType = static_cast<int>(binding.ValueType);
                if (ImGui::Combo("Value type", &valueType, Types, static_cast<int>(std::size(Types)))) { binding.ValueType = static_cast<ProcessValueType>(valueType); changed = true; }
                ImGui::TextDisabled("Read-only through process_vm_readv(); ptrace_scope/permissions still apply.");
            }
            else
            {
                if (binding.HasAddress) ImGui::Text("Exact address: 0x%llX", static_cast<unsigned long long>(binding.AddressValue));
                ImGui::TextDisabled("Address bindings preserve uintptr_t precision for object descriptors instead of squeezing pointers through float values.");
            }
        }
        else if (binding.Source == RuntimeSourceKind::BindingStatus)
        {
            ImGui::SeparatorText("Binding status");
            const RuntimeBinding* target = engine.findBinding(binding.StatusBindingId);
            ImGui::SetNextItemWidth(320.0f);
            if (ImGui::BeginCombo("Binding to inspect", target ? target->Name : "<select binding>"))
            {
                for (const auto& candidate : engine.bindings())
                {
                    if (candidate.Id == binding.Id) continue;
                    const bool selected = candidate.Id == binding.StatusBindingId;
                    if (ImGui::Selectable(candidate.Name, selected)) { binding.StatusBindingId = candidate.Id; changed = true; }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Status values are normal 0/1 binding sources, so they can drive shader uniforms, parameter links, or control rules.");
        }

        if (ImGui::TreeNode("Value origin"))
        {
            const auto sourceSignals = runtimeSignalNames(binding.Source);
            const int signalIndex = std::clamp(binding.Signal, 0, std::max(static_cast<int>(sourceSignals.size()) - 1, 0));
            ImGui::Text("Source: %s", runtimeSourceName(binding.Source));
            if (!sourceSignals.empty()) ImGui::Text("Signal: %s", sourceSignals[static_cast<std::size_t>(signalIndex)].data());
            if (binding.Source == RuntimeSourceKind::BindingValue)
            {
                const RuntimeBinding* sourceBinding = engine.findBinding(binding.ValueBindingId);
                ImGui::Text("From binding: %s (#%llu)", sourceBinding ? sourceBinding->Name : "<missing>", static_cast<unsigned long long>(binding.ValueBindingId));
            }
            else if (binding.Source == RuntimeSourceKind::BindingStatus)
            {
                const RuntimeBinding* sourceBinding = engine.findBinding(binding.StatusBindingId);
                ImGui::Text("Status of: %s (#%llu)", sourceBinding ? sourceBinding->Name : "<missing>", static_cast<unsigned long long>(binding.StatusBindingId));
            }
            else if (binding.Source == RuntimeSourceKind::ObjectField || binding.Source == RuntimeSourceKind::ObjectStatus)
            {
                const RuntimeObjectPointer* pointer = engine.findPointer(binding.ObjectPointerId);
                const RuntimeObjectDescriptor* model = pointer ? engine.findObject(pointer->DescriptorId) : engine.findObject(binding.ObjectId);
                ImGui::Text("Pointer instance: %s (#%llu)", pointer ? pointer->Name : "<missing>", static_cast<unsigned long long>(binding.ObjectPointerId));
                ImGui::Text("Descriptor model: %s", model ? model->Name : "<missing>");
                if (binding.Source == RuntimeSourceKind::ObjectField && model)
                {
                    const RuntimeObjectField* field = engine.findObjectField(*model, binding.ObjectFieldId);
                    ImGui::Text("Field: %s (#%llu)", field ? field->Name : "<missing>", static_cast<unsigned long long>(binding.ObjectFieldId));
                }
            }
            else if (binding.Source == RuntimeSourceKind::ValueBank)
            {
                const RuntimeValueBankEntry* bank = engine.findBankValue(binding.BankValueId);
                ImGui::Text("Bank value: %s (#%llu)", bank ? bank->Name : "<missing>", static_cast<unsigned long long>(binding.BankValueId));
            }
            else if (binding.Source == RuntimeSourceKind::Aggregate || binding.Source == RuntimeSourceKind::MassCompare)
            {
                ImGui::Text("Members: %zu", binding.References.size());
                for (const auto& reference : binding.References)
                {
                    if (!reference.Enabled) continue;
                    if (reference.Kind == RuntimeReferenceKind::Binding) { const RuntimeBinding* member = engine.findBinding(reference.Id); ImGui::BulletText("binding %s (#%llu)", member ? member->Name : "<missing>", static_cast<unsigned long long>(reference.Id)); }
                    else { const RuntimeControlRule* member = engine.findControl(reference.Id); ImGui::BulletText("control %s (#%llu)", member ? member->Name : "<missing>", static_cast<unsigned long long>(reference.Id)); }
                }
            }
            if (binding.HasString) ImGui::TextWrapped("String value: %s", binding.StringValue.c_str());
            if (binding.HasValue) ImGui::Text("Raw %.9g  ->  value %.9g", binding.RawValue, binding.Value);
            else ImGui::TextDisabled("No readable value yet.");
            ImGui::TreePop();
        }

        if (binding.HasAddress)
        {
            if (ImGui::TreeNode("Address origin"))
            {
                ImGui::Text("Current exact address: 0x%llX", static_cast<unsigned long long>(binding.AddressValue));
                if (binding.AddressProvenance.empty()) ImGui::TextDisabled("No provenance trace recorded yet.");
                for (std::size_t i = 0; i < binding.AddressProvenance.size(); ++i)
                {
                    ImGui::PushID(static_cast<int>(i)); const bool leaf = i + 1 == binding.AddressProvenance.size();
                    ImGui::TreeNodeEx("##origin", ImGuiTreeNodeFlags_DefaultOpen | (leaf ? ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen : ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen), "%s", binding.AddressProvenance[i].c_str()); ImGui::PopID();
                }
                ImGui::TreePop();
            }
        }

        ImGui::SeparatorText("Material binding");
        changed |= ImGui::Checkbox("Write value to shader material", &binding.WriteMaterial);
        if (binding.WriteMaterial) changed |= drawRuntimeTargetSelector(binding, shaderFramebuffer);
        else ImGui::TextDisabled("Value-only binding: still available to links, bank values, status sources, controls and actions.");

        if (ImGui::BeginTabBar("BindingOptions", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyResizeDown))
        {
            if (ImGui::BeginTabItem("Output / actions"))
            {
                changed |= ImGui::Checkbox("Store successful value in bank", &binding.StoreToBank);
                if (binding.StoreToBank) changed |= drawRuntimeBankReferenceCombo(engine, "Bank destination", binding.StoreBankValueId);
                ImGui::SeparatorText("Actions");
                ImGui::TextDisabled("Bindings can fan out to multiple side effects on update/change/truthy/falsy without creating helper controls.");
                changed |= drawRuntimeActionList(engine, shaderFramebuffer, binding.Actions, binding.Id, false);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Advanced transform"))
            {
                ImGui::TextDisabled("Transform controls can use local values or another binding. This tab stays out of the way for normal graph editing.");
                changed |= drawRuntimeLinkedBool(engine, binding, RuntimeParameterSlot::Normalize, "Normalize input", binding.Normalize);
                if (binding.Normalize || binding.ParameterLinks[static_cast<std::size_t>(RuntimeParameterSlot::Normalize)].Enabled)
                {
                    changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::InputMin, "Input min", binding.InputMin);
                    changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::InputMax, "Input max", binding.InputMax);
                }
                changed |= drawRuntimeLinkedBool(engine, binding, RuntimeParameterSlot::Invert, "Invert", binding.Invert);
                changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::Scale, "Scale", binding.Scale);
                changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::Offset, "Offset", binding.Offset);
                changed |= drawRuntimeLinkedBool(engine, binding, RuntimeParameterSlot::Clamp, "Clamp output", binding.Clamp);
                if (binding.Clamp || binding.ParameterLinks[static_cast<std::size_t>(RuntimeParameterSlot::Clamp)].Enabled)
                {
                    changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::OutputMin, "Output min", binding.OutputMin);
                    changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::OutputMax, "Output max", binding.OutputMax);
                }
                changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::SmoothingHz, "Smoothing", binding.SmoothingHz, 0.1f, "%.1f Hz");
                changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::UpdateHz, "Update rate", binding.UpdateHz, 0.5f, "%.1f Hz");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        if (binding.HasAddress) ImGui::TextDisabled("Exact address: 0x%llX", static_cast<unsigned long long>(binding.AddressValue));
        ImGui::Unindent(10.0f);
        ImGui::Dummy(ImVec2(0.0f, 5.0f));
        ImGui::Separator();

        if (changed) engine.markChanged();
        ImGui::PopID();
        ImGui::PopID();
        return changed;
    }
