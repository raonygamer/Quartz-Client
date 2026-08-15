
    static bool drawProfileHotkey(RuntimeBindingProfile& profile)
    {
        struct KeyOption { const char* Name; int Key; };
        static constexpr KeyOption Keys[] = {
            {"None", 0}, {"F1", GLFW_KEY_F1}, {"F2", GLFW_KEY_F2}, {"F3", GLFW_KEY_F3}, {"F4", GLFW_KEY_F4}, {"F5", GLFW_KEY_F5}, {"F6", GLFW_KEY_F6},
            {"F7", GLFW_KEY_F7}, {"F8", GLFW_KEY_F8}, {"F9", GLFW_KEY_F9}, {"F10", GLFW_KEY_F10}, {"F11", GLFW_KEY_F11}, {"F12", GLFW_KEY_F12},
            {"1", GLFW_KEY_1}, {"2", GLFW_KEY_2}, {"3", GLFW_KEY_3}, {"4", GLFW_KEY_4}, {"5", GLFW_KEY_5}, {"6", GLFW_KEY_6}, {"7", GLFW_KEY_7}, {"8", GLFW_KEY_8}, {"9", GLFW_KEY_9}, {"0", GLFW_KEY_0},
            {"A", GLFW_KEY_A}, {"B", GLFW_KEY_B}, {"C", GLFW_KEY_C}, {"D", GLFW_KEY_D}, {"E", GLFW_KEY_E}, {"F", GLFW_KEY_F}, {"G", GLFW_KEY_G}, {"H", GLFW_KEY_H}, {"I", GLFW_KEY_I}, {"J", GLFW_KEY_J},
            {"K", GLFW_KEY_K}, {"L", GLFW_KEY_L}, {"M", GLFW_KEY_M}, {"N", GLFW_KEY_N}, {"O", GLFW_KEY_O}, {"P", GLFW_KEY_P}, {"Q", GLFW_KEY_Q}, {"R", GLFW_KEY_R}, {"S", GLFW_KEY_S}, {"T", GLFW_KEY_T},
            {"U", GLFW_KEY_U}, {"V", GLFW_KEY_V}, {"W", GLFW_KEY_W}, {"X", GLFW_KEY_X}, {"Y", GLFW_KEY_Y}, {"Z", GLFW_KEY_Z}
        };
        bool changed = false;
        changed |= ImGui::Checkbox("Ctrl", &profile.HotkeyCtrl); ImGui::SameLine(); changed |= ImGui::Checkbox("Alt", &profile.HotkeyAlt); ImGui::SameLine(); changed |= ImGui::Checkbox("Shift", &profile.HotkeyShift); ImGui::SameLine();
        const char* preview = "None"; for (const auto& key : Keys) if (key.Key == profile.HotkeyKey) { preview = key.Name; break; }
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::BeginCombo("Key", preview))
        {
            for (const auto& key : Keys) { const bool selected = key.Key == profile.HotkeyKey; if (ImGui::Selectable(key.Name, selected)) { profile.HotkeyKey = key.Key; changed = true; } if (selected) ImGui::SetItemDefaultFocus(); }
            ImGui::EndCombo();
        }
        return changed;
    }

    static void drawRuntimeValueBank(RuntimeBindingEngine& engine)
    {
        ImGui::TextUnformatted("Value bank"); ImGui::SameLine(); if (ImGui::Button("+ Add value")) engine.addBankValue();
        ImGui::SameLine(); ImGui::TextDisabled("Persistent scratch values survive source loss/restarts and can hold numbers, booleans, strings, or exact addresses.");
        std::optional<std::size_t> erase;
        if (ImGui::BeginTable("RuntimeValueBank", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 48.0f); ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110.0f); ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Description"); ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, 55.0f); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 34.0f); ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < engine.bank().size(); ++i)
            {
                auto& value = engine.bank()[i]; bool changed = false, valueChanged = false; ImGui::PushID(static_cast<int>(value.Id & 0x7fffffffULL)); ImGui::TableNextRow();
                ImGui::TableNextColumn(); changed |= ImGui::Checkbox("##enabled", &value.Enabled); ImGui::SameLine(); const bool bankGood = value.HasValue && (value.Type == RuntimeBankValueType::Boolean ? value.Boolean : value.Type == RuntimeBankValueType::Address ? value.Address != 0 : value.Type == RuntimeBankValueType::String ? value.String[0] != '\0' : true); drawRuntimeStateSquare("##bankState", runtimeStateColor(value.Enabled, bankGood, false, !value.HasValue));
                ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f); changed |= ImGui::InputText("##name", value.Name, sizeof(value.Name));
                ImGui::TableNextColumn(); int type = static_cast<int>(value.Type); ImGui::SetNextItemWidth(-1.0f); if (ImGui::Combo("##type", &type, "Number\0Integer\0Boolean\0String\0Address\0")) { value.Type = static_cast<RuntimeBankValueType>(type); changed = valueChanged = true; }
                ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f);
                if (value.Type == RuntimeBankValueType::Number) { if (ImGui::DragFloat("##number", &value.Number, 0.01f)) changed = valueChanged = true; }
                else if (value.Type == RuntimeBankValueType::Integer) { long long temp = static_cast<long long>(value.Integer); if (ImGui::InputScalar("##integer", ImGuiDataType_S64, &temp)) { value.Integer = static_cast<std::int64_t>(temp); changed = valueChanged = true; } }
                else if (value.Type == RuntimeBankValueType::Boolean) { if (ImGui::Checkbox("##boolean", &value.Boolean)) changed = valueChanged = true; }
                else if (value.Type == RuntimeBankValueType::String) { if (ImGui::InputText("##string", value.String, sizeof(value.String))) changed = valueChanged = true; }
                else { unsigned long long address = static_cast<unsigned long long>(value.Address); if (ImGui::InputScalar("##address", ImGuiDataType_U64, &address, nullptr, nullptr, "0x%llX", ImGuiInputTextFlags_CharsHexadecimal)) { value.Address = static_cast<std::uintptr_t>(address); changed = valueChanged = true; } }
                ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f); changed |= ImGui::InputText("##description", value.Description, sizeof(value.Description));
                ImGui::TableNextColumn(); ImGui::Text("#%llu", static_cast<unsigned long long>(value.Id));
                ImGui::TableNextColumn(); if (ImGui::SmallButton("x")) erase = i;
                if (valueChanged) { value.HasValue = true; value.ChangedThisFrame = true; }
                if (changed) engine.markChanged();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (erase) engine.eraseBankValue(*erase);
        if (engine.bank().empty()) ImGui::TextDisabled("No bank values yet. Add one to remember state such as the shader that was active before a game took over.");
    }

    static void drawRuntimeProfiles(RuntimeBindingEngine& engine)
    {
        ImGui::TextUnformatted("Binding profiles"); ImGui::SameLine(); if (ImGui::Button("+ Add profile")) engine.addProfile();
        ImGui::SameLine(); ImGui::TextDisabled("Profiles mass-enable/disable graph nodes and can be activated with a key combination while Quartz has keyboard focus.");
        if (!engine.profiles().empty())
        {
            const RuntimeBindingProfile* active = engine.findProfile(engine.activeProfileId());
            ImGui::SetNextItemWidth(280.0f);
            if (ImGui::BeginCombo("Active profile", active ? active->Name : "<manual / none>"))
            {
                if (ImGui::Selectable("<manual / none>", engine.activeProfileId() == 0)) engine.clearActiveProfile();
                for (auto& profile : engine.profiles()) { const bool selected = profile.Id == engine.activeProfileId(); if (ImGui::Selectable(profile.Name, selected)) engine.applyProfile(profile); if (selected) ImGui::SetItemDefaultFocus(); }
                ImGui::EndCombo();
            }
        }
        std::optional<std::size_t> erase;
        for (std::size_t i = 0; i < engine.profiles().size(); ++i)
        {
            auto& profile = engine.profiles()[i]; ImGui::PushID(static_cast<int>(profile.Id & 0x7fffffffULL));
            drawRuntimeStateSquare("##profileState", runtimeStateColor(profile.Enabled, profile.Id == engine.activeProfileId())); ImGui::SameLine();
            const std::string header = std::string(profile.Name) + (profile.Id == engine.activeProfileId() ? "  ACTIVE" : "") + "###Profile" + std::to_string(profile.Id);
            if (ImGui::CollapsingHeader(header.c_str()))
            {
                bool changed = false; changed |= ImGui::Checkbox("Enabled", &profile.Enabled); ImGui::SameLine(); ImGui::SetNextItemWidth(220.0f); changed |= ImGui::InputText("Name", profile.Name, sizeof(profile.Name));
                changed |= ImGui::Checkbox("Exclusive activation", &profile.Exclusive); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Exclusive profiles disable every binding/control first, then enable their selected members.");
                if (ImGui::Button("Activate")) engine.applyProfile(profile); ImGui::SameLine(); if (ImGui::Button("Enable members")) engine.setProfileMembersEnabled(profile, true); ImGui::SameLine(); if (ImGui::Button("Disable members")) engine.setProfileMembersEnabled(profile, false); ImGui::SameLine(); if (ImGui::Button("Remove profile")) erase = i;
                ImGui::SeparatorText("Hotkey"); changed |= drawProfileHotkey(profile);
                ImGui::SeparatorText("Bindings");
                if (ImGui::BeginTable("ProfileBindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable))
                {
                    ImGui::TableSetupColumn("Member", ImGuiTableColumnFlags_WidthFixed, 65.0f); ImGui::TableSetupColumn("Binding"); ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthFixed, 80.0f); ImGui::TableHeadersRow();
                    for (const auto& binding : engine.bindings()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); bool member = std::find(profile.BindingIds.begin(), profile.BindingIds.end(), binding.Id) != profile.BindingIds.end(); if (ImGui::Checkbox(("##pb" + std::to_string(binding.Id)).c_str(), &member)) { if (member) profile.BindingIds.push_back(binding.Id); else std::erase(profile.BindingIds, binding.Id); changed = true; } ImGui::TableNextColumn(); ImGui::TextUnformatted(binding.Name); ImGui::TableNextColumn(); ImGui::TextColored(binding.Enabled ? ImVec4(0.2f,0.8f,0.3f,1) : ImVec4(0.8f,0.25f,0.25f,1), "%s", binding.Enabled ? "enabled" : "disabled"); }
                    ImGui::EndTable();
                }
                ImGui::SeparatorText("Controls");
                if (ImGui::BeginTable("ProfileControls", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable))
                {
                    ImGui::TableSetupColumn("Member", ImGuiTableColumnFlags_WidthFixed, 65.0f); ImGui::TableSetupColumn("Control"); ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthFixed, 80.0f); ImGui::TableHeadersRow();
                    for (const auto& control : engine.controls()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); bool member = std::find(profile.ControlIds.begin(), profile.ControlIds.end(), control.Id) != profile.ControlIds.end(); if (ImGui::Checkbox(("##pc" + std::to_string(control.Id)).c_str(), &member)) { if (member) profile.ControlIds.push_back(control.Id); else std::erase(profile.ControlIds, control.Id); changed = true; } ImGui::TableNextColumn(); ImGui::TextUnformatted(control.Name); ImGui::TableNextColumn(); ImGui::TextColored(control.Enabled ? ImVec4(0.2f,0.8f,0.3f,1) : ImVec4(0.8f,0.25f,0.25f,1), "%s", control.Enabled ? "enabled" : "disabled"); }
                    ImGui::EndTable();
                }
                if (changed) engine.markChanged();
            }
            ImGui::Separator(); ImGui::PopID();
        }
        if (erase) engine.eraseProfile(*erase);
    }

    static void drawGroupedBindings(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer)
    {
        std::vector<RuntimeBinding*> order; for (auto& binding : engine.bindings()) order.push_back(&binding); runtimeSortUiNodes(order); std::set<std::string> groups; for (auto* binding : order) if (binding->Group[0]) groups.insert(binding->Group);
        std::optional<std::size_t> erase;
        auto drawOne = [&](RuntimeBinding& binding) { bool shouldErase = false; drawRuntimeBinding(engine, shaderFramebuffer, binding, shouldErase); if (shouldErase) erase = static_cast<std::size_t>(&binding - engine.bindings().data()); };
        for (auto* binding : order) if (!binding->Group[0]) drawOne(*binding);
        for (const auto& group : groups) if (ImGui::CollapsingHeader((group + "###BindingGroup" + group).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) { ImGui::Indent(8.0f); for (auto* binding : order) if (group == binding->Group) drawOne(*binding); ImGui::Unindent(8.0f); }
        if (erase) engine.erase(*erase);
    }

    static void drawGroupedControls(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer)
    {
        std::vector<RuntimeControlRule*> order; for (auto& control : engine.controls()) order.push_back(&control); runtimeSortUiNodes(order); std::set<std::string> groups; for (auto* control : order) if (control->Group[0]) groups.insert(control->Group);
        std::optional<std::size_t> erase;
        auto drawOne = [&](RuntimeControlRule& control) { bool shouldErase = false; drawRuntimeControlRule(engine, shaderFramebuffer, control, shouldErase); if (shouldErase) erase = static_cast<std::size_t>(&control - engine.controls().data()); };
        for (auto* control : order) if (!control->Group[0]) drawOne(*control);
        for (const auto& group : groups) if (ImGui::CollapsingHeader((group + "###ControlGroup" + group).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) { ImGui::Indent(8.0f); for (auto* control : order) if (group == control->Group) drawOne(*control); ImGui::Unindent(8.0f); }
        if (erase) engine.eraseControl(*erase);
    }

    static void drawRuntimeBindingsPage(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer)
    {
        ImGui::TextWrapped("Runtime bindings are a priority-ordered dataflow graph. Native/object sources, aggregate/comparison nodes, the value bank and controls can all feed each other; control iteration is bounded to prevent runaway loops.");
        int passes = engine.controlPassLimit(); ImGui::SetNextItemWidth(110.0f); if (ImGui::InputInt("Control passes", &passes)) engine.setControlPassLimit(passes); ImGui::SameLine(); ImGui::TextDisabled("max iterative passes/frame");

        constexpr ImGuiTabBarFlags RuntimeTabs = ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyResizeDown | ImGuiTabBarFlags_TabListPopupButton;
        if (ImGui::BeginTabBar("RuntimeGraphTabs", RuntimeTabs))
        {
            if (ImGui::BeginTabItem("Bindings"))
            {
                ImGui::TextUnformatted("Bindings"); ImGui::SameLine(); if (ImGui::Button("+ Add binding")) engine.add(); ImGui::SameLine(); if (ImGui::Button("Save")) engine.save(); ImGui::SameLine(); ImGui::TextDisabled("%s", engine.path().string().c_str());
                if (ImGui::CollapsingHeader("Quick bindings"))
                {
                    auto addPreset = [&](const char* name, const RuntimeSourceKind source, const int signal, const char* target)
                    {
                        auto& binding = engine.add(); std::snprintf(binding.Name, sizeof(binding.Name), "%s", name); binding.Source = source; binding.Signal = signal; std::snprintf(binding.TargetId, sizeof(binding.TargetId), "%s", target); binding.Clamp = true; binding.OutputMin = 0.0f; binding.OutputMax = 1.0f; binding.SmoothingHz = 8.0f; binding.UpdateHz = source == RuntimeSourceKind::NativeProcess ? 20.0f : 60.0f; return &binding;
                    };
                    if (ImGui::SmallButton("Native value")) addPreset("Native process value", RuntimeSourceKind::NativeProcess, 0, "runtime.native"); ImGui::SameLine();
                    if (ImGui::SmallButton("Native address")) { auto* b = addPreset("Native object address", RuntimeSourceKind::NativeAddress, 1, "runtime.address"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; } ImGui::SameLine();
                    if (ImGui::SmallButton("Writable")) { auto* b = addPreset("Writable state", RuntimeSourceKind::Unbound, 0, "runtime.state"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; } ImGui::SameLine();
                    if (ImGui::SmallButton("Current shader")) { auto* b = addPreset("Current shader", RuntimeSourceKind::ShaderState, 0, "runtime.shader"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; } ImGui::SameLine();
                    if (ImGui::SmallButton("Previous shader")) { auto* b = addPreset("Previous shader", RuntimeSourceKind::ShaderState, 9, "runtime.previous_shader"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; } ImGui::SameLine();
                    if (ImGui::SmallButton("Active profile")) { auto* b = addPreset("Active profile", RuntimeSourceKind::ProfileState, 0, "runtime.profile"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; } ImGui::SameLine();
                    if (ImGui::SmallButton("Audio RMS")) { auto* b = addPreset("Audio RMS", RuntimeSourceKind::Audio, 0, "runtime.audio"); b->WriteMaterial = false; } ImGui::SameLine();
                    if (ImGui::SmallButton("Media title")) { auto* b = addPreset("Media title", RuntimeSourceKind::Media, 5, "runtime.media_title"); b->WriteMaterial = false; b->Clamp = false; b->SmoothingHz = 0.0f; }
                }
                if (shaderFramebuffer.materialParameters().empty()) ImGui::TextDisabled("Current shader has no reflected material parameters.");
                drawGroupedBindings(engine, shaderFramebuffer);
                if (engine.bindings().empty()) ImGui::TextDisabled("No bindings yet.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Controls"))
            {
                ImGui::TextUnformatted("Controls"); ImGui::SameLine(); if (ImGui::Button("+ Add control")) engine.addControl(); ImGui::SameLine(); ImGui::TextDisabled("green/red squares show the current boolean state; higher priority wins output conflicts");
                drawGroupedControls(engine, shaderFramebuffer);
                if (engine.controls().empty()) ImGui::TextDisabled("No control rules yet.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Objects")) { drawRuntimeObjectDescriptors(engine); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Pointers")) { drawRuntimePointers(engine); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Value Bank")) { drawRuntimeValueBank(engine); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Profiles")) { drawRuntimeProfiles(engine); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }

    static void drawQRPCInspectorPage(RuntimeTelemetry& telemetry)
    {
        ImGui::SeparatorText("QRPC packet inspector");
        static int selectedNewest = 0;
        if (ImGui::Button("Clear packets")) { telemetry.clearPackets(); selectedNewest = 0; }
        ImGui::SameLine();
        ImGui::TextDisabled("Assembled QRPC packets, not raw libusb transfer boundaries. Click a packet for a captured hex view.");
        const auto packets = telemetry.packets();
        if (!packets.empty()) selectedNewest = std::clamp(selectedNewest, 0, static_cast<int>(packets.size()) - 1);
        if (ImGui::BeginTable("QRPCPackets", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 330.0f)))
        {
            ImGui::TableSetupColumn("t");
            ImGui::TableSetupColumn("Dir");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Packet id");
            ImGui::TableSetupColumn("Response for");
            ImGui::TableSetupColumn("Payload");
            ImGui::TableSetupColumn("Packet");
            ImGui::TableSetupColumn("Version");
            ImGui::TableHeadersRow();
            int newestIndex = 0;
            for (auto it = packets.rbegin(); it != packets.rend(); ++it, ++newestIndex)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(newestIndex);
                const bool selected = selectedNewest == newestIndex;
                if (ImGui::Selectable("##packet", selected, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, ImGui::GetTextLineHeight())))
                    selectedNewest = newestIndex;
                ImGui::SameLine(); ImGui::Text("%.3f", it->Time);
                ImGui::PopID();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->Tx ? "Host -> Device" : "Device -> Host");
                ImGui::TableNextColumn(); ImGui::Text("%s (0x%04X)", packetTypeLabel(it->Type), it->Type);
                ImGui::TableNextColumn(); ImGui::Text("%u", it->PacketId);
                ImGui::TableNextColumn(); ImGui::Text("%u", it->ResponseFor);
                ImGui::TableNextColumn(); ImGui::Text("%u B", it->PayloadLength);
                ImGui::TableNextColumn(); ImGui::Text("%zu B", it->Bytes);
                ImGui::TableNextColumn(); ImGui::Text("%u", it->Version);
            }
            ImGui::EndTable();
        }
        ImGui::Text("Buffered packets: %zu / 512", packets.size());
        if (!packets.empty())
        {
            const auto& packet = packets[packets.size() - 1 - static_cast<std::size_t>(selectedNewest)];
            ImGui::SeparatorText("Packet bytes");
            ImGui::Text("%s %s   packet %u   %zu/%zu B captured", packet.Tx ? "TX" : "RX", packetTypeLabel(packet.Type), packet.PacketId, packet.CapturedBytes, packet.Bytes);
            if (ImGui::BeginChild("##PacketHex", ImVec2(0.0f, 160.0f), true, ImGuiWindowFlags_HorizontalScrollbar))
            {
                for (std::size_t offset = 0; offset < packet.CapturedBytes; offset += 16)
                {
                    char line[128]{};
                    int written = std::snprintf(line, sizeof(line), "%04zX  ", offset);
                    for (std::size_t i = 0; i < 16; ++i)
                    {
                        if (offset + i < packet.CapturedBytes)
                            written += std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written), "%02X ", std::to_integer<unsigned>(packet.Data[offset + i]));
                        else
                            written += std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written), "   ");
                    }
                    written += std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written), " |");
                    for (std::size_t i = 0; i < 16 && offset + i < packet.CapturedBytes; ++i)
                    {
                        const unsigned byte = std::to_integer<unsigned>(packet.Data[offset + i]);
                        if (written + 2 < static_cast<int>(sizeof(line))) line[written++] = byte >= 32 && byte < 127 ? static_cast<char>(byte) : '.';
                    }
                    if (written + 2 < static_cast<int>(sizeof(line))) { line[written++] = '|'; line[written] = '\0'; }
                    ImGui::TextUnformatted(line);
                }
                ImGui::EndChild();
            }
        }
    }

    static void drawUSBProfilerPage(RawUSB& usb, const RuntimeBindingEngine& bindings)
    {
        const auto stats = usb.stats();
        const auto& rates = bindings.usbRates();
        ImGui::SeparatorText("USB transport");
        ImGui::Text("Device: %04X:%04X   interface %d   OUT 0x%02X   IN 0x%02X", VendorId, ProductId, RPCInterfaceNumber, RPCOutEndpoint, RPCInEndpoint);
        ImGui::Text("State: %s   product: %s", usb.isConnected() ? "connected" : "disconnected", usb.deviceName().empty() ? "Quartz K552X" : usb.deviceName().c_str());
        ImGui::Text("TX %.2f KiB/s  %.1f transfers/s   RX %.2f KiB/s  %.1f transfers/s", rates.TxKiB, rates.TxTransfers, rates.RxKiB, rates.RxTransfers);
        ImGui::Text("TX total %.2f MiB / %llu transfers   RX total %.2f MiB / %llu transfers", stats.TxBytes / (1024.0 * 1024.0), static_cast<unsigned long long>(stats.TxTransfers), stats.RxBytes / (1024.0 * 1024.0), static_cast<unsigned long long>(stats.RxTransfers));
        ImGui::Text("Transfer latency: TX %.3f ms   RX %.3f ms", stats.LastTxMilliseconds, stats.LastRxMilliseconds);
        ImGui::Text("Errors: TX %llu   RX %llu   connects %llu   disconnects %llu", static_cast<unsigned long long>(stats.TxErrors), static_cast<unsigned long long>(stats.RxErrors), static_cast<unsigned long long>(stats.Connects), static_cast<unsigned long long>(stats.Disconnects));
        if (usb.lastError() != LIBUSB_SUCCESS) ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.48f, 1.0f), "Last libusb status: %s", libusb_error_name(usb.lastError()));
    }

    static void drawInputAnalyzerPage(const EvdevKeyboard& keyboard, const ReactiveKeyState& keys, const RuntimeInputAnalytics& analytics)
    {
        const float held = std::accumulate(keys.Down.begin(), keys.Down.end(), 0.0f);
        ImGui::SeparatorText("Global keyboard input");
        ImGui::Text("evdev: %s", keyboard.connected() ? "connected" : "disconnected");
        ImGui::TextWrapped("%s", keyboard.status().c_str());
        ImGui::Text("Held mapped keys: %.0f   total presses: %llu   longest press: %.3f s   Caps %s   Scroll %s", held, static_cast<unsigned long long>(analytics.TotalPresses), analytics.LongestPress, keys.CapsLockActive ? "ON" : "off", keys.ScrollLockActive ? "ON" : "off");

        std::array<Color32, MatrixSize> stateFramebuffer{};
        for (std::size_t i = 0; i < MatrixSize; ++i)
            if (keys.Down[i] > 0.5f) stateFramebuffer[i] = {220, 220, 220};
        ImGui::TextUnformatted("Live state");
        drawFramebufferPreview(stateFramebuffer, 0.55f, 532.0f, 0.12f);

        const auto maxPressIt = std::max_element(analytics.PressCount.begin(), analytics.PressCount.end());
        const std::uint64_t maxPresses = maxPressIt != analytics.PressCount.end() ? *maxPressIt : 0;
        std::array<Color32, MatrixSize> heatmap{};
        if (maxPresses != 0)
        {
            for (std::size_t i = 0; i < MatrixSize; ++i)
            {
                const float amount = std::sqrt(static_cast<float>(analytics.PressCount[i]) / static_cast<float>(maxPresses));
                const auto value = static_cast<std::uint8_t>(std::lround(amount * 255.0f));
                heatmap[i] = {value, value, value};
            }
        }
        ImGui::Text("Press-count heatmap   max/key %llu", static_cast<unsigned long long>(maxPresses));
        drawFramebufferPreview(heatmap, 0.55f, 532.0f, 0.08f);

        ImGui::SeparatorText("Recent mapped key events");
        if (ImGui::BeginTable("RecentKeyEvents", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 230.0f)))
        {
            ImGui::TableSetupColumn("Slot");
            ImGui::TableSetupColumn("Row");
            ImGui::TableSetupColumn("Column");
            ImGui::TableSetupColumn("Age");
            ImGui::TableSetupColumn("Presses");
            ImGui::TableSetupColumn("Held / last");
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < keys.Events.size(); ++i)
            {
                const auto& event = keys.Events[i];
                if (event.Valid < 0.5f) continue;
                const std::size_t row = static_cast<std::size_t>(std::clamp(event.Row, 0.0f, static_cast<float>(Rows - 1)));
                const std::size_t column = static_cast<std::size_t>(std::clamp(event.Column, 0.0f, static_cast<float>(Columns - 1)));
                const std::size_t index = row * Columns + column;
                const double heldFor = analytics.heldDuration(index, ImGui::GetTime());
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%zu", i);
                ImGui::TableNextColumn(); ImGui::Text("%zu", row);
                ImGui::TableNextColumn(); ImGui::Text("%zu", column);
                ImGui::TableNextColumn(); ImGui::Text("%.3f s", std::max(static_cast<float>(ImGui::GetTime()) - event.Time, 0.0f));
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(analytics.PressCount[index]));
                ImGui::TableNextColumn(); ImGui::Text("%.3f s", heldFor > 0.0 ? heldFor : analytics.LastDuration[index]);
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Ctrl+Alt+Shift+Q remains the global recovery shortcut when the Quartz window is hidden.");
    }

    static void drawRGBProfilerPage(const std::array<Color32, MatrixSize>& framebuffer, const VisualizerSettings& settings, const RuntimeRGBAnalytics& analytics)
    {
        std::array<float, ActiveProbeRows> rowLuma{};
        float r = 0.0f, g = 0.0f, b = 0.0f, lit = 0.0f, peak = 0.0f;
        for (std::size_t row = 0; row < ActiveProbeRows; ++row)
        {
            for (std::size_t column = 0; column < Columns; ++column)
            {
                const auto& color = framebuffer[row * Columns + column];
                const float rf = color.R / 255.0f, gf = color.G / 255.0f, bf = color.B / 255.0f;
                const float luma = rf * 0.2126f + gf * 0.7152f + bf * 0.0722f;
                r += rf; g += gf; b += bf; rowLuma[row] += luma; peak = std::max(peak, std::max({rf, gf, bf}));
                if (color.R || color.G || color.B) lit += 1.0f;
            }
            rowLuma[row] /= static_cast<float>(Columns);
        }
        constexpr float Count = static_cast<float>(ActiveProbeRows * Columns);
        r /= Count; g /= Count; b /= Count;
        ImGui::SeparatorText("Framebuffer / RGB profiler");
        ImGui::Text("Output %.1f Hz   changed this frame %zu / %.0f   EMA changed %.1f cells/frame", analytics.FrameRate, analytics.ChangedCells, Count, analytics.AverageChangedCells);
        ImGui::Text("Active cells: %.0f / %.0f (%.1f%%)   peak channel %.1f%%", lit, Count, lit / Count * 100.0f, peak * 100.0f);
        ImGui::Text("Average RGB: %.1f%% / %.1f%% / %.1f%%   luma %.1f%%", r * 100.0f, g * 100.0f, b * 100.0f, (r * 0.2126f + g * 0.7152f + b * 0.0722f) * 100.0f);
        ImGui::Text("Frames profiled %llu   total cell changes %llu   Global brightness %.0f%%   preview interpolation %.0f%%", static_cast<unsigned long long>(analytics.Frames), static_cast<unsigned long long>(analytics.ChangedCellsTotal), settings.GlobalBrightness * 100.0f, settings.LiveOutputInterpolation * 100.0f);
        ImGui::PlotHistogram("Luma distribution", analytics.LumaHistogram.data(), static_cast<int>(analytics.LumaHistogram.size()), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 90.0f));
        ImGui::PlotHistogram("Per-row luma", rowLuma.data(), static_cast<int>(rowLuma.size()), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 100.0f));
        drawFramebufferPreview(framebuffer, 0.55f, 532.0f, settings.LiveOutputInterpolation);
    }

    static void drawAudioLabPage(AudioSpectrum& audio, const AudioLevelSnapshot& level, const AutoGainState& autoGain, VisualizerSettings& settings, const std::array<float, FFTSize>& analysisBands, const std::array<float, Columns>& mappedBands, const std::array<float, Columns>& smoothedBands)
    {
        static std::array<float, 300> rmsHistory{};
        static std::array<float, 300> peakHistory{};
        static double nextHistory = 0.0;
        if (ImGui::GetTime() >= nextHistory)
        {
            std::move(rmsHistory.begin() + 1, rmsHistory.end(), rmsHistory.begin());
            std::move(peakHistory.begin() + 1, peakHistory.end(), peakHistory.begin());
            rmsHistory.back() = level.Rms;
            peakHistory.back() = level.Peak;
            nextHistory = ImGui::GetTime() + 1.0 / 30.0;
        }

        ImGui::SeparatorText("Audio level / normalization");
        ImGui::Text("Capture: %s", audio.source().c_str());
        ImGui::Text("RMS %.5f   peak %.5f", level.Rms, level.Peak);
        ImGui::PlotLines("RMS history", rmsHistory.data(), static_cast<int>(rmsHistory.size()), 0, nullptr, 0.0f, std::max(0.25f, *std::max_element(peakHistory.begin(), peakHistory.end())), ImVec2(-1.0f, 100.0f));
        ImGui::PlotLines("Peak history", peakHistory.data(), static_cast<int>(peakHistory.size()), 0, nullptr, 0.0f, std::max(0.25f, *std::max_element(peakHistory.begin(), peakHistory.end())), ImVec2(-1.0f, 70.0f));

        ImGui::Checkbox("Enable automatic overall gain", &settings.AutomaticOverallGain);
        if (settings.AutomaticOverallGain)
        {
            ImGui::SliderFloat("Baseline gain", &settings.AutoGainBaseline, 0.10f, 6.0f, "%.2fx");
            ImGui::SliderFloat("Target long-term RMS", &settings.AutoGainTargetRms, 0.01f, 0.50f, "%.3f");
            ImGui::SliderFloat("Adaptation speed", &settings.AutoGainAdaptation, 0.02f, 2.0f, "%.2f /s", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Minimum correction", &settings.AutoGainMinCorrection, 0.10f, 1.0f, "%.2fx");
            ImGui::SliderFloat("Maximum correction", &settings.AutoGainMaxCorrection, 1.0f, 8.0f, "%.2fx");
            ImGui::SliderFloat("Silence gate", &settings.AutoGainSilenceGate, 0.0f, 0.05f, "%.4f");
            ImGui::Text("Learned RMS %.5f   correction %.3fx   effective gain %.3fx", autoGain.LongTermRms, autoGain.Correction, autoGain.EffectiveGain);
            ImGui::TextDisabled("The baseline is the normal gain. Long-term media loudness only applies a slow bounded correction around it; silence is ignored.");
        }
        else
        {
            ImGui::SliderFloat("Manual overall gain", &settings.OverallGain, 0.10f, 4.0f, "%.2fx");
            ImGui::Text("Effective gain %.3fx", autoGain.EffectiveGain);
        }

        ImGui::SeparatorText("Spectrum pipeline");
        ImGui::PlotLines("FFT / log analysis", analysisBands.data(), settings.AnalysisBandCount, 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 120.0f));
        ImGui::PlotHistogram("Mapped", mappedBands.data(), static_cast<int>(Columns), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 90.0f));
        ImGui::PlotHistogram("Attack / release output", smoothedBands.data(), static_cast<int>(Columns), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 90.0f));
    }

    static void drawTimelinePage(RuntimeTelemetry& telemetry)
    {
        if (ImGui::Button("Clear timeline")) telemetry.clearEvents();
        ImGui::SameLine();
        ImGui::TextDisabled("USB/media/input/runtime events");
        const auto events = telemetry.events();
        if (ImGui::BeginTable("RuntimeTimeline", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 470.0f)))
        {
            ImGui::TableSetupColumn("t", ImGuiTableColumnFlags_WidthFixed, 85.0f);
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (auto it = events.rbegin(); it != events.rend(); ++it)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%.3f", it->Time);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->Category.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->Text.c_str());
            }
            ImGui::EndTable();
        }
    }

    static void drawFirmwarePage(const PerformanceSnapshot& performance, const bool hasPerformance, const MatrixTimingProbeResult<ActiveProbeRows>& timingProbe, const bool hasTimingProbe)
    {
        ImGui::SeparatorText("Firmware identity");
        ImGui::Text("Quartz K552X firmware %s", FirmwareVersion);
        ImGui::Text("QRPC protocol v%u   VID:PID %04X:%04X", static_cast<unsigned>(ProtocolVersion), VendorId, ProductId);
        ImGui::Text("RPC interface %d   bulk OUT 0x%02X   bulk IN 0x%02X", RPCInterfaceNumber, RPCOutEndpoint, RPCInEndpoint);
        ImGui::Text("Framebuffer %zux%zu logical / %zux%zu active RGB   Color32 %zu B", Columns, Rows, Columns, ActiveProbeRows, sizeof(Color32));
        ImGui::Text("PacketHeader %zu B   framebuffer payload %zu B", sizeof(PacketHeader), sizeof(FramebufferSetPayload<MatrixSize>));
        ImGui::SeparatorText("Live firmware telemetry");
        if (hasPerformance) drawPerformance(performance); else ImGui::TextDisabled("Waiting for PerformanceResponse...");
        ImGui::SeparatorText("Matrix settle probe");
        if (hasTimingProbe) drawTimingProbe(timingProbe); else ImGui::TextDisabled("No matrix timing probe result received yet.");
    }

    template<typename T>
    static bool defaultButton(const char* id, T& value, const T& defaultValue)
    {
        const bool isDefault = value == defaultValue;
        ImGui::SameLine();
        if (isDefault) ImGui::BeginDisabled();
        char label[96];
        std::snprintf(label, sizeof(label), "Default##%s", id);
        const bool pressed = ImGui::SmallButton(label);
        if (isDefault) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Restore default");
        if (pressed) value = defaultValue;
        return pressed;
    }

    static bool defaultAudioSourceButton(const char* id, VisualizerSettings& settings, const VisualizerSettings& defaults, AudioSpectrum& audio)
    {
        const bool isDefault = std::strcmp(settings.AudioSource, defaults.AudioSource) == 0;
        ImGui::SameLine();
        if (isDefault) ImGui::BeginDisabled();
        char label[96];
        std::snprintf(label, sizeof(label), "Default##%s", id);
        const bool pressed = ImGui::SmallButton(label);
        if (isDefault) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Restore default");
        if (pressed)
        {
            std::snprintf(settings.AudioSource, sizeof(settings.AudioSource), "%s", defaults.AudioSource);
            audio.start(settings.AudioSource);
        }
        return pressed;
    }

    static void applyDarkTheme()
    {
        ImGui::StyleColorsDark();
        auto& style = ImGui::GetStyle();
        for (auto& color : style.Colors)
        {
            const float gray = color.x * 0.2126f + color.y * 0.7152f + color.z * 0.0722f;
            color.x = gray;
            color.y = gray;
            color.z = gray;
        }
    }

    static constexpr float ShaderEditorBaseSize = 14.0f;
    static constexpr std::array<float, 20> ShaderEditorZoomLevels{0.60f, 0.70f, 0.80f, 0.90f, 1.00f, 1.10f, 1.20f, 1.30f, 1.40f, 1.50f, 1.60f, 1.70f, 1.80f, 1.90f, 2.00f, 2.10f, 2.20f, 2.30f, 2.40f, 2.50f};
#if IMGUI_VERSION_NUM >= 19200
    static ImFont* ShaderEditorFont = nullptr;
#else
    static std::array<ImFont*, ShaderEditorZoomLevels.size()> ShaderEditorFonts{};
#endif

    static float shaderEditorPixelSize(const float zoom) noexcept { return std::max(8.0f, std::round(ShaderEditorBaseSize * zoom)); }

    static void initializeShaderEditorFonts()
    {
        ImGuiIO& io = ImGui::GetIO();
#if IMGUI_VERSION_NUM >= 19200
        // ImGui 1.92+ can rasterize the scalable embedded font at the requested size on demand.
        // This avoids stretching one cached bitmap when the editor is zoomed.
        ShaderEditorFont = io.Fonts->AddFontDefaultVector();
#else
        // Older ImGui has fixed-size fonts, so bake one font per zoom level at an integer pixel size.
        for (std::size_t i = 0; i < ShaderEditorZoomLevels.size(); ++i)
        {
            ImFontConfig config{};
            config.SizePixels = shaderEditorPixelSize(ShaderEditorZoomLevels[i]);
            config.OversampleH = 2;
            config.OversampleV = 2;
            config.PixelSnapH = true;
            ShaderEditorFonts[i] = io.Fonts->AddFontDefault(&config);
        }
#endif
    }

    static std::size_t shaderEditorZoomIndex(float& zoom) noexcept
    {
        std::size_t best = 0;
        float bestDistance = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < ShaderEditorZoomLevels.size(); ++i)
        {
            const float distance = std::abs(ShaderEditorZoomLevels[i] - zoom);
            if (distance < bestDistance) { bestDistance = distance; best = i; }
        }
        zoom = ShaderEditorZoomLevels[best];
        return best;
    }

    static void updateShaderEditorZoomShortcuts(ShaderEditorState& shaderEditor, VisualizerSettings& settings, const bool editorHovered)
    {
        GLFWwindow* window = glfwGetCurrentContext();
        if (!window) return;
        const bool ctrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
        const bool zoomInDown = ctrl && (glfwGetKey(window, GLFW_KEY_EQUAL) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_KP_ADD) == GLFW_PRESS);
        const bool zoomOutDown = ctrl && (glfwGetKey(window, GLFW_KEY_MINUS) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS);
        const bool zoomResetDown = ctrl && glfwGetKey(window, GLFW_KEY_0) == GLFW_PRESS;
        if (zoomInDown && !shaderEditor.ZoomInWasDown) settings.ShaderEditorZoom = std::min(settings.ShaderEditorZoom + 0.10f, 2.50f);
        if (zoomOutDown && !shaderEditor.ZoomOutWasDown) settings.ShaderEditorZoom = std::max(settings.ShaderEditorZoom - 0.10f, 0.60f);
        if (zoomResetDown && !shaderEditor.ZoomResetWasDown) settings.ShaderEditorZoom = 1.0f;
        shaderEditor.ZoomInWasDown = zoomInDown;
        shaderEditor.ZoomOutWasDown = zoomOutDown;
        shaderEditor.ZoomResetWasDown = zoomResetDown;
        if (editorHovered && ctrl && ImGui::GetIO().MouseWheel != 0.0f) settings.ShaderEditorZoom = std::clamp(settings.ShaderEditorZoom + ImGui::GetIO().MouseWheel * 0.10f, 0.60f, 2.50f);
        settings.ShaderEditorZoom = std::round(settings.ShaderEditorZoom * 10.0f) / 10.0f;
    }

    static bool renderShaderTextEditor(TextEditor& editor, const char* id, const ImVec2 size, ShaderEditorState& shaderEditor, VisualizerSettings& settings)
    {
        updateShaderEditorZoomShortcuts(shaderEditor, settings, false);
        const std::size_t zoomIndex = shaderEditorZoomIndex(settings.ShaderEditorZoom);
#if IMGUI_VERSION_NUM >= 19200
        ImGui::PushFont(ShaderEditorFont, shaderEditorPixelSize(settings.ShaderEditorZoom));
#else
        ImGui::PushFont(ShaderEditorFonts[zoomIndex] ? ShaderEditorFonts[zoomIndex] : ImGui::GetFont());
#endif
        const bool changed = editor.Render(id, size);
        ImGui::PopFont();
        const bool hovered = ImGui::IsItemHovered();
        updateShaderEditorZoomShortcuts(shaderEditor, settings, hovered);
        return changed;
    }
