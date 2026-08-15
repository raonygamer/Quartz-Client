#pragma once
#include "quartz/client/runtime/RuntimeTypes.hpp"
#include "quartz/client/shader/ShaderFramebuffer.hpp"

namespace quartz::client
{
    class RuntimeBindingEngine
    {
    public:
        RuntimeBindingEngine() { load(); }
        ~RuntimeBindingEngine() { save(); }

        std::vector<RuntimeBinding>& bindings() noexcept { return _bindings; }
        const std::vector<RuntimeBinding>& bindings() const noexcept { return _bindings; }
        std::vector<RuntimeControlRule>& controls() noexcept { return _controls; }
        const std::vector<RuntimeControlRule>& controls() const noexcept { return _controls; }
        std::vector<RuntimeObjectDescriptor>& objects() noexcept { return _objects; }
        const std::vector<RuntimeObjectDescriptor>& objects() const noexcept { return _objects; }
        std::vector<RuntimeObjectPointer>& pointers() noexcept { return _pointers; }
        const std::vector<RuntimeObjectPointer>& pointers() const noexcept { return _pointers; }
        std::vector<RuntimeValueBankEntry>& bank() noexcept { return _bank; }
        const std::vector<RuntimeValueBankEntry>& bank() const noexcept { return _bank; }
        std::vector<RuntimeBindingProfile>& profiles() noexcept { return _profiles; }
        const std::vector<RuntimeBindingProfile>& profiles() const noexcept { return _profiles; }
        std::uint64_t activeProfileId() const noexcept { return _activeProfileId; }
        void clearActiveProfile() noexcept { if (_activeProfileId != 0) { _activeProfileId = 0; ++_revision; } }
        const RuntimeUSBRates& usbRates() const noexcept { return _usbRates; }
        std::uint64_t revision() const noexcept { return _revision; }
        const std::filesystem::path& path() const noexcept { return _path; }
        int controlPassLimit() const noexcept { return _controlPassLimit; }
        void setControlPassLimit(const int passes) noexcept { const int clamped = std::clamp(passes, 1, 16); if (_controlPassLimit != clamped) { _controlPassLimit = clamped; ++_revision; } }
        int previousShaderPreset() const noexcept { return _previousShaderPreset; }
        const std::string& previousShaderId() const noexcept { return _previousShaderId; }

        RuntimeBinding& add()
        {
            _bindings.emplace_back();
            _bindings.back().Id = _nextBindingId++;
            _bindings.back().Order = static_cast<int>(_bindings.size() - 1);
            std::snprintf(_bindings.back().Name, sizeof(_bindings.back().Name), "Binding %zu", _bindings.size());
            ++_revision;
            return _bindings.back();
        }

        RuntimeControlRule& addControl()
        {
            _controls.emplace_back();
            auto& control = _controls.back();
            control.Id = _nextControlId++;
            control.Order = static_cast<int>(_controls.size() - 1);
            std::snprintf(control.Name, sizeof(control.Name), "Control %zu", _controls.size());
            ++_revision;
            return control;
        }

        RuntimeValueBankEntry& addBankValue()
        {
            _bank.emplace_back();
            auto& value = _bank.back();
            value.Id = _nextBankValueId++;
            std::snprintf(value.Name, sizeof(value.Name), "Value %zu", _bank.size());
            ++_revision;
            return value;
        }

        RuntimeBindingProfile& addProfile()
        {
            _profiles.emplace_back();
            auto& profile = _profiles.back();
            profile.Id = _nextProfileId++;
            std::snprintf(profile.Name, sizeof(profile.Name), "Profile %zu", _profiles.size());
            ++_revision;
            return profile;
        }

        void eraseProfile(const std::size_t index)
        {
            if (index >= _profiles.size()) return;
            const std::uint64_t erasedId = _profiles[index].Id;
            if (_activeProfileId == erasedId) _activeProfileId = 0;
            _profiles.erase(_profiles.begin() + static_cast<std::ptrdiff_t>(index));
            for (auto& binding : _bindings) if (binding.ProfileId == erasedId) binding.ProfileId = 0;
            ++_revision;
        }

        void setProfileMembersEnabled(RuntimeBindingProfile& profile, const bool enabled)
        {
            for (const auto id : profile.BindingIds) if (auto* binding = findBinding(id)) binding->Enabled = enabled;
            for (const auto id : profile.ControlIds) if (auto* control = findControl(id)) control->Enabled = enabled;
            ++_revision;
        }

        void applyProfile(RuntimeBindingProfile& profile)
        {
            if (profile.Exclusive)
            {
                for (auto& binding : _bindings) binding.Enabled = false;
                for (auto& control : _controls) control.Enabled = false;
            }
            setProfileMembersEnabled(profile, true);
            _activeProfileId = profile.Id;
            ++_revision;
        }

        void pollProfileHotkeys(GLFWwindow* window)
        {
            if (!window) return;
            const bool ctrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
            const bool alt = glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
            const bool shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
            for (auto& profile : _profiles)
            {
                if (!profile.Enabled || profile.HotkeyKey <= 0) { profile.HotkeyDown = false; continue; }
                const bool modifiers = (!profile.HotkeyCtrl || ctrl) && (!profile.HotkeyAlt || alt) && (!profile.HotkeyShift || shift);
                const bool down = modifiers && glfwGetKey(window, profile.HotkeyKey) == GLFW_PRESS;
                if (down && !profile.HotkeyDown) applyProfile(profile);
                profile.HotkeyDown = down;
            }
        }

        RuntimeObjectPointer& addPointer()
        {
            _pointers.emplace_back();
            auto& pointer = _pointers.back();
            pointer.Id = _nextPointerId++;
            pointer.Order = static_cast<int>(_pointers.size() - 1);
            std::snprintf(pointer.Name, sizeof(pointer.Name), "Pointer %zu", _pointers.size());
            ++_revision;
            return pointer;
        }

        void erasePointer(const std::size_t index)
        {
            if (index >= _pointers.size()) return;
            const auto id = _pointers[index].Id;
            _pointers.erase(_pointers.begin() + static_cast<std::ptrdiff_t>(index));
            for (auto& binding : _bindings) if (binding.ObjectPointerId == id) binding.ObjectPointerId = 0;
            ++_revision;
        }

        RuntimeObjectPointer* findPointer(const std::uint64_t id) noexcept { const auto it = std::ranges::find_if(_pointers, [&](const RuntimeObjectPointer& p) { return p.Id == id; }); return it == _pointers.end() ? nullptr : &*it; }
        const RuntimeObjectPointer* findPointer(const std::uint64_t id) const noexcept { const auto it = std::ranges::find_if(_pointers, [&](const RuntimeObjectPointer& p) { return p.Id == id; }); return it == _pointers.end() ? nullptr : &*it; }

        RuntimeObjectDescriptor& addObject()
        {
            _objects.emplace_back();
            auto& object = _objects.back();
            object.Id = _nextObjectId++;
            object.Order = static_cast<int>(_objects.size() - 1);
            std::snprintf(object.Name, sizeof(object.Name), "Object %zu", _objects.size());
            ++_revision;
            return object;
        }

        RuntimeObjectField& addObjectField(RuntimeObjectDescriptor& object)
        {
            object.Fields.emplace_back();
            auto& field = object.Fields.back();
            field.Id = _nextObjectFieldId++;
            std::snprintf(field.Name, sizeof(field.Name), "Field %zu", object.Fields.size());
            ++_revision;
            return field;
        }

        void eraseObjectField(RuntimeObjectDescriptor& object, const std::size_t index)
        {
            if (index >= object.Fields.size()) return;
            const std::uint64_t fieldId = object.Fields[index].Id;
            object.Fields.erase(object.Fields.begin() + static_cast<std::ptrdiff_t>(index));
            for (auto& binding : _bindings)
                if (binding.ObjectId == object.Id && binding.ObjectFieldId == fieldId) binding.ObjectFieldId = 0;
            ++_revision;
        }

        void eraseObject(const std::size_t index)
        {
            if (index >= _objects.size()) return;
            const std::uint64_t objectId = _objects[index].Id;
            _objects.erase(_objects.begin() + static_cast<std::ptrdiff_t>(index));
            for (auto& binding : _bindings)
                if (binding.ObjectId == objectId) { binding.ObjectId = 0; binding.ObjectFieldId = 0; }
            for (auto& pointer : _pointers) if (pointer.DescriptorId == objectId) pointer.DescriptorId = 0;
            ++_revision;
        }

        void eraseBankValue(const std::size_t index)
        {
            if (index >= _bank.size()) return;
            const std::uint64_t erasedId = _bank[index].Id;
            _bank.erase(_bank.begin() + static_cast<std::ptrdiff_t>(index));
            for (auto& binding : _bindings)
            {
                if (binding.BankValueId == erasedId) binding.BankValueId = 0;
                if (binding.StoreBankValueId == erasedId) { binding.StoreBankValueId = 0; binding.StoreToBank = false; }
                for (auto& action : binding.Actions)
                {
                    if (action.BankValueId == erasedId) action.BankValueId = 0;
                    if (action.TargetBankValueId == erasedId) action.TargetBankValueId = 0;
                }
            }
            for (auto& control : _controls)
            {
                if (control.TargetBankValueId == erasedId) control.TargetBankValueId = 0;
                for (auto& action : control.Actions)
                {
                    if (action.BankValueId == erasedId) action.BankValueId = 0;
                    if (action.TargetBankValueId == erasedId) action.TargetBankValueId = 0;
                }
            }
            ++_revision;
        }

        void eraseControl(const std::size_t index)
        {
            if (index >= _controls.size()) return;
            const std::uint64_t erasedId = _controls[index].Id;
            _controls.erase(_controls.begin() + static_cast<std::ptrdiff_t>(index));
            for (auto& binding : _bindings)
            {
                if (binding.ControlStatusId == erasedId) binding.ControlStatusId = 0;
                for (auto& action : binding.Actions) if (action.TargetControlId == erasedId) action.TargetControlId = 0;
                std::erase_if(binding.References, [&](const RuntimeSourceReference& reference) { return reference.Kind == RuntimeReferenceKind::Control && reference.Id == erasedId; });
            }
            for (auto& control : _controls)
            {
                if (control.TargetControlId == erasedId) control.TargetControlId = 0;
                for (auto& action : control.Actions) if (action.TargetControlId == erasedId) action.TargetControlId = 0;
            }
            for (auto& profile : _profiles) std::erase(profile.ControlIds, erasedId);
            ++_revision;
        }

        void erase(const std::size_t index)
        {
            if (index >= _bindings.size()) return;
            const std::uint64_t erasedId = _bindings[index].Id;
            _bindings.erase(_bindings.begin() + static_cast<std::ptrdiff_t>(index));
            for (auto& binding : _bindings)
            {
                if (binding.StatusBindingId == erasedId) binding.StatusBindingId = 0;
                if (binding.ValueBindingId == erasedId) binding.ValueBindingId = 0;
                for (auto& action : binding.Actions)
                {
                    if (action.TargetBindingId == erasedId) action.TargetBindingId = 0;
                    if (action.ValueBindingId == erasedId) action.ValueBindingId = 0;
                }
                for (auto& link : binding.ParameterLinks)
                    if (link.Enabled && link.BindingId == erasedId) link = {};
                std::erase_if(binding.References, [&](const RuntimeSourceReference& reference) { return reference.Kind == RuntimeReferenceKind::Binding && reference.Id == erasedId; });
            }
            for (auto& object : _objects)
            {
                if (object.BaseBindingId == erasedId) object.BaseBindingId = 0;
                if (object.ProcessBindingId == erasedId) object.ProcessBindingId = 0;
            }
            for (auto& control : _controls)
            {
                if (control.SourceBindingId == erasedId) control.SourceBindingId = 0;
                if (control.TargetBindingId == erasedId) control.TargetBindingId = 0;
                for (auto& action : control.Actions)
                {
                    if (action.TargetBindingId == erasedId) action.TargetBindingId = 0;
                    if (action.ValueBindingId == erasedId) action.ValueBindingId = 0;
                }
            }
            for (auto& profile : _profiles) std::erase(profile.BindingIds, erasedId);
            ++_revision;
        }

        void markChanged() noexcept { ++_revision; }

        RuntimeBinding* findBinding(const std::uint64_t id) noexcept
        {
            const auto it = std::ranges::find(_bindings, id, &RuntimeBinding::Id);
            return it == _bindings.end() ? nullptr : &*it;
        }

        const RuntimeBinding* findBinding(const std::uint64_t id) const noexcept
        {
            const auto it = std::ranges::find(_bindings, id, &RuntimeBinding::Id);
            return it == _bindings.end() ? nullptr : &*it;
        }

        RuntimeControlRule* findControl(const std::uint64_t id) noexcept
        {
            const auto it = std::ranges::find(_controls, id, &RuntimeControlRule::Id);
            return it == _controls.end() ? nullptr : &*it;
        }

        const RuntimeControlRule* findControl(const std::uint64_t id) const noexcept
        {
            const auto it = std::ranges::find(_controls, id, &RuntimeControlRule::Id);
            return it == _controls.end() ? nullptr : &*it;
        }

        RuntimeValueBankEntry* findBankValue(const std::uint64_t id) noexcept
        {
            const auto it = std::ranges::find(_bank, id, &RuntimeValueBankEntry::Id);
            return it == _bank.end() ? nullptr : &*it;
        }

        const RuntimeValueBankEntry* findBankValue(const std::uint64_t id) const noexcept
        {
            const auto it = std::ranges::find(_bank, id, &RuntimeValueBankEntry::Id);
            return it == _bank.end() ? nullptr : &*it;
        }

        RuntimeBindingProfile* findProfile(const std::uint64_t id) noexcept
        {
            const auto it = std::ranges::find(_profiles, id, &RuntimeBindingProfile::Id);
            return it == _profiles.end() ? nullptr : &*it;
        }

        const RuntimeBindingProfile* findProfile(const std::uint64_t id) const noexcept
        {
            const auto it = std::ranges::find(_profiles, id, &RuntimeBindingProfile::Id);
            return it == _profiles.end() ? nullptr : &*it;
        }

        RuntimeObjectDescriptor* findObject(const std::uint64_t id) noexcept
        {
            const auto it = std::ranges::find(_objects, id, &RuntimeObjectDescriptor::Id);
            return it == _objects.end() ? nullptr : &*it;
        }

        const RuntimeObjectDescriptor* findObject(const std::uint64_t id) const noexcept
        {
            const auto it = std::ranges::find(_objects, id, &RuntimeObjectDescriptor::Id);
            return it == _objects.end() ? nullptr : &*it;
        }

        RuntimeObjectField* findObjectField(RuntimeObjectDescriptor& object, const std::uint64_t id) noexcept
        {
            const auto it = std::ranges::find(object.Fields, id, &RuntimeObjectField::Id);
            return it == object.Fields.end() ? nullptr : &*it;
        }

        const RuntimeObjectField* findObjectField(const RuntimeObjectDescriptor& object, const std::uint64_t id) const noexcept
        {
            const auto it = std::ranges::find(object.Fields, id, &RuntimeObjectField::Id);
            return it == object.Fields.end() ? nullptr : &*it;
        }

        bool canParameterLink(const std::uint64_t ownerId, const std::uint64_t sourceId) const
        {
            if (ownerId == 0 || sourceId == 0 || ownerId == sourceId || !findBinding(sourceId)) return false;
            std::set<std::uint64_t> visited;
            return !bindingDependsOn(sourceId, ownerId, visited);
        }

        bool setParameterLink(RuntimeBinding& owner, const RuntimeParameterSlot slot, const std::uint64_t sourceId)
        {
            auto& link = owner.ParameterLinks[static_cast<std::size_t>(slot)];
            if (sourceId == 0)
            {
                link = {};
                ++_revision;
                return true;
            }
            if (!canParameterLink(owner.Id, sourceId)) return false;
            link.Enabled = true;
            link.BindingId = sourceId;
            ++_revision;
            return true;
        }

        float parameterValue(const RuntimeBinding& owner, const RuntimeParameterSlot slot, const float fallback) const noexcept
        {
            const auto& link = owner.ParameterLinks[static_cast<std::size_t>(slot)];
            if (!link.Enabled || link.BindingId == 0) return fallback;
            const RuntimeBinding* source = findBinding(link.BindingId);
            return source && source->Enabled && source->HasValue ? source->Value : fallback;
        }

        bool parameterBool(const RuntimeBinding& owner, const RuntimeParameterSlot slot, const bool fallback) const noexcept
        {
            return parameterValue(owner, slot, fallback ? 1.0f : 0.0f) >= 0.5f;
        }

        void applyMaterialValues(ShaderFramebuffer& shader) const noexcept
        {
            std::vector<const RuntimeBinding*> order;
            order.reserve(_bindings.size());
            for (const auto& binding : _bindings) order.push_back(&binding);
            std::ranges::sort(order, [](const RuntimeBinding* a, const RuntimeBinding* b)
            {
                if (a->Priority != b->Priority) return a->Priority < b->Priority;
                return a->Id < b->Id;
            });
            for (const RuntimeBinding* binding : order)
                if (binding->Enabled && binding->RuntimeEnabled && binding->WriteMaterial && binding->HasValue)
                    shader.setMaterialParameter(binding->TargetId, binding->TargetComponent, binding->Value);
        }

        void updateRates(const USBStatsSnapshot& stats, const double now)
        {
            if (_rateTime <= 0.0)
            {
                _lastUSB = stats;
                _rateTime = now;
                return;
            }
            const double elapsed = now - _rateTime;
            if (elapsed < 0.25) return;
            _usbRates.TxKiB = (stats.TxBytes - _lastUSB.TxBytes) / 1024.0 / elapsed;
            _usbRates.RxKiB = (stats.RxBytes - _lastUSB.RxBytes) / 1024.0 / elapsed;
            _usbRates.TxTransfers = (stats.TxTransfers - _lastUSB.TxTransfers) / elapsed;
            _usbRates.RxTransfers = (stats.RxTransfers - _lastUSB.RxTransfers) / elapsed;
            _lastUSB = stats;
            _rateTime = now;
        }

        void update(const RuntimeSignalContext& context, ShaderFramebuffer& shader)
        {
            _lastRuntimeTime = context.Time;
            _pendingOutput = {};
            if (_frameTime != context.Time)
            {
                _frameTime = context.Time;
                for (auto& value : _bank) value.ChangedThisFrame = false;
            }
            if (_observedShaderPreset < 0) { _observedShaderPreset = context.CurrentShaderPreset; _previousShaderPreset = context.CurrentShaderPreset; }
            else if (context.CurrentShaderPreset != _observedShaderPreset) { _previousShaderPreset = _observedShaderPreset; _observedShaderPreset = context.CurrentShaderPreset; }
            if (_observedShaderId.empty()) { _observedShaderId = context.CurrentShaderId; _previousShaderId = context.CurrentShaderId; }
            else if (!context.CurrentShaderId.empty() && context.CurrentShaderId != _observedShaderId) { _previousShaderId = _observedShaderId; _observedShaderId = context.CurrentShaderId; }
            for (auto& binding : _bindings) binding.RuntimeEnabled = true;
            for (auto& control : _controls) control.RuntimeEnabled = true;

            std::vector<RuntimeBinding*> order;
            order.reserve(_bindings.size());
            for (auto& binding : _bindings) order.push_back(&binding);
            std::ranges::sort(order, [](const RuntimeBinding* a, const RuntimeBinding* b)
            {
                if (a->Priority != b->Priority) return a->Priority < b->Priority;
                return a->Id < b->Id;
            });
            for (RuntimeBinding* bindingPtr : order)
            {
                auto& binding = *bindingPtr;
                if (!binding.Enabled || !binding.RuntimeEnabled || context.Time < binding.NextUpdate) continue;
                const float updateHz = std::clamp(parameterValue(binding, RuntimeParameterSlot::UpdateHz, binding.UpdateHz), 0.5f, 500.0f);
                binding.NextUpdate = context.Time + 1.0 / updateHz;

                const float previousValue = binding.Value;
                const std::string previousString = binding.StringValue;
                const bool hadValue = binding.HasValue;
                const bool hadString = binding.HasString;
                float raw = 0.0f;
                binding.LastReadSucceeded = false;
                binding.HasString = false;
                binding.StringValue.clear();
                if (!readSource(binding, context, raw)) continue;
                binding.LastReadSucceeded = true;
                binding.LastSuccessTime = context.Time;
                binding.RawValue = raw;

                const bool normalize = parameterBool(binding, RuntimeParameterSlot::Normalize, binding.Normalize);
                const float inputMin = parameterValue(binding, RuntimeParameterSlot::InputMin, binding.InputMin);
                const float inputMax = parameterValue(binding, RuntimeParameterSlot::InputMax, binding.InputMax);
                const bool invert = parameterBool(binding, RuntimeParameterSlot::Invert, binding.Invert);
                const float scale = parameterValue(binding, RuntimeParameterSlot::Scale, binding.Scale);
                const float offset = parameterValue(binding, RuntimeParameterSlot::Offset, binding.Offset);
                const bool clampOutput = parameterBool(binding, RuntimeParameterSlot::Clamp, binding.Clamp);
                const float outputMin = parameterValue(binding, RuntimeParameterSlot::OutputMin, binding.OutputMin);
                const float outputMax = parameterValue(binding, RuntimeParameterSlot::OutputMax, binding.OutputMax);
                const float smoothingHz = std::max(parameterValue(binding, RuntimeParameterSlot::SmoothingHz, binding.SmoothingHz), 0.0f);

                float transformed = raw;
                if (normalize)
                {
                    const float range = inputMax - inputMin;
                    transformed = std::abs(range) > 0.000001f ? (transformed - inputMin) / range : 0.0f;
                }
                if (invert) transformed = 1.0f - transformed;
                transformed = transformed * scale + offset;
                if (clampOutput) transformed = std::clamp(transformed, std::min(outputMin, outputMax), std::max(outputMin, outputMax));

                const float dt = binding.LastUpdate > 0.0 ? static_cast<float>(std::clamp(context.Time - binding.LastUpdate, 0.0001, 1.0)) : 1.0f / updateHz;
                binding.LastUpdate = context.Time;
                if (!binding.HasValue)
                {
                    binding.Value = transformed;
                    binding.HasValue = true;
                }
                else
                {
                    const float alpha = smoothingHz <= 0.0f ? 1.0f : 1.0f - std::exp(-smoothingHz * dt);
                    binding.Value += (transformed - binding.Value) * alpha;
                }

                const bool numericChanged = !hadValue || std::abs(binding.Value - previousValue) > 0.000001f;
                const bool stringChanged = binding.HasString && (!hadString || binding.StringValue != previousString);
                if (binding.StoreToBank && binding.StoreBankValueId != 0)
                    writeBindingToBank(binding, binding.StoreBankValueId);
                for (auto& action : binding.Actions)
                {
                    const bool run = action.When == RuntimeActionWhen::OnUpdate || action.When == RuntimeActionWhen::WhileActive
                        || (action.When == RuntimeActionWhen::OnTrigger && (!hadValue || (!hadString && binding.HasString)))
                        || (action.When == RuntimeActionWhen::OnChange && (numericChanged || stringChanged))
                        || (action.When == RuntimeActionWhen::OnTruthy && binding.Value >= 0.5f)
                        || (action.When == RuntimeActionWhen::OnFalsy && binding.Value < 0.5f);
                    if (run && action.Enabled) applyAction(action, &binding, shader, _pendingOutput);
                }

                if (binding.WriteMaterial)
                {
                    if (!shader.setMaterialParameter(binding.TargetId, binding.TargetComponent, binding.Value)) binding.Error = "material id/component not active in current shader";
                    else binding.Error.clear();
                }
                else binding.Error.clear();
            }
            for (auto& pointer : _pointers) resolveObjectPointer(pointer);
            for (auto& object : _objects) runtimeObjectFieldOffset(object, 0, &object.Size);
        }

        RuntimeControlOutput evaluateControls(ShaderFramebuffer& shader)
        {
            for (auto& control : _controls) control.TriggeredThisFrame = false;
            RuntimeControlOutput output = _pendingOutput;
            std::vector<RuntimeControlRule*> order;
            order.reserve(_controls.size());
            for (auto& control : _controls) order.push_back(&control);
            std::ranges::sort(order, [](const RuntimeControlRule* a, const RuntimeControlRule* b)
            {
                if (a->Priority != b->Priority) return a->Priority < b->Priority;
                return a->Id < b->Id;
            });

            for (int pass = 0; pass < _controlPassLimit; ++pass)
            {
                bool mutated = false;
                for (RuntimeControlRule* controlPtr : order)
                {
                    auto& control = *controlPtr;
                    if (!control.Enabled || !control.RuntimeEnabled) { control.ConditionActive = false; continue; }
                    RuntimeBinding* source = findBinding(control.SourceBindingId);
                    if (!source || !source->Enabled || !source->HasValue) { control.ConditionActive = false; continue; }
                    const bool wasActive = control.ConditionActive;
                    const bool active = evaluateControlCondition(control, *source);
                    const bool event = runtimeControlConditionIsEvent(control.Condition);
                    const bool triggered = event ? active : active && !wasActive;
                    if (triggered && !control.TriggeredThisFrame)
                    {
                        control.TriggeredThisFrame = true;
                        control.LastTriggerTime = _lastRuntimeTime;
                        ++control.TriggerCount;
                    }
                    if (!active && !triggered) continue;

                    // Legacy single target stays valid for v5/v6 configs; binding operations are edge-triggered by default so an active comparison does not rescan/rearm every frame.
                    if (runtimeControlTargetIsBindingOperation(control.Target)) { if (triggered) mutated |= applyLegacyControlTarget(control, *source, shader, output); }
                    else if (active) mutated |= applyLegacyControlTarget(control, *source, shader, output);
                    for (auto& action : control.Actions)
                    {
                        if (!action.Enabled) continue;
                        const bool run = action.When == RuntimeActionWhen::WhileActive ? active
                            : action.When == RuntimeActionWhen::OnTrigger ? triggered
                            : action.When == RuntimeActionWhen::OnUpdate ? active
                            : action.When == RuntimeActionWhen::OnChange ? triggered
                            : action.When == RuntimeActionWhen::OnTruthy ? active && source->Value >= 0.5f
                            : active && source->Value < 0.5f;
                        if (run) mutated |= applyAction(action, source, shader, output);
                    }
                }
                if (!mutated) break;
            }
            return output;
        }

        bool save()
        {
            std::error_code ec;
            std::filesystem::create_directories(_path.parent_path(), ec);
            const auto temporary = std::filesystem::path(_path.string() + ".tmp");
            std::ofstream file(temporary, std::ios::trunc);
            if (!file) return false;
            file << "# Quartz runtime material bindings v10\n";
            for (const auto& b : _bindings)
            {
                file << "B\t" << b.Enabled << '\t' << static_cast<int>(b.Source) << '\t' << b.Signal << '\t' << b.Constant << '\t'
                     << runtimeEscape(b.Name) << '\t' << runtimeEscape(b.TargetId) << '\t' << b.TargetComponent << '\t'
                     << b.ProcessId << '\t' << b.AutoReattach << '\t' << static_cast<int>(b.ValueType) << '\t'
                     << runtimeEscape(b.ProcessName) << '\t' << runtimeEscape(b.Module) << '\t' << runtimeEscape(b.Address) << '\t'
                     << "" << '\t' << "" << '\t' << 0 << '\t' << false << '\t'
                     << b.Normalize << '\t' << b.InputMin << '\t' << b.InputMax << '\t' << b.Invert << '\t' << b.Scale << '\t' << b.Offset << '\t'
                     << b.Clamp << '\t' << b.OutputMin << '\t' << b.OutputMax << '\t' << b.SmoothingHz << '\t' << b.UpdateHz << '\t'
                     << static_cast<int>(b.RebindMode) << '\t' << runtimeEscape(b.ProcessRebindPattern) << '\t' << b.Id << '\t'
                     << static_cast<int>(b.AddressMode) << '\t' << runtimeEscape(b.Signature) << '\t' << b.SignatureExecutableOnly << '\t'
                     << static_cast<int>(b.SignatureResolve) << '\t' << b.SignatureResultOffset << '\t' << b.SignatureInstructionSize << '\t' << b.SignatureRetrySeconds << '\t'
                     << runtimeEscape(serializeParameterLinks(b)) << '\t' << static_cast<int>(b.SignatureRegister) << '\t' << b.SignatureRegisterDisplacementOffset << '\t'
                     << static_cast<int>(b.SignatureDisplacementType) << '\t' << b.SignatureManualDisplacement << '\t' << b.SignatureCaptureTimeoutSeconds << '\t' << b.StatusBindingId << '\t' << b.WriteMaterial << '\t'
                     << b.Priority << '\t' << b.UnboundValue << '\t' << b.ValueBindingId << '\t' << b.ControlStatusId << '\t' << b.ObjectId << '\t' << b.ObjectFieldId << '\t'
                     << static_cast<int>(b.AggregateOperation) << '\t' << static_cast<int>(b.CompareCondition) << '\t' << static_cast<int>(b.CompareResult) << '\t'
                     << b.CompareA << '\t' << b.CompareB << '\t' << b.CompareTolerance << '\t' << runtimeEscape(serializeReferences(b.References)) << '\t'
                     << b.BankValueId << '\t' << b.StoreBankValueId << '\t' << b.StoreToBank << '\t' << runtimeEscape(b.StringConstant) << '\t' << runtimeEscape(serializeActions(b.Actions)) << '\t' << b.ProfileId << '\t'
                     << static_cast<int>(b.SignaturePatternKind) << '\t' << b.Order << '\t' << runtimeEscape(b.Group) << '\t' << b.ObjectPointerId << '\n';
            }
            for (const auto& c : _controls)
            {
                file << "C\t" << c.Enabled << '\t' << c.Id << '\t' << runtimeEscape(c.Name) << '\t' << c.SourceBindingId << '\t'
                     << static_cast<int>(c.Condition) << '\t' << c.ValueA << '\t' << c.ValueB << '\t' << c.Tolerance << '\t' << c.Hysteresis << '\t'
                     << static_cast<int>(c.Target) << '\t' << c.ShaderPresetIndex << '\t' << c.TargetBindingId << '\t' << c.TargetValue << '\t' << c.TargetBool << '\t'
                     << c.TargetComponent << '\t' << runtimeEscape(c.TargetId) << '\t' << c.TransitionSeconds << '\t' << c.Priority << '\t' << c.TargetUseSourceValue << '\t'
                     << runtimeEscape(c.StringCompare) << '\t' << c.FireOnFirstSample << '\t' << runtimeEscape(serializeActions(c.Actions)) << '\t'
                     << c.TargetBankValueId << '\t' << c.TargetControlId << '\t' << runtimeEscape(c.ShaderId) << '\t' << c.Order << '\t' << runtimeEscape(c.Group) << '\n';
            }
            file << "G\t" << _controlPassLimit << '\t' << _activeProfileId << '\n';
            for (const auto& value : _bank)
                file << "V\t" << value.Enabled << '\t' << value.Id << '\t' << runtimeEscape(value.Name) << '\t' << runtimeEscape(value.Description) << '\t'
                     << static_cast<int>(value.Type) << '\t' << value.Number << '\t' << value.Integer << '\t' << value.Boolean << '\t' << runtimeEscape(value.String) << '\t'
                     << static_cast<unsigned long long>(value.Address) << '\t' << value.HasValue << '\n';
            for (const auto& profile : _profiles)
                file << "P\t" << profile.Enabled << '\t' << profile.Id << '\t' << runtimeEscape(profile.Name) << '\t' << profile.Exclusive << '\t'
                     << profile.HotkeyCtrl << '\t' << profile.HotkeyAlt << '\t' << profile.HotkeyShift << '\t' << profile.HotkeyKey << '\t'
                     << serializeIdList(profile.BindingIds) << '\t' << serializeIdList(profile.ControlIds) << '\n';
            for (const auto& pointer : _pointers)
                file << "Q\t" << pointer.Enabled << '\t' << pointer.Id << '\t' << runtimeEscape(pointer.Name) << '\t' << pointer.DescriptorId << '\t' << pointer.BaseBindingId << '\t' << pointer.ProcessBindingId << '\t' << pointer.BaseOffset << '\t' << pointer.Order << '\t' << runtimeEscape(pointer.Group) << '\n';
            for (const auto& object : _objects)
            {
                file << "O\t" << object.Enabled << '\t' << object.Id << '\t' << runtimeEscape(object.Name) << '\t' << runtimeEscape(object.Description) << '\t'
                     << object.BaseBindingId << '\t' << object.ProcessBindingId << '\t' << object.BaseOffset << '\t' << static_cast<int>(object.Packing) << '\t' << object.Order << '\t' << runtimeEscape(object.Group) << '\n';
                for (const auto& field : object.Fields)
                    file << "F\t" << object.Id << '\t' << field.Id << '\t' << field.Enabled << '\t' << runtimeEscape(field.Name) << '\t' << static_cast<int>(field.Type) << '\t'
                         << static_cast<int>(field.Alignment) << '\t' << field.ManualOffset << '\t' << field.Offset << '\t' << field.CustomFillerBytes << '\t' << field.StringMaxLength << '\t' << field.FixedElementCount << '\n';
            }
            file.close();
            if (!file) return false;
            std::filesystem::rename(temporary, _path, ec);
            if (ec)
            {
                std::filesystem::remove(_path, ec);
                ec.clear();
                std::filesystem::rename(temporary, _path, ec);
            }
            if (!ec) _savedRevision = _revision;
            return !ec;
        }

        void saveIfChanged() { if (_savedRevision != _revision) save(); }

    private:
        template<std::size_t N>
        static void copyField(char (&destination)[N], const std::string& value)
        {
            std::snprintf(destination, N, "%s", value.c_str());
        }

        void load()
        {
            _path = runtimeBindingsPath();
            std::ifstream file(_path);
            if (!file) return;
            std::string line;
            while (std::getline(file, line))
            {
                if (line.starts_with("B\t"))
                {
                    std::vector<std::string> fields;
                    std::size_t start = 2;
                    for (;;)
                    {
                        const std::size_t tab = line.find('\t', start);
                        fields.emplace_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                        if (tab == std::string::npos) break;
                        start = tab + 1;
                    }
                    if (fields.size() < 27) continue;
                    RuntimeBinding b;
                    auto parseInt = [&](const std::size_t index, int& value) { return index < fields.size() && parseNumber(fields[index], value); };
                    auto parseFloat = [&](const std::size_t index, float& value) { return index < fields.size() && parseNumber(fields[index], value); };
                    auto parseB = [&](const std::size_t index, bool& value) { return index < fields.size() && parseBool(fields[index], value); };
                    int source = 0, valueType = 0;
                    if (!parseB(0, b.Enabled) || !parseInt(1, source) || !parseInt(2, b.Signal) || !parseFloat(3, b.Constant)) continue;
                    b.Source = static_cast<RuntimeSourceKind>(std::clamp(source, 0, static_cast<int>(RuntimeSourceKind::ProfileState)));
                    copyField(b.Name, runtimeUnescape(fields[4])); copyField(b.TargetId, runtimeUnescape(fields[5]));
                    parseInt(6, b.TargetComponent); parseInt(7, b.ProcessId); parseB(8, b.AutoReattach); parseInt(9, valueType);
                    b.ValueType = static_cast<ProcessValueType>(std::clamp(valueType, 0, static_cast<int>(ProcessValueType::Bool)));
                    copyField(b.ProcessName, runtimeUnescape(fields[10])); copyField(b.Module, runtimeUnescape(fields[11])); copyField(b.Address, runtimeUnescape(fields[12]));
                    // fields 13..16 were the removed CLR type/member/instance/static fields. Keep their slots for v4 compatibility.
                    parseB(17, b.Normalize); parseFloat(18, b.InputMin); parseFloat(19, b.InputMax);
                    parseB(20, b.Invert); parseFloat(21, b.Scale); parseFloat(22, b.Offset); parseB(23, b.Clamp); parseFloat(24, b.OutputMin); parseFloat(25, b.OutputMax);
                    if (fields.size() > 26) parseFloat(26, b.SmoothingHz);
                    if (fields.size() > 27) parseFloat(27, b.UpdateHz);
                    int rebindMode = static_cast<int>(ProcessRebindMode::NameExact);
                    if (fields.size() > 28) parseInt(28, rebindMode);
                    b.RebindMode = static_cast<ProcessRebindMode>(std::clamp(rebindMode, 0, static_cast<int>(ProcessRebindMode::AnyRegex)));
                    if (fields.size() > 29) copyField(b.ProcessRebindPattern, runtimeUnescape(fields[29]));
                    if (b.ProcessRebindPattern[0] == '\0' && b.ProcessName[0] != '\0') std::snprintf(b.ProcessRebindPattern, sizeof(b.ProcessRebindPattern), "%s", b.ProcessName);
                    if (fields.size() > 30) parseNumber(fields[30], b.Id);
                    int addressMode = static_cast<int>(ProcessAddressMode::AddressChain);
                    if (fields.size() > 31) parseInt(31, addressMode);
                    b.AddressMode = static_cast<ProcessAddressMode>(std::clamp(addressMode, 0, static_cast<int>(ProcessAddressMode::Signature)));
                    if (fields.size() > 32) copyField(b.Signature, runtimeUnescape(fields[32]));
                    if (fields.size() > 33) parseB(33, b.SignatureExecutableOnly);
                    int signatureResolve = static_cast<int>(SignatureResultMode::MatchAddress);
                    if (fields.size() > 34) parseInt(34, signatureResolve);
                    b.SignatureResolve = static_cast<SignatureResultMode>(std::clamp(signatureResolve, 0, static_cast<int>(SignatureResultMode::Address32AtOffset)));
                    if (fields.size() > 35) parseInt(35, b.SignatureResultOffset);
                    if (fields.size() > 36) parseInt(36, b.SignatureInstructionSize);
                    if (fields.size() > 37) parseFloat(37, b.SignatureRetrySeconds);
                    if (fields.size() > 38) parseParameterLinks(runtimeUnescape(fields[38]), b);
                    int signatureRegister = static_cast<int>(RuntimeX64Register::R15);
                    if (fields.size() > 39) parseInt(39, signatureRegister);
                    b.SignatureRegister = static_cast<RuntimeX64Register>(std::clamp(signatureRegister, 0, static_cast<int>(RuntimeX64Register::R15)));
                    if (fields.size() > 40) parseInt(40, b.SignatureRegisterDisplacementOffset);
                    int displacementType = static_cast<int>(RuntimeDisplacementType::I32);
                    if (fields.size() > 41) parseInt(41, displacementType);
                    b.SignatureDisplacementType = static_cast<RuntimeDisplacementType>(std::clamp(displacementType, 0, static_cast<int>(RuntimeDisplacementType::Manual)));
                    if (fields.size() > 42) parseInt(42, b.SignatureManualDisplacement);
                    if (fields.size() > 43) parseFloat(43, b.SignatureCaptureTimeoutSeconds);
                    if (fields.size() > 44) parseNumber(fields[44], b.StatusBindingId);
                    if (fields.size() > 45) parseB(45, b.WriteMaterial);
                    if (fields.size() > 46) parseInt(46, b.Priority);
                    if (fields.size() > 47) parseFloat(47, b.UnboundValue);
                    if (fields.size() > 48) parseNumber(fields[48], b.ValueBindingId);
                    if (fields.size() > 49) parseNumber(fields[49], b.ControlStatusId);
                    if (fields.size() > 50) parseNumber(fields[50], b.ObjectId);
                    if (fields.size() > 51) parseNumber(fields[51], b.ObjectFieldId);
                    int aggregateOperation = static_cast<int>(RuntimeAggregateOperation::Average), compareCondition = static_cast<int>(RuntimeCompareCondition::Greater), compareResult = static_cast<int>(RuntimeMassCompareResult::Any);
                    if (fields.size() > 52) parseInt(52, aggregateOperation);
                    if (fields.size() > 53) parseInt(53, compareCondition);
                    if (fields.size() > 54) parseInt(54, compareResult);
                    b.AggregateOperation = static_cast<RuntimeAggregateOperation>(std::clamp(aggregateOperation, 0, static_cast<int>(RuntimeAggregateOperation::All)));
                    b.CompareCondition = static_cast<RuntimeCompareCondition>(std::clamp(compareCondition, 0, static_cast<int>(RuntimeCompareCondition::Outside)));
                    b.CompareResult = static_cast<RuntimeMassCompareResult>(std::clamp(compareResult, 0, static_cast<int>(RuntimeMassCompareResult::FirstMatchIndex)));
                    if (fields.size() > 55) parseFloat(55, b.CompareA);
                    if (fields.size() > 56) parseFloat(56, b.CompareB);
                    if (fields.size() > 57) parseFloat(57, b.CompareTolerance);
                    if (fields.size() > 58) parseReferences(runtimeUnescape(fields[58]), b.References);
                    if (fields.size() > 59) parseNumber(fields[59], b.BankValueId);
                    if (fields.size() > 60) parseNumber(fields[60], b.StoreBankValueId);
                    if (fields.size() > 61) parseB(61, b.StoreToBank);
                    if (fields.size() > 62) copyField(b.StringConstant, runtimeUnescape(fields[62]));
                    if (fields.size() > 63) parseActions(runtimeUnescape(fields[63]), b.Actions);
                    if (fields.size() > 64) parseNumber(fields[64], b.ProfileId);
                    int patternKind = 0; if (fields.size() > 65) parseInt(65, patternKind); b.SignaturePatternKind = static_cast<RuntimeSignaturePatternKind>(std::clamp(patternKind, 0, 1));
                    if (fields.size() > 66) parseInt(66, b.Order);
                    if (fields.size() > 67) copyField(b.Group, runtimeUnescape(fields[67]));
                    if (fields.size() > 68) parseNumber(fields[68], b.ObjectPointerId);
                    if (b.Id == 0) b.Id = _nextBindingId++;
                    else _nextBindingId = std::max(_nextBindingId, b.Id + 1);
                    _bindings.emplace_back(std::move(b));
                }
                else if (line.starts_with("C\t"))
                {
                    std::vector<std::string> fields;
                    std::size_t start = 2;
                    for (;;)
                    {
                        const std::size_t tab = line.find('\t', start);
                        fields.emplace_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                        if (tab == std::string::npos) break;
                        start = tab + 1;
                    }
                    if (fields.size() < 15) continue;
                    RuntimeControlRule c;
                    int condition = 0, target = 0;
                    if (!parseBool(fields[0], c.Enabled) || !parseNumber(fields[1], c.Id)) continue;
                    copyField(c.Name, runtimeUnescape(fields[2]));
                    parseNumber(fields[3], c.SourceBindingId);
                    parseNumber(fields[4], condition);
                    c.Condition = static_cast<RuntimeControlCondition>(std::clamp(condition, 0, static_cast<int>(RuntimeControlCondition::StringContains)));
                    parseNumber(fields[5], c.ValueA); parseNumber(fields[6], c.ValueB); parseNumber(fields[7], c.Tolerance); parseNumber(fields[8], c.Hysteresis);
                    parseNumber(fields[9], target);
                    c.Target = static_cast<RuntimeControlTarget>(std::clamp(target, 0, static_cast<int>(RuntimeControlTarget::BindingClearError)));
                    parseNumber(fields[10], c.ShaderPresetIndex); parseNumber(fields[11], c.TargetBindingId); parseNumber(fields[12], c.TargetValue); parseBool(fields[13], c.TargetBool);
                    if (fields.size() > 14) parseNumber(fields[14], c.TargetComponent);
                    if (fields.size() > 15) copyField(c.TargetId, runtimeUnescape(fields[15]));
                    if (fields.size() > 16) parseNumber(fields[16], c.TransitionSeconds);
                    if (fields.size() > 17) parseNumber(fields[17], c.Priority);
                    if (fields.size() > 18) parseBool(fields[18], c.TargetUseSourceValue);
                    if (fields.size() > 19) copyField(c.StringCompare, runtimeUnescape(fields[19]));
                    if (fields.size() > 20) parseBool(fields[20], c.FireOnFirstSample);
                    if (fields.size() > 21) parseActions(runtimeUnescape(fields[21]), c.Actions);
                    if (fields.size() > 22) parseNumber(fields[22], c.TargetBankValueId);
                    if (fields.size() > 23) parseNumber(fields[23], c.TargetControlId);
                    if (fields.size() > 24) copyField(c.ShaderId, runtimeUnescape(fields[24]));
                    if (fields.size() > 25) parseNumber(fields[25], c.Order);
                    if (fields.size() > 26) copyField(c.Group, runtimeUnescape(fields[26]));
                    if (c.Id == 0) c.Id = _nextControlId++;
                    else _nextControlId = std::max(_nextControlId, c.Id + 1);
                    _controls.emplace_back(std::move(c));
                }
                else if (line.starts_with("G\t"))
                {
                    std::vector<std::string> fields; std::size_t start = 2;
                    for (;;) { const std::size_t tab = line.find('\t', start); fields.emplace_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start)); if (tab == std::string::npos) break; start = tab + 1; }
                    int passes = _controlPassLimit;
                    if (!fields.empty() && parseNumber(fields[0], passes)) _controlPassLimit = std::clamp(passes, 1, 16);
                    if (fields.size() > 1) parseNumber(fields[1], _activeProfileId);
                }
                else if (line.starts_with("V\t"))
                {
                    std::vector<std::string> fields;
                    std::size_t start = 2;
                    for (;;)
                    {
                        const std::size_t tab = line.find('\t', start);
                        fields.emplace_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                        if (tab == std::string::npos) break;
                        start = tab + 1;
                    }
                    if (fields.size() < 10) continue;
                    RuntimeValueBankEntry value;
                    int type = 0; unsigned long long address = 0;
                    if (!parseBool(fields[0], value.Enabled) || !parseNumber(fields[1], value.Id)) continue;
                    copyField(value.Name, runtimeUnescape(fields[2])); copyField(value.Description, runtimeUnescape(fields[3]));
                    parseNumber(fields[4], type); value.Type = static_cast<RuntimeBankValueType>(std::clamp(type, 0, static_cast<int>(RuntimeBankValueType::Address)));
                    parseNumber(fields[5], value.Number); parseNumber(fields[6], value.Integer); parseBool(fields[7], value.Boolean); copyField(value.String, runtimeUnescape(fields[8]));
                    parseNumber(fields[9], address); value.Address = static_cast<std::uintptr_t>(address); if (fields.size() > 10) parseBool(fields[10], value.HasValue);
                    if (value.Id == 0) value.Id = _nextBankValueId++; else _nextBankValueId = std::max(_nextBankValueId, value.Id + 1);
                    _bank.emplace_back(std::move(value));
                }
                else if (line.starts_with("P\t"))
                {
                    std::vector<std::string> fields; std::size_t start = 2;
                    for (;;) { const std::size_t tab = line.find('\t', start); fields.emplace_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start)); if (tab == std::string::npos) break; start = tab + 1; }
                    if (fields.size() < 10) continue;
                    RuntimeBindingProfile profile;
                    if (!parseBool(fields[0], profile.Enabled) || !parseNumber(fields[1], profile.Id)) continue;
                    copyField(profile.Name, runtimeUnescape(fields[2])); parseBool(fields[3], profile.Exclusive); parseBool(fields[4], profile.HotkeyCtrl); parseBool(fields[5], profile.HotkeyAlt); parseBool(fields[6], profile.HotkeyShift); parseNumber(fields[7], profile.HotkeyKey);
                    parseIdList(fields[8], profile.BindingIds); parseIdList(fields[9], profile.ControlIds);
                    if (profile.Id == 0) profile.Id = _nextProfileId++; else _nextProfileId = std::max(_nextProfileId, profile.Id + 1);
                    _profiles.emplace_back(std::move(profile));
                }
                else if (line.starts_with("Q\t"))
                {
                    std::vector<std::string> fields; std::size_t start = 2; for (;;) { const std::size_t tab = line.find('\t', start); fields.emplace_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start)); if (tab == std::string::npos) break; start = tab + 1; }
                    if (fields.size() >= 7)
                    {
                        RuntimeObjectPointer pointer; parseBool(fields[0], pointer.Enabled); parseNumber(fields[1], pointer.Id); copyField(pointer.Name, runtimeUnescape(fields[2])); parseNumber(fields[3], pointer.DescriptorId); parseNumber(fields[4], pointer.BaseBindingId); parseNumber(fields[5], pointer.ProcessBindingId); parseNumber(fields[6], pointer.BaseOffset); if (fields.size() > 7) parseNumber(fields[7], pointer.Order); if (fields.size() > 8) copyField(pointer.Group, runtimeUnescape(fields[8]));
                        if (pointer.Id == 0) pointer.Id = _nextPointerId++; else _nextPointerId = std::max(_nextPointerId, pointer.Id + 1); _pointers.emplace_back(std::move(pointer));
                    }
                }
                else if (line.starts_with("O\t"))
                {
                    std::vector<std::string> fields;
                    std::size_t start = 2;
                    for (;;)
                    {
                        const std::size_t tab = line.find('\t', start);
                        fields.emplace_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                        if (tab == std::string::npos) break;
                        start = tab + 1;
                    }
                    if (fields.size() < 8) continue;
                    RuntimeObjectDescriptor object;
                    int packing = 0;
                    if (!parseBool(fields[0], object.Enabled) || !parseNumber(fields[1], object.Id)) continue;
                    copyField(object.Name, runtimeUnescape(fields[2])); copyField(object.Description, runtimeUnescape(fields[3]));
                    parseNumber(fields[4], object.BaseBindingId); parseNumber(fields[5], object.ProcessBindingId); parseNumber(fields[6], object.BaseOffset); parseNumber(fields[7], packing);
                    object.Packing = static_cast<RuntimeObjectPacking>(std::clamp(packing, 0, static_cast<int>(RuntimeObjectPacking::Pack16)));
                    if (fields.size() > 8) parseNumber(fields[8], object.Order);
                    if (fields.size() > 9) copyField(object.Group, runtimeUnescape(fields[9]));
                    if (object.Id == 0) object.Id = _nextObjectId++;
                    else _nextObjectId = std::max(_nextObjectId, object.Id + 1);
                    _objects.emplace_back(std::move(object));
                }
                else if (line.starts_with("F\t"))
                {
                    std::vector<std::string> fields;
                    std::size_t start = 2;
                    for (;;)
                    {
                        const std::size_t tab = line.find('\t', start);
                        fields.emplace_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                        if (tab == std::string::npos) break;
                        start = tab + 1;
                    }
                    if (fields.size() < 9) continue;
                    std::uint64_t objectId = 0;
                    if (!parseNumber(fields[0], objectId)) continue;
                    RuntimeObjectDescriptor* object = findObject(objectId);
                    if (!object) continue;
                    RuntimeObjectField field;
                    int type = 0, alignment = 0;
                    parseNumber(fields[1], field.Id); parseBool(fields[2], field.Enabled); copyField(field.Name, runtimeUnescape(fields[3]));
                    parseNumber(fields[4], type); parseNumber(fields[5], alignment); parseBool(fields[6], field.ManualOffset); parseNumber(fields[7], field.Offset); parseNumber(fields[8], field.CustomFillerBytes);
                    if (fields.size() > 9) parseNumber(fields[9], field.StringMaxLength);
                    if (fields.size() > 10) parseNumber(fields[10], field.FixedElementCount);
                    field.Type = static_cast<RuntimeObjectFieldType>(std::clamp(type, 0, static_cast<int>(RuntimeObjectFieldType::FixedWString)));
                    field.Alignment = static_cast<RuntimeObjectAlignment>(std::clamp(alignment, 0, static_cast<int>(RuntimeObjectAlignment::Align16)));
                    if (field.Id == 0) field.Id = _nextObjectFieldId++;
                    else _nextObjectFieldId = std::max(_nextObjectFieldId, field.Id + 1);
                    object->Fields.emplace_back(std::move(field));
                }
            }
            if (_pointers.empty())
            {
                for (auto& object : _objects) if (object.BaseBindingId != 0)
                {
                    auto& pointer = _pointers.emplace_back(); pointer.Id = _nextPointerId++; pointer.Order = static_cast<int>(_pointers.size() - 1); pointer.Enabled = object.Enabled; pointer.DescriptorId = object.Id; pointer.BaseBindingId = object.BaseBindingId; pointer.ProcessBindingId = object.ProcessBindingId; pointer.BaseOffset = object.BaseOffset; std::snprintf(pointer.Name, sizeof(pointer.Name), "%s instance", object.Name);
                }
            }
            validateParameterLinks();
            _savedRevision = _revision;
        }

        static std::string serializeIdList(const std::vector<std::uint64_t>& ids)
        {
            std::string result;
            for (const auto id : ids) { if (!result.empty()) result.push_back(','); result += std::to_string(id); }
            return result;
        }

        static void parseIdList(const std::string_view text, std::vector<std::uint64_t>& ids)
        {
            ids.clear(); std::size_t start = 0;
            while (start < text.size())
            {
                const std::size_t end = text.find(',', start);
                std::uint64_t id = 0; const auto token = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
                if (parseNumber(token, id) && id != 0) ids.push_back(id);
                if (end == std::string_view::npos) break; start = end + 1;
            }
        }

        static std::string serializeActions(const std::vector<RuntimeAction>& actions)
        {
            std::string result;
            for (const auto& action : actions)
            {
                if (!result.empty()) result.push_back(';');
                result += std::to_string(action.Enabled) + "," + std::to_string(static_cast<int>(action.Target)) + "," + std::to_string(static_cast<int>(action.ValueMode)) + "," + std::to_string(static_cast<int>(action.When)) + ","
                    + std::to_string(action.ShaderPresetIndex) + "," + std::to_string(action.TargetBindingId) + "," + std::to_string(action.TargetControlId) + "," + std::to_string(action.ValueBindingId) + ","
                    + std::to_string(action.BankValueId) + "," + std::to_string(action.Value) + "," + std::to_string(action.BoolValue) + "," + std::to_string(action.TargetComponent) + ","
                    + runtimeEscape(action.TargetId) + "," + runtimeEscape(action.StringValue) + "," + std::to_string(action.TransitionSeconds) + "," + std::to_string(action.TargetBankValueId) + "," + runtimeEscape(action.ShaderId);
            }
            return result;
        }

        static void parseActions(const std::string_view specification, std::vector<RuntimeAction>& actions)
        {
            actions.clear();
            std::size_t start = 0;
            while (start < specification.size())
            {
                const std::size_t end = specification.find(';', start);
                const std::string token(specification.substr(start, end == std::string_view::npos ? specification.size() - start : end - start));
                std::vector<std::string> fields;
                std::size_t fieldStart = 0;
                for (;;)
                {
                    const std::size_t comma = token.find(',', fieldStart);
                    fields.emplace_back(token.substr(fieldStart, comma == std::string::npos ? std::string::npos : comma - fieldStart));
                    if (comma == std::string::npos) break;
                    fieldStart = comma + 1;
                }
                if (fields.size() >= 12)
                {
                    RuntimeAction action; int target = 0, mode = 0, when = 0;
                    parseBool(fields[0], action.Enabled); parseNumber(fields[1], target); parseNumber(fields[2], mode); parseNumber(fields[3], when);
                    action.Target = static_cast<RuntimeActionTarget>(std::clamp(target, 0, static_cast<int>(RuntimeActionTarget::BindingClearError)));
                    action.ValueMode = static_cast<RuntimeActionValueMode>(std::clamp(mode, 0, static_cast<int>(RuntimeActionValueMode::BankValue)));
                    action.When = static_cast<RuntimeActionWhen>(std::clamp(when, 0, static_cast<int>(RuntimeActionWhen::OnFalsy)));
                    parseNumber(fields[4], action.ShaderPresetIndex); parseNumber(fields[5], action.TargetBindingId); parseNumber(fields[6], action.TargetControlId); parseNumber(fields[7], action.ValueBindingId);
                    parseNumber(fields[8], action.BankValueId); parseNumber(fields[9], action.Value); parseBool(fields[10], action.BoolValue); parseNumber(fields[11], action.TargetComponent);
                    if (fields.size() > 12) copyField(action.TargetId, runtimeUnescape(fields[12]));
                    if (fields.size() > 13) copyField(action.StringValue, runtimeUnescape(fields[13]));
                    if (fields.size() > 14) parseNumber(fields[14], action.TransitionSeconds);
                    if (fields.size() > 15) parseNumber(fields[15], action.TargetBankValueId);
                    if (fields.size() > 16) copyField(action.ShaderId, runtimeUnescape(fields[16]));
                    actions.emplace_back(std::move(action));
                }
                if (end == std::string_view::npos) break;
                start = end + 1;
            }
        }

        static std::string serializeReferences(const std::vector<RuntimeSourceReference>& references)
        {
            std::string result;
            for (const auto& reference : references)
            {
                if (!result.empty()) result.push_back(';');
                result += std::to_string(static_cast<int>(reference.Kind)) + "," + std::to_string(reference.Id) + "," + std::to_string(reference.Signal) + "," + std::to_string(reference.Weight) + "," + (reference.Enabled ? "1" : "0")
                    + "," + (reference.UseOwnComparison ? "1" : "0") + "," + std::to_string(static_cast<int>(reference.CompareCondition)) + "," + std::to_string(reference.CompareA) + "," + std::to_string(reference.CompareB) + "," + std::to_string(reference.CompareTolerance);
            }
            return result;
        }

        static void parseReferences(const std::string_view specification, std::vector<RuntimeSourceReference>& references)
        {
            references.clear();
            std::size_t start = 0;
            while (start < specification.size())
            {
                const std::size_t end = specification.find(';', start);
                const std::string token(specification.substr(start, end == std::string_view::npos ? specification.size() - start : end - start));
                std::vector<std::string> fields;
                std::size_t fieldStart = 0;
                for (;;)
                {
                    const std::size_t comma = token.find(',', fieldStart);
                    fields.emplace_back(token.substr(fieldStart, comma == std::string::npos ? std::string::npos : comma - fieldStart));
                    if (comma == std::string::npos) break;
                    fieldStart = comma + 1;
                }
                if (fields.size() >= 2)
                {
                    RuntimeSourceReference reference; int kind = 0, compare = static_cast<int>(RuntimeCompareCondition::Greater);
                    parseNumber(fields[0], kind); parseNumber(fields[1], reference.Id);
                    if (fields.size() > 2) parseNumber(fields[2], reference.Signal);
                    if (fields.size() > 3) parseNumber(fields[3], reference.Weight);
                    if (fields.size() > 4) parseBool(fields[4], reference.Enabled);
                    if (fields.size() > 5) parseBool(fields[5], reference.UseOwnComparison);
                    if (fields.size() > 6) parseNumber(fields[6], compare);
                    if (fields.size() > 7) parseNumber(fields[7], reference.CompareA);
                    if (fields.size() > 8) parseNumber(fields[8], reference.CompareB);
                    if (fields.size() > 9) parseNumber(fields[9], reference.CompareTolerance);
                    reference.Kind = static_cast<RuntimeReferenceKind>(std::clamp(kind, 0, static_cast<int>(RuntimeReferenceKind::Control)));
                    reference.CompareCondition = static_cast<RuntimeCompareCondition>(std::clamp(compare, 0, static_cast<int>(RuntimeCompareCondition::Outside)));
                    if (reference.Id != 0) references.push_back(reference);
                }
                if (end == std::string_view::npos) break;
                start = end + 1;
            }
        }

        static std::string serializeParameterLinks(const RuntimeBinding& binding)
        {
            std::string result;
            for (std::size_t i = 0; i < binding.ParameterLinks.size(); ++i)
            {
                const auto& link = binding.ParameterLinks[i];
                if (!link.Enabled || link.BindingId == 0) continue;
                if (!result.empty()) result.push_back(';');
                result += std::to_string(i) + "=" + std::to_string(link.BindingId);
            }
            return result;
        }

        static void parseParameterLinks(const std::string_view specification, RuntimeBinding& binding)
        {
            std::size_t start = 0;
            while (start < specification.size())
            {
                const std::size_t end = specification.find(';', start);
                const std::string_view token = specification.substr(start, end == std::string_view::npos ? specification.size() - start : end - start);
                const std::size_t equal = token.find('=');
                if (equal != std::string_view::npos)
                {
                    int slot = -1;
                    std::uint64_t id = 0;
                    const auto [slotPtr, slotEc] = std::from_chars(token.data(), token.data() + equal, slot);
                    const auto [idPtr, idEc] = std::from_chars(token.data() + equal + 1, token.data() + token.size(), id);
                    if (slotEc == std::errc{} && idEc == std::errc{} && slotPtr == token.data() + equal && idPtr == token.data() + token.size() && slot >= 0 && slot < static_cast<int>(RuntimeParameterSlot::Count) && id != 0)
                        binding.ParameterLinks[static_cast<std::size_t>(slot)] = {true, id};
                }
                if (end == std::string_view::npos) break;
                start = end + 1;
            }
        }

        static bool runtimeControlConditionIsEvent(const RuntimeControlCondition condition) noexcept
        {
            return condition == RuntimeControlCondition::RisingEdge || condition == RuntimeControlCondition::FallingEdge || condition == RuntimeControlCondition::OnChange
                || condition == RuntimeControlCondition::ChangedTo || condition == RuntimeControlCondition::ChangedFrom || condition == RuntimeControlCondition::BecomesTrue || condition == RuntimeControlCondition::BecomesFalse;
        }

        static float bankNumericValue(const RuntimeValueBankEntry& value) noexcept
        {
            switch (value.Type)
            {
            case RuntimeBankValueType::Integer: return static_cast<float>(value.Integer);
            case RuntimeBankValueType::Boolean: return value.Boolean ? 1.0f : 0.0f;
            case RuntimeBankValueType::String: return static_cast<float>(std::strlen(value.String));
            case RuntimeBankValueType::Address: return value.Address != 0 ? 1.0f : 0.0f;
            case RuntimeBankValueType::Number: default: return value.Number;
            }
        }

        bool writeBankValue(RuntimeValueBankEntry& target, const float numeric, const std::string_view stringValue = {}, const std::uintptr_t address = 0)
        {
            if (!target.Enabled) return false;
            bool changed = false;
            switch (target.Type)
            {
            case RuntimeBankValueType::Number:
                changed = !target.HasValue || std::abs(target.Number - numeric) > 0.000001f;
                target.Number = numeric;
                break;
            case RuntimeBankValueType::Integer:
            {
                const auto value = static_cast<std::int64_t>(std::llround(numeric));
                changed = !target.HasValue || target.Integer != value;
                target.Integer = value;
                break;
            }
            case RuntimeBankValueType::Boolean:
            {
                const bool value = numeric >= 0.5f;
                changed = !target.HasValue || target.Boolean != value;
                target.Boolean = value;
                break;
            }
            case RuntimeBankValueType::String:
            {
                const std::string value(stringValue);
                changed = !target.HasValue || value != target.String;
                std::snprintf(target.String, sizeof(target.String), "%s", value.c_str());
                break;
            }
            case RuntimeBankValueType::Address:
            {
                const std::uintptr_t value = address != 0 ? address : static_cast<std::uintptr_t>(std::max(numeric, 0.0f));
                changed = !target.HasValue || target.Address != value;
                target.Address = value;
                break;
            }
            }
            target.HasValue = true;
            target.ChangedThisFrame |= changed;
            return changed;
        }

        bool writeBindingToBank(const RuntimeBinding& source, const std::uint64_t bankId)
        {
            RuntimeValueBankEntry* bank = findBankValue(bankId);
            if (!bank) return false;
            return writeBankValue(*bank, source.Value, source.HasString ? std::string_view(source.StringValue) : std::string_view{}, source.HasAddress ? source.AddressValue : 0);
        }

        static void invalidateBindingOutput(RuntimeBinding& binding)
        {
            binding.HasValue = false;
            binding.HasString = false;
            binding.StringValue.clear();
            binding.HasAddress = false;
            binding.AddressValue = 0;
            binding.AddressProvenance.clear();
            binding.LastReadSucceeded = false;
        }

        static void retryBindingRegisterCapture(RuntimeBinding& binding)
        {
            binding.SignatureRegisterCapture.reset();
            binding.SignatureResolvedAddress = 0;
            binding.SignatureCapturedRegister = 0;
            binding.NextRegisterCapture = 0.0;
            binding.NextUpdate = 0.0;
            invalidateBindingOutput(binding);
            if (binding.SignatureInstructionAddress != 0) binding.SignatureStatus = "register recapture requested";
        }

        static bool applyBindingOperation(RuntimeBinding& binding, const RuntimeBindingOperation operation)
        {
            switch (operation)
            {
            case RuntimeBindingOperation::Refresh:
                invalidateBindingOutput(binding);
                binding.NextUpdate = 0.0;
                binding.Error.clear();
                if ((binding.Source == RuntimeSourceKind::NativeProcess || binding.Source == RuntimeSourceKind::NativeAddress) && binding.AddressMode == ProcessAddressMode::Signature)
                {
                    if (binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture && binding.SignatureInstructionAddress != 0) retryBindingRegisterCapture(binding);
                    else { resetRuntimeSignatureScan(binding); binding.SignatureConfigHash = 0; }
                }
                return false;
            case RuntimeBindingOperation::ForceUpdate:
                binding.NextUpdate = 0.0;
                return false;
            case RuntimeBindingOperation::Invalidate:
                invalidateBindingOutput(binding);
                return false;
            case RuntimeBindingOperation::ResetState:
                invalidateBindingOutput(binding);
                binding.RawValue = 0.0f;
                binding.Value = 0.0f;
                binding.LastUpdate = 0.0;
                binding.NextUpdate = 0.0;
                binding.PreviousActionValue = 0.0f;
                binding.PreviousActionString.clear();
                binding.ActionPreviousInitialized = false;
                return false;
            case RuntimeBindingOperation::RetryRegisterCapture:
                if ((binding.Source == RuntimeSourceKind::NativeProcess || binding.Source == RuntimeSourceKind::NativeAddress) && binding.AddressMode == ProcessAddressMode::Signature && binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture)
                {
                    if (binding.SignatureInstructionAddress != 0) retryBindingRegisterCapture(binding);
                    else { resetRuntimeSignatureScan(binding); binding.SignatureConfigHash = 0; binding.NextUpdate = 0.0; invalidateBindingOutput(binding); }
                }
                return false;
            case RuntimeBindingOperation::RescanPattern:
                if ((binding.Source == RuntimeSourceKind::NativeProcess || binding.Source == RuntimeSourceKind::NativeAddress) && binding.AddressMode == ProcessAddressMode::Signature)
                {
                    resetRuntimeSignatureScan(binding);
                    binding.SignatureConfigHash = 0;
                    binding.NextUpdate = 0.0;
                    invalidateBindingOutput(binding);
                    binding.Error.clear();
                }
                return false;
            case RuntimeBindingOperation::RebindProcess:
                if (binding.Source == RuntimeSourceKind::NativeProcess || binding.Source == RuntimeSourceKind::NativeAddress)
                {
                    binding.ProcessId = 0;
                    binding.NextProcessSearch = 0.0;
                    resetRuntimeSignatureScan(binding);
                    binding.SignatureConfigHash = 0;
                    binding.NextUpdate = 0.0;
                    invalidateBindingOutput(binding);
                    binding.Error.clear();
                }
                return false;
            case RuntimeBindingOperation::ClearError:
                binding.Error.clear();
                return false;
            }
            return false;
        }

        std::optional<float> actionNumericValue(const RuntimeAction& action, const RuntimeBinding* source) const noexcept
        {
            switch (action.ValueMode)
            {
            case RuntimeActionValueMode::SourceValue: return source && source->HasValue ? std::optional<float>(source->Value) : std::nullopt;
            case RuntimeActionValueMode::BindingValue:
                if (const RuntimeBinding* binding = findBinding(action.ValueBindingId); binding && binding->HasValue) return binding->Value;
                return std::nullopt;
            case RuntimeActionValueMode::BankValue:
                if (const RuntimeValueBankEntry* bank = findBankValue(action.BankValueId); bank && bank->Enabled && bank->HasValue) return bankNumericValue(*bank);
                return std::nullopt;
            case RuntimeActionValueMode::Constant: default: return action.Value;
            }
        }

        std::string actionStringValue(const RuntimeAction& action, const RuntimeBinding* source) const
        {
            switch (action.ValueMode)
            {
            case RuntimeActionValueMode::SourceValue: return source && source->HasString ? source->StringValue : std::string{};
            case RuntimeActionValueMode::BindingValue:
                if (const RuntimeBinding* binding = findBinding(action.ValueBindingId); binding && binding->HasString) return binding->StringValue;
                return {};
            case RuntimeActionValueMode::BankValue:
                if (const RuntimeValueBankEntry* bank = findBankValue(action.BankValueId); bank && bank->Type == RuntimeBankValueType::String && bank->HasValue) return bank->String;
                return {};
            case RuntimeActionValueMode::Constant: default: return action.StringValue;
            }
        }

        std::uintptr_t actionAddressValue(const RuntimeAction& action, const RuntimeBinding* source) const noexcept
        {
            switch (action.ValueMode)
            {
            case RuntimeActionValueMode::SourceValue: return source && source->HasAddress ? source->AddressValue : 0;
            case RuntimeActionValueMode::BindingValue:
                if (const RuntimeBinding* binding = findBinding(action.ValueBindingId); binding && binding->HasAddress) return binding->AddressValue;
                return 0;
            case RuntimeActionValueMode::BankValue:
                if (const RuntimeValueBankEntry* bank = findBankValue(action.BankValueId); bank && bank->Type == RuntimeBankValueType::Address && bank->HasValue) return bank->Address;
                return 0;
            case RuntimeActionValueMode::Constant: default: return 0;
            }
        }

        bool applyAction(RuntimeAction& action, const RuntimeBinding* source, ShaderFramebuffer& shader, RuntimeControlOutput& output)
        {
            const auto numeric = actionNumericValue(action, source);
            const float value = numeric.value_or(action.Value);
            switch (action.Target)
            {
            case RuntimeActionTarget::ActiveShader:
            {
                std::string shaderId = action.ValueMode == RuntimeActionValueMode::Constant && action.ShaderId[0] ? action.ShaderId : action.ValueMode != RuntimeActionValueMode::Constant ? actionStringValue(action, source) : std::string{};
                int preset = action.ValueMode == RuntimeActionValueMode::Constant ? action.ShaderPresetIndex : static_cast<int>(std::lround(value));
                if (preset == -1) { preset = _previousShaderPreset; if (shaderId.empty()) shaderId = _previousShaderId; }
                if (!shaderId.empty())
                {
                    if (!findShaderPresetById(shaderId)) return false;
                    const bool changed = !output.ShaderId || *output.ShaderId != shaderId; output.ShaderId = shaderId; output.ShaderPresetIndex.reset(); output.ShaderTransitionSeconds = std::clamp(action.TransitionSeconds, 0.0f, 10.0f); return changed;
                }
                if (preset > 0 && preset <= static_cast<int>(ShaderPresets.size())) { const bool changed = !output.ShaderPresetIndex || *output.ShaderPresetIndex != preset; output.ShaderPresetIndex = preset; output.ShaderId = shaderPresetIdByIndex(preset); output.ShaderTransitionSeconds = std::clamp(action.TransitionSeconds, 0.0f, 10.0f); return changed; }
                return false;
            }
            case RuntimeActionTarget::BindingEnabled:
                if (RuntimeBinding* target = findBinding(action.TargetBindingId)) { const bool next = action.ValueMode == RuntimeActionValueMode::Constant ? action.BoolValue : value >= 0.5f; const bool changed = target->RuntimeEnabled != next; target->RuntimeEnabled = next; return changed; }
                return false;
            case RuntimeActionTarget::GlobalBrightness:
            {
                const float next = std::clamp(value, 0.0f, 1.0f); const bool changed = !output.GlobalBrightness || std::abs(*output.GlobalBrightness - next) > 0.000001f; output.GlobalBrightness = next; return changed;
            }
            case RuntimeActionTarget::SendFramebuffer:
            {
                const bool next = action.ValueMode == RuntimeActionValueMode::Constant ? action.BoolValue : value >= 0.5f; const bool changed = !output.SendFramebuffer || *output.SendFramebuffer != next; output.SendFramebuffer = next; return changed;
            }
            case RuntimeActionTarget::BaseColorMode:
            {
                const int next = std::clamp(static_cast<int>(std::lround(value)), 0, 2); const bool changed = !output.BaseColorMode || *output.BaseColorMode != next; output.BaseColorMode = next; return changed;
            }
            case RuntimeActionTarget::MaterialParameter:
                shader.setMaterialParameter(action.TargetId, action.TargetComponent, value);
                return false;
            case RuntimeActionTarget::BindingValue:
                if (RuntimeBinding* target = findBinding(action.TargetBindingId); target && target->Source == RuntimeSourceKind::Unbound)
                {
                    const bool changed = !target->HasValue || std::abs(target->UnboundValue - value) > 0.000001f;
                    target->UnboundValue = value; target->RawValue = value; target->Value = value; target->HasValue = true; target->LastReadSucceeded = true; target->LastSuccessTime = _lastRuntimeTime;
                    return changed;
                }
                return false;
            case RuntimeActionTarget::ValueBank:
                if (RuntimeValueBankEntry* target = findBankValue(action.TargetBankValueId)) return writeBankValue(*target, value, actionStringValue(action, source), actionAddressValue(action, source));
                return false;
            case RuntimeActionTarget::ControlEnabled:
                if (RuntimeControlRule* target = findControl(action.TargetControlId)) { const bool next = action.ValueMode == RuntimeActionValueMode::Constant ? action.BoolValue : value >= 0.5f; const bool changed = target->RuntimeEnabled != next; target->RuntimeEnabled = next; return changed; }
                return false;
            case RuntimeActionTarget::BindingRefresh: if (RuntimeBinding* target = findBinding(action.TargetBindingId)) return applyBindingOperation(*target, RuntimeBindingOperation::Refresh); return false;
            case RuntimeActionTarget::BindingForceUpdate: if (RuntimeBinding* target = findBinding(action.TargetBindingId)) return applyBindingOperation(*target, RuntimeBindingOperation::ForceUpdate); return false;
            case RuntimeActionTarget::BindingInvalidate: if (RuntimeBinding* target = findBinding(action.TargetBindingId)) return applyBindingOperation(*target, RuntimeBindingOperation::Invalidate); return false;
            case RuntimeActionTarget::BindingResetState: if (RuntimeBinding* target = findBinding(action.TargetBindingId)) return applyBindingOperation(*target, RuntimeBindingOperation::ResetState); return false;
            case RuntimeActionTarget::BindingRetryRegisterCapture: if (RuntimeBinding* target = findBinding(action.TargetBindingId)) return applyBindingOperation(*target, RuntimeBindingOperation::RetryRegisterCapture); return false;
            case RuntimeActionTarget::BindingRescanPattern: if (RuntimeBinding* target = findBinding(action.TargetBindingId)) return applyBindingOperation(*target, RuntimeBindingOperation::RescanPattern); return false;
            case RuntimeActionTarget::BindingRebindProcess: if (RuntimeBinding* target = findBinding(action.TargetBindingId)) return applyBindingOperation(*target, RuntimeBindingOperation::RebindProcess); return false;
            case RuntimeActionTarget::BindingClearError: if (RuntimeBinding* target = findBinding(action.TargetBindingId)) return applyBindingOperation(*target, RuntimeBindingOperation::ClearError); return false;
            }
            return false;
        }

        bool applyLegacyControlTarget(RuntimeControlRule& control, const RuntimeBinding& source, ShaderFramebuffer& shader, RuntimeControlOutput& output)
        {
            switch (control.Target)
            {
            case RuntimeControlTarget::ActiveShader:
            {
                std::string shaderId = control.ShaderId[0] ? control.ShaderId : std::string{};
                const int preset = control.ShaderPresetIndex == -1 ? _previousShaderPreset : control.ShaderPresetIndex;
                if (control.ShaderPresetIndex == -1 && shaderId.empty()) shaderId = _previousShaderId;
                if (!shaderId.empty()) { if (!findShaderPresetById(shaderId)) return false; const bool changed = !output.ShaderId || *output.ShaderId != shaderId; output.ShaderId = shaderId; output.ShaderPresetIndex.reset(); output.ShaderTransitionSeconds = std::clamp(control.TransitionSeconds, 0.0f, 10.0f); return changed; }
                if (preset > 0 && preset <= static_cast<int>(ShaderPresets.size())) { const bool changed = !output.ShaderPresetIndex || *output.ShaderPresetIndex != preset; output.ShaderPresetIndex = preset; output.ShaderId = shaderPresetIdByIndex(preset); output.ShaderTransitionSeconds = std::clamp(control.TransitionSeconds, 0.0f, 10.0f); return changed; }
                return false;
            }
            case RuntimeControlTarget::BindingEnabled:
                if (RuntimeBinding* target = findBinding(control.TargetBindingId); target && target != &source) { const bool changed = target->RuntimeEnabled != control.TargetBool; target->RuntimeEnabled = control.TargetBool; return changed; }
                return false;
            case RuntimeControlTarget::GlobalBrightness: { const float next = std::clamp(control.TargetValue, 0.0f, 1.0f); const bool changed = !output.GlobalBrightness || std::abs(*output.GlobalBrightness - next) > 0.000001f; output.GlobalBrightness = next; return changed; }
            case RuntimeControlTarget::SendFramebuffer: { const bool changed = !output.SendFramebuffer || *output.SendFramebuffer != control.TargetBool; output.SendFramebuffer = control.TargetBool; return changed; }
            case RuntimeControlTarget::BaseColorMode: { const int next = std::clamp(static_cast<int>(std::lround(control.TargetValue)), 0, 2); const bool changed = !output.BaseColorMode || *output.BaseColorMode != next; output.BaseColorMode = next; return changed; }
            case RuntimeControlTarget::MaterialParameter: shader.setMaterialParameter(control.TargetId, control.TargetComponent, control.TargetValue); return false;
            case RuntimeControlTarget::BindingValue:
                if (RuntimeBinding* target = findBinding(control.TargetBindingId); target && target != &source && target->Source == RuntimeSourceKind::Unbound)
                {
                    const float next = control.TargetUseSourceValue ? source.Value : control.TargetValue; const bool changed = !target->HasValue || std::abs(target->UnboundValue - next) > 0.000001f;
                    target->UnboundValue = next; target->RawValue = next; target->Value = next; target->HasValue = true; target->LastReadSucceeded = true; target->LastSuccessTime = _lastRuntimeTime; return changed;
                }
                return false;
            case RuntimeControlTarget::ValueBank:
                if (RuntimeValueBankEntry* target = findBankValue(control.TargetBankValueId)) return writeBankValue(*target, control.TargetUseSourceValue ? source.Value : control.TargetValue, control.TargetUseSourceValue && source.HasString ? std::string_view(source.StringValue) : std::string_view{}, control.TargetUseSourceValue && source.HasAddress ? source.AddressValue : 0);
                return false;
            case RuntimeControlTarget::ControlEnabled:
                if (RuntimeControlRule* target = findControl(control.TargetControlId); target && target != &control) { const bool changed = target->RuntimeEnabled != control.TargetBool; target->RuntimeEnabled = control.TargetBool; return changed; }
                return false;
            case RuntimeControlTarget::BindingRefresh: if (RuntimeBinding* target = findBinding(control.TargetBindingId); target && target != &source) return applyBindingOperation(*target, RuntimeBindingOperation::Refresh); return false;
            case RuntimeControlTarget::BindingForceUpdate: if (RuntimeBinding* target = findBinding(control.TargetBindingId); target && target != &source) return applyBindingOperation(*target, RuntimeBindingOperation::ForceUpdate); return false;
            case RuntimeControlTarget::BindingInvalidate: if (RuntimeBinding* target = findBinding(control.TargetBindingId); target && target != &source) return applyBindingOperation(*target, RuntimeBindingOperation::Invalidate); return false;
            case RuntimeControlTarget::BindingResetState: if (RuntimeBinding* target = findBinding(control.TargetBindingId); target && target != &source) return applyBindingOperation(*target, RuntimeBindingOperation::ResetState); return false;
            case RuntimeControlTarget::BindingRetryRegisterCapture: if (RuntimeBinding* target = findBinding(control.TargetBindingId); target && target != &source) return applyBindingOperation(*target, RuntimeBindingOperation::RetryRegisterCapture); return false;
            case RuntimeControlTarget::BindingRescanPattern: if (RuntimeBinding* target = findBinding(control.TargetBindingId); target && target != &source) return applyBindingOperation(*target, RuntimeBindingOperation::RescanPattern); return false;
            case RuntimeControlTarget::BindingRebindProcess: if (RuntimeBinding* target = findBinding(control.TargetBindingId); target && target != &source) return applyBindingOperation(*target, RuntimeBindingOperation::RebindProcess); return false;
            case RuntimeControlTarget::BindingClearError: if (RuntimeBinding* target = findBinding(control.TargetBindingId); target && target != &source) return applyBindingOperation(*target, RuntimeBindingOperation::ClearError); return false;
            }
            return false;
        }

        static bool evaluateControlCondition(RuntimeControlRule& control, const RuntimeBinding& source) noexcept
        {
            const float value = source.Value;
            const float a = control.ValueA;
            const float b = control.ValueB;
            const float lo = std::min(a, b);
            const float hi = std::max(a, b);
            const float h = std::max(control.Hysteresis, 0.0f);
            const float tolerance = std::max(control.Tolerance, 0.000001f);
            bool active = false;

            switch (control.Condition)
            {
            case RuntimeControlCondition::Equal:
                active = std::abs(value - a) <= tolerance + (control.ConditionActive ? h : 0.0f);
                break;
            case RuntimeControlCondition::NotEqual:
                active = std::abs(value - a) > std::max(tolerance - (control.ConditionActive ? h : 0.0f), 0.000001f);
                break;
            case RuntimeControlCondition::Less:
                active = control.ConditionActive ? value < a + h : value < a;
                break;
            case RuntimeControlCondition::LessEqual:
                active = control.ConditionActive ? value <= a + h : value <= a;
                break;
            case RuntimeControlCondition::Greater:
                active = control.ConditionActive ? value > a - h : value > a;
                break;
            case RuntimeControlCondition::GreaterEqual:
                active = control.ConditionActive ? value >= a - h : value >= a;
                break;
            case RuntimeControlCondition::Between:
                active = control.ConditionActive ? value >= lo - h && value <= hi + h : value >= lo && value <= hi;
                break;
            case RuntimeControlCondition::Outside:
                active = control.ConditionActive ? value < lo + h || value > hi - h : value < lo || value > hi;
                break;
            case RuntimeControlCondition::RisingEdge:
                active = control.PreviousInitialized ? control.PreviousValue < a && value >= a : control.FireOnFirstSample && value >= a;
                break;
            case RuntimeControlCondition::FallingEdge:
                active = control.PreviousInitialized ? control.PreviousValue > a && value <= a : control.FireOnFirstSample && value <= a;
                break;
            case RuntimeControlCondition::OnChange:
            {
                const bool numeric = control.PreviousInitialized && std::abs(value - control.PreviousValue) > tolerance;
                const bool text = source.HasString && control.PreviousStringInitialized && source.StringValue != control.PreviousString;
                active = control.PreviousInitialized || control.PreviousStringInitialized ? numeric || text : control.FireOnFirstSample;
                break;
            }
            case RuntimeControlCondition::ChangedTo:
                active = std::abs(value - a) <= tolerance && (control.PreviousInitialized ? std::abs(control.PreviousValue - a) > tolerance : control.FireOnFirstSample);
                break;
            case RuntimeControlCondition::ChangedFrom:
                active = control.PreviousInitialized && std::abs(control.PreviousValue - a) <= tolerance && std::abs(value - a) > tolerance;
                break;
            case RuntimeControlCondition::BecomesTrue:
                active = value >= 0.5f && (control.PreviousInitialized ? control.PreviousValue < 0.5f : control.FireOnFirstSample);
                break;
            case RuntimeControlCondition::BecomesFalse:
                active = value < 0.5f && (control.PreviousInitialized ? control.PreviousValue >= 0.5f : control.FireOnFirstSample);
                break;
            case RuntimeControlCondition::StringEqual:
                active = source.HasString && source.StringValue == control.StringCompare;
                break;
            case RuntimeControlCondition::StringNotEqual:
                active = source.HasString && source.StringValue != control.StringCompare;
                break;
            case RuntimeControlCondition::StringContains:
                active = source.HasString && source.StringValue.find(control.StringCompare) != std::string::npos;
                break;
            }

            control.PreviousValue = value;
            control.PreviousInitialized = true;
            if (source.HasString) { control.PreviousString = source.StringValue; control.PreviousStringInitialized = true; }
            control.ConditionActive = runtimeControlConditionIsEvent(control.Condition) ? false : active;
            return active;
        }

        bool bindingDependsOn(const std::uint64_t startId, const std::uint64_t wantedId, std::set<std::uint64_t>& visited) const
        {
            if (startId == wantedId) return true;
            if (!visited.insert(startId).second) return false;
            const RuntimeBinding* binding = findBinding(startId);
            if (!binding) return false;
            for (const auto& link : binding->ParameterLinks)
                if (link.Enabled && link.BindingId != 0 && bindingDependsOn(link.BindingId, wantedId, visited)) return true;
            return false;
        }

        void validateParameterLinks()
        {
            for (auto& binding : _bindings)
            {
                for (auto& link : binding.ParameterLinks)
                {
                    if (!link.Enabled || link.BindingId == 0) continue;
                    std::set<std::uint64_t> visited;
                    if (!findBinding(link.BindingId) || link.BindingId == binding.Id || bindingDependsOn(link.BindingId, binding.Id, visited)) link = {};
                }
            }
        }

        bool resolveObjectPointer(RuntimeObjectPointer& pointer)
        {
            pointer.Resolved = false; pointer.Address = 0; pointer.ProcessId = 0; pointer.Provenance.clear();
            if (!pointer.Enabled) { pointer.Status = "disabled"; return false; }
            RuntimeObjectDescriptor* descriptor = findObject(pointer.DescriptorId);
            if (!descriptor || !descriptor->Enabled) { pointer.Status = "descriptor model is missing or disabled"; return false; }
            RuntimeBinding* base = findBinding(pointer.BaseBindingId);
            if (!base || !base->Enabled || !base->HasAddress || base->AddressValue == 0) { pointer.Status = "base address binding is not ready"; return false; }
            RuntimeBinding* process = pointer.ProcessBindingId ? findBinding(pointer.ProcessBindingId) : base;
            if (!process || process->ProcessId <= 0 || !runtimeProcessIsAlive(static_cast<pid_t>(process->ProcessId))) { pointer.Status = "process binding is not alive"; return false; }
            pointer.Address = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(base->AddressValue) + pointer.BaseOffset);
            pointer.ProcessId = static_cast<pid_t>(process->ProcessId);
            pointer.Resolved = pointer.Address != 0;
            pointer.Provenance = base->AddressProvenance;
            pointer.Provenance.push_back(std::string("binding ") + base->Name + " -> " + runtimeHexAddress(base->AddressValue));
            if (pointer.BaseOffset != 0) pointer.Provenance.push_back("base offset " + std::to_string(pointer.BaseOffset) + " -> " + runtimeHexAddress(pointer.Address));
            pointer.Provenance.push_back(std::string("model ") + descriptor->Name + " assigned @ " + runtimeHexAddress(pointer.Address));
            if (!pointer.Resolved) { pointer.Status = "resolved address is null"; return false; }
            std::size_t size = 0; runtimeObjectFieldOffset(*descriptor, 0, &size);
            std::ostringstream status; status << runtimeHexAddress(pointer.Address) << "  pid " << pointer.ProcessId << "  model " << descriptor->Name << "  size " << size << " B"; pointer.Status = status.str();
            return true;
        }


        static bool readRuntimeCString(const pid_t pid, const std::uintptr_t address, const std::size_t maxLength, std::string& result, std::string& error)
        {
            result.clear(); if (address == 0) { error = "null string pointer"; return false; }
            const std::size_t cap = std::clamp<std::size_t>(maxLength, 1, 4096); std::vector<std::uint8_t> bytes(cap);
            iovec local{bytes.data(), bytes.size()}; iovec remote{reinterpret_cast<void*>(address), bytes.size()}; errno = 0;
            const ssize_t count = ::process_vm_readv(pid, &local, 1, &remote, 1, 0); if (count <= 0) { error = std::strerror(errno); return false; }
            const auto end = std::find(bytes.begin(), bytes.begin() + count, 0); result.assign(reinterpret_cast<const char*>(bytes.data()), static_cast<std::size_t>(end - bytes.begin())); error.clear(); return true;
        }

        static bool readRuntimeWString(const pid_t pid, const std::uintptr_t address, const std::size_t maxLength, std::string& result, std::string& error)
        {
            result.clear(); if (address == 0) { error = "null wide string pointer"; return false; }
            const std::size_t cap = std::clamp<std::size_t>(maxLength, 1, 2048); std::vector<wchar_t> chars(cap);
            iovec local{chars.data(), chars.size() * sizeof(wchar_t)}; iovec remote{reinterpret_cast<void*>(address), chars.size() * sizeof(wchar_t)}; errno = 0;
            const ssize_t count = ::process_vm_readv(pid, &local, 1, &remote, 1, 0); if (count <= 0) { error = std::strerror(errno); return false; }
            const std::size_t available = static_cast<std::size_t>(count) / sizeof(wchar_t);
            for (std::size_t i = 0; i < available && chars[i] != 0; ++i)
            {
                const std::uint32_t cp = static_cast<std::uint32_t>(chars[i]);
                if (cp < 0x80) result.push_back(static_cast<char>(cp));
                else if (cp < 0x800) { result.push_back(static_cast<char>(0xC0 | (cp >> 6))); result.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                else if (cp < 0x10000) { result.push_back(static_cast<char>(0xE0 | (cp >> 12))); result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); result.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                else { result.push_back('?'); }
            }
            error.clear(); return true;
        }

        bool readObjectField(RuntimeBinding& binding, float& output)
        {
            binding.HasAddress = false; binding.AddressProvenance.clear(); binding.HasString = false; binding.StringValue.clear();
            RuntimeObjectPointer* pointer = findPointer(binding.ObjectPointerId);
            RuntimeObjectDescriptor* object = binding.ObjectId ? findObject(binding.ObjectId) : pointer ? findObject(pointer->DescriptorId) : nullptr;
            if (!pointer && object)
                for (auto& candidate : _pointers) if (candidate.DescriptorId == object->Id && candidate.Enabled) { pointer = &candidate; binding.ObjectPointerId = candidate.Id; break; }
            if (!object) { binding.Error = "object descriptor model is missing"; return false; }
            if (!pointer) { binding.Error = "object has no pointer assignment"; return false; }
            if (!resolveObjectPointer(*pointer)) { binding.Error = pointer->Status; return false; }
            RuntimeObjectField* field = findObjectField(*object, binding.ObjectFieldId);
            if (!field || !field->Enabled) { binding.Error = "object field is missing or disabled"; return false; }
            if (runtimeObjectFieldIsFiller(field->Type)) { binding.Error = "filler fields are layout-only and cannot be read"; return false; }
            const std::size_t offset = runtimeObjectFieldOffset(*object, field->Id, &object->Size);
            if (offset == std::numeric_limits<std::size_t>::max()) { binding.Error = "object field offset could not be resolved"; return false; }
            const std::uintptr_t address = pointer->Address + offset; binding.ProcessId = static_cast<int>(pointer->ProcessId); binding.AddressValue = address; binding.HasAddress = true; binding.AddressProvenance = pointer->Provenance;
            binding.AddressProvenance.push_back(std::string(object->Name) + "." + field->Name + " +0x" + [&]{ std::ostringstream ss; ss << std::hex << offset; return ss.str(); }() + " @ " + runtimeHexAddress(address));
            std::string error;
#define QUARTZ_READ_OBJECT(type) do { type value{}; if (!readProcessMemoryValue(pointer->ProcessId, address, value, error)) { binding.HasAddress = false; binding.Error = std::move(error); return false; } output = static_cast<float>(value); } while (false)
            switch (field->Type)
            {
            case RuntimeObjectFieldType::U8: QUARTZ_READ_OBJECT(std::uint8_t); break;
            case RuntimeObjectFieldType::I8: QUARTZ_READ_OBJECT(std::int8_t); break;
            case RuntimeObjectFieldType::U16: QUARTZ_READ_OBJECT(std::uint16_t); break;
            case RuntimeObjectFieldType::I16: QUARTZ_READ_OBJECT(std::int16_t); break;
            case RuntimeObjectFieldType::U32: QUARTZ_READ_OBJECT(std::uint32_t); break;
            case RuntimeObjectFieldType::I32: QUARTZ_READ_OBJECT(std::int32_t); break;
            case RuntimeObjectFieldType::U64: QUARTZ_READ_OBJECT(std::uint64_t); break;
            case RuntimeObjectFieldType::I64: QUARTZ_READ_OBJECT(std::int64_t); break;
            case RuntimeObjectFieldType::Float: QUARTZ_READ_OBJECT(float); break;
            case RuntimeObjectFieldType::Double: { double value{}; if (!readProcessMemoryValue(pointer->ProcessId, address, value, error)) { binding.HasAddress = false; binding.Error = std::move(error); return false; } output = static_cast<float>(value); break; }
            case RuntimeObjectFieldType::Bool: { std::uint8_t value{}; if (!readProcessMemoryValue(pointer->ProcessId, address, value, error)) { binding.HasAddress = false; binding.Error = std::move(error); return false; } output = value ? 1.0f : 0.0f; break; }
            case RuntimeObjectFieldType::Pointer: { std::uintptr_t value{}; if (!readProcessMemoryValue(pointer->ProcessId, address, value, error)) { binding.HasAddress = false; binding.Error = std::move(error); return false; } binding.AddressValue = value; binding.HasAddress = value != 0; output = value ? 1.0f : 0.0f; binding.AddressProvenance.push_back("dereference -> " + runtimeHexAddress(value)); break; }
            case RuntimeObjectFieldType::CStringPointer:
            case RuntimeObjectFieldType::WStringPointer:
            {
                std::uintptr_t value{}; if (!readProcessMemoryValue(pointer->ProcessId, address, value, error)) { binding.HasAddress = false; binding.Error = std::move(error); return false; }
                binding.AddressValue = value; binding.HasAddress = value != 0; binding.AddressProvenance.push_back("string pointer -> " + runtimeHexAddress(value));
                const bool ok = field->Type == RuntimeObjectFieldType::CStringPointer ? readRuntimeCString(pointer->ProcessId, value, field->StringMaxLength, binding.StringValue, error) : readRuntimeWString(pointer->ProcessId, value, field->StringMaxLength, binding.StringValue, error);
                if (!ok) { binding.Error = error; return false; } binding.HasString = true; output = static_cast<float>(binding.StringValue.size()); break;
            }
            case RuntimeObjectFieldType::FixedCString:
            case RuntimeObjectFieldType::FixedWString:
            {
                const bool ok = field->Type == RuntimeObjectFieldType::FixedCString ? readRuntimeCString(pointer->ProcessId, address, field->FixedElementCount, binding.StringValue, error) : readRuntimeWString(pointer->ProcessId, address, field->FixedElementCount, binding.StringValue, error);
                if (!ok) { binding.Error = error; return false; } binding.HasString = true; output = static_cast<float>(binding.StringValue.size()); break;
            }
            default: binding.Error = "unsupported object field type"; return false;
            }
#undef QUARTZ_READ_OBJECT
            binding.Error.clear(); return true;
        }

        std::optional<float> referenceValue(const RuntimeSourceReference& reference) const noexcept
        {
            if (!reference.Enabled || reference.Id == 0) return std::nullopt;
            if (reference.Kind == RuntimeReferenceKind::Binding)
            {
                const RuntimeBinding* binding = findBinding(reference.Id);
                if (!binding) return std::nullopt;
                switch (reference.Signal)
                {
                case 1: return binding->RawValue;
                case 2: return binding->HasValue ? 1.0f : 0.0f;
                case 3: return binding->LastReadSucceeded ? 1.0f : 0.0f;
                case 4: return binding->Enabled && binding->RuntimeEnabled ? 1.0f : 0.0f;
                case 5: return static_cast<float>(binding->Priority);
                case 6: return binding->HasAddress ? 1.0f : 0.0f;
                case 7: return binding->Error.empty() ? 0.0f : 1.0f;
                case 8: return binding->LastSuccessTime > 0.0 ? static_cast<float>(std::max(_lastRuntimeTime - binding->LastSuccessTime, 0.0)) : std::numeric_limits<float>::infinity();
                default: return binding->HasValue ? std::optional<float>(binding->Value) : std::nullopt;
                }
            }
            const RuntimeControlRule* control = findControl(reference.Id);
            if (!control) return std::nullopt;
            switch (reference.Signal)
            {
            case 1: return control->TriggeredThisFrame ? 1.0f : 0.0f;
            case 2: return control->Enabled ? 1.0f : 0.0f;
            case 3: return static_cast<float>(control->TriggerCount);
            case 4: return static_cast<float>(control->Priority);
            case 5: return control->LastTriggerTime > 0.0 ? static_cast<float>(std::max(_lastRuntimeTime - control->LastTriggerTime, 0.0)) : std::numeric_limits<float>::infinity();
            case 6:
            {
                const RuntimeBinding* source = findBinding(control->SourceBindingId);
                return source && source->HasValue ? std::optional<float>(source->Value) : std::nullopt;
            }
            default: return control->ConditionActive ? 1.0f : 0.0f;
            }
        }

        static bool compareRuntimeValue(const RuntimeCompareCondition condition, const float value, const float a, const float b, const float tolerance) noexcept
        {
            const float lo = std::min(a, b), hi = std::max(a, b), t = std::max(tolerance, 0.000001f);
            switch (condition)
            {
            case RuntimeCompareCondition::Equal: return std::abs(value - a) <= t;
            case RuntimeCompareCondition::NotEqual: return std::abs(value - a) > t;
            case RuntimeCompareCondition::Less: return value < a;
            case RuntimeCompareCondition::LessEqual: return value <= a;
            case RuntimeCompareCondition::Greater: return value > a;
            case RuntimeCompareCondition::GreaterEqual: return value >= a;
            case RuntimeCompareCondition::Between: return value >= lo && value <= hi;
            case RuntimeCompareCondition::Outside: return value < lo || value > hi;
            }
            return false;
        }

        bool readAggregate(RuntimeBinding& binding, float& output) const
        {
            std::vector<std::pair<float, float>> values;
            values.reserve(binding.References.size());
            for (const auto& reference : binding.References)
                if (const auto value = referenceValue(reference)) values.emplace_back(*value, reference.Weight);
            if (values.empty()) { binding.Error = "aggregate has no readable members"; return false; }
            switch (binding.AggregateOperation)
            {
            case RuntimeAggregateOperation::Sum:
                output = std::accumulate(values.begin(), values.end(), 0.0f, [](const float total, const auto& item) { return total + item.first * item.second; });
                break;
            case RuntimeAggregateOperation::Average:
            {
                float weighted = 0.0f, weights = 0.0f;
                for (const auto& [value, weight] : values) { weighted += value * weight; weights += std::abs(weight); }
                output = weights > 0.000001f ? weighted / weights : 0.0f;
                break;
            }
            case RuntimeAggregateOperation::Minimum:
                output = std::min_element(values.begin(), values.end(), [](const auto& a, const auto& b) { return a.first < b.first; })->first;
                break;
            case RuntimeAggregateOperation::Maximum:
                output = std::max_element(values.begin(), values.end(), [](const auto& a, const auto& b) { return a.first < b.first; })->first;
                break;
            case RuntimeAggregateOperation::Product:
                output = 1.0f; for (const auto& [value, weight] : values) output *= value * weight;
                break;
            case RuntimeAggregateOperation::Count: output = static_cast<float>(values.size()); break;
            case RuntimeAggregateOperation::CountTruthy: output = static_cast<float>(std::count_if(values.begin(), values.end(), [](const auto& item) { return item.first >= 0.5f; })); break;
            case RuntimeAggregateOperation::FractionTruthy: output = static_cast<float>(std::count_if(values.begin(), values.end(), [](const auto& item) { return item.first >= 0.5f; })) / static_cast<float>(values.size()); break;
            case RuntimeAggregateOperation::Any: output = std::ranges::any_of(values, [](const auto& item) { return item.first >= 0.5f; }) ? 1.0f : 0.0f; break;
            case RuntimeAggregateOperation::All: output = std::ranges::all_of(values, [](const auto& item) { return item.first >= 0.5f; }) ? 1.0f : 0.0f; break;
            }
            binding.Error.clear();
            return true;
        }

        bool readMassCompare(RuntimeBinding& binding, float& output) const
        {
            std::vector<bool> matches;
            matches.reserve(binding.References.size());
            for (const auto& reference : binding.References)
            {
                const auto value = referenceValue(reference);
                if (!value) continue;
                const RuntimeCompareCondition condition = reference.UseOwnComparison ? reference.CompareCondition : binding.CompareCondition;
                const float a = reference.UseOwnComparison ? reference.CompareA : binding.CompareA;
                const float b = reference.UseOwnComparison ? reference.CompareB : binding.CompareB;
                const float tolerance = reference.UseOwnComparison ? reference.CompareTolerance : binding.CompareTolerance;
                matches.push_back(compareRuntimeValue(condition, *value, a, b, tolerance));
            }
            if (matches.empty()) { binding.Error = "mass compare has no readable members"; return false; }
            const std::size_t count = static_cast<std::size_t>(std::count(matches.begin(), matches.end(), true));
            switch (binding.CompareResult)
            {
            case RuntimeMassCompareResult::Any: output = count != 0 ? 1.0f : 0.0f; break;
            case RuntimeMassCompareResult::All: output = count == matches.size() ? 1.0f : 0.0f; break;
            case RuntimeMassCompareResult::None: output = count == 0 ? 1.0f : 0.0f; break;
            case RuntimeMassCompareResult::Count: output = static_cast<float>(count); break;
            case RuntimeMassCompareResult::Fraction: output = static_cast<float>(count) / static_cast<float>(matches.size()); break;
            case RuntimeMassCompareResult::FirstMatchIndex:
            {
                const auto it = std::find(matches.begin(), matches.end(), true);
                output = it == matches.end() ? -1.0f : static_cast<float>(std::distance(matches.begin(), it));
                break;
            }
            }
            binding.Error.clear();
            return true;
        }

        bool readSource(RuntimeBinding& binding, const RuntimeSignalContext& context, float& output)
        {
            const auto signal = std::max(binding.Signal, 0);
            switch (binding.Source)
            {
            case RuntimeSourceKind::Constant:
                output = binding.Constant;
                return true;
            case RuntimeSourceKind::Unbound:
                output = binding.UnboundValue;
                binding.Error.clear();
                return true;
            case RuntimeSourceKind::Time:
                if (signal == 1) output = std::sin(static_cast<float>(context.Time));
                else if (signal == 2) output = std::cos(static_cast<float>(context.Time));
                else if (signal == 3) output = static_cast<float>(context.Time - std::floor(context.Time));
                else if (signal == 4)
                {
                    const float saw = static_cast<float>(context.Time - std::floor(context.Time));
                    output = 1.0f - std::abs(saw * 2.0f - 1.0f);
                }
                else if (signal == 5) output = std::fmod(context.Time, 1.0) < 0.5 ? 1.0f : 0.0f;
                else if (signal == 6) output = std::fmod(context.Time, 0.5) < 0.25 ? 1.0f : 0.0f;
                else output = static_cast<float>(context.Time);
                return true;
            case RuntimeSourceKind::Audio:
                if (signal == 1) output = context.Audio.Peak;
                else if (signal == 2) output = context.SmoothedBands ? std::accumulate(context.SmoothedBands->begin(), context.SmoothedBands->begin() + 4, 0.0f) / 4.0f : 0.0f;
                else if (signal == 3) output = context.SmoothedBands ? std::accumulate(context.SmoothedBands->begin() + 4, context.SmoothedBands->begin() + 11, 0.0f) / 7.0f : 0.0f;
                else if (signal == 4) output = context.SmoothedBands ? std::accumulate(context.SmoothedBands->begin() + 11, context.SmoothedBands->end(), 0.0f) / 5.0f : 0.0f;
                else if (signal == 5) output = context.EffectiveGain;
                else if (signal == 6) output = context.GainCorrection;
                else if (signal == 7) output = context.SmoothedBands ? std::accumulate(context.SmoothedBands->begin(), context.SmoothedBands->end(), 0.0f) / static_cast<float>(context.SmoothedBands->size()) : 0.0f;
                else if (signal == 8) output = context.SmoothedBands ? *std::max_element(context.SmoothedBands->begin(), context.SmoothedBands->end()) : 0.0f;
                else if (signal == 9)
                {
                    if (!context.SmoothedBands) output = 0.0f;
                    else
                    {
                        const float bass = std::accumulate(context.SmoothedBands->begin(), context.SmoothedBands->begin() + 4, 0.0f) / 4.0f;
                        const float treble = std::accumulate(context.SmoothedBands->begin() + 11, context.SmoothedBands->end(), 0.0f) / 5.0f;
                        output = bass / std::max(treble, 0.0001f);
                    }
                }
                else if (signal == 10) output = context.SmoothedBands ? *std::max_element(context.SmoothedBands->begin(), context.SmoothedBands->begin() + 4) : 0.0f;
                else output = context.Audio.Rms;
                return true;
            case RuntimeSourceKind::Media:
            {
                const Color32 color = context.MediaColor.value_or(Color32{0, 0, 0});
                if (signal == 1) output = color.R / 255.0f;
                else if (signal == 2) output = color.G / 255.0f;
                else if (signal == 3) output = color.B / 255.0f;
                else if (signal == 4) output = context.MediaPlaying ? 1.0f : 0.0f;
                else if (signal == 5) { binding.StringValue = context.MediaTitle; binding.HasString = true; output = static_cast<float>(binding.StringValue.size()); }
                else output = context.MediaAmount;
                return true;
            }
            case RuntimeSourceKind::Keyboard:
                if (signal == 0) output = context.Keys.CapsLockActive ? 1.0f : 0.0f;
                else if (signal == 1) output = context.Keys.ScrollLockActive ? 1.0f : 0.0f;
                else if (signal == 2) output = std::accumulate(context.Keys.Down.begin(), context.Keys.Down.end(), 0.0f) / static_cast<float>(MatrixSize);
                else if (signal == 3)
                {
                    float pulse = 0.0f;
                    for (const auto& event : context.Keys.Events) if (event.Valid > 0.5f) pulse = std::max(pulse, std::exp(-std::max(static_cast<float>(context.Time) - event.Time, 0.0f) * 6.0f));
                    output = pulse;
                }
                else if (signal == 4) output = std::accumulate(context.Keys.Down.begin(), context.Keys.Down.end(), 0.0f);
                else if (signal == 5) output = static_cast<float>(std::count_if(context.Keys.Events.begin(), context.Keys.Events.end(), [](const auto& event) { return event.Valid > 0.5f; }));
                else
                {
                    const auto it = std::max_element(context.Keys.Events.begin(), context.Keys.Events.end(), [](const auto& a, const auto& b) { return a.Valid < b.Valid || (a.Valid == b.Valid && a.Time < b.Time); });
                    if (it == context.Keys.Events.end() || it->Valid <= 0.5f) output = -1.0f;
                    else output = signal == 6 ? it->Column : it->Row;
                }
                return true;
            case RuntimeSourceKind::RPC:
                if (!context.HasPerformance || context.Performance.CoreClock == 0) { binding.Error = "no firmware performance data"; return false; }
                if (signal == 1) output = context.Performance.AverageScanPeriodTicks ? static_cast<float>(context.Performance.CoreClock) / context.Performance.AverageScanPeriodTicks : 0.0f;
                else if (signal == 2) output = (context.Performance.BeginScanTicks + context.Performance.ScanTicks + context.Performance.EndScanTicks) * 1'000'000.0f / context.Performance.CoreClock;
                else if (signal == 3) output = context.Performance.RGBTicks * 1'000'000.0f / context.Performance.CoreClock;
                else if (signal == 4) output = context.Performance.AverageScanPeriodTicks * 1'000'000.0f / context.Performance.CoreClock;
                else if (signal == 5) output = context.Performance.StateUpdateTicks * 1'000'000.0f / context.Performance.CoreClock;
                else if (signal == 6) output = context.Performance.HIDTicks * 1'000'000.0f / context.Performance.CoreClock;
                else if (signal == 7)
                {
                    const std::uint32_t total = context.Performance.BeginScanTicks + context.Performance.ScanTicks + context.Performance.EndScanTicks + context.Performance.StateUpdateTicks + context.Performance.HIDTicks + context.Performance.RGBTicks;
                    output = total * 1'000'000.0f / context.Performance.CoreClock;
                }
                else
                {
                    const std::uint32_t total = context.Performance.BeginScanTicks + context.Performance.ScanTicks + context.Performance.EndScanTicks + context.Performance.StateUpdateTicks + context.Performance.HIDTicks;
                    output = context.Performance.AverageScanPeriodTicks ? total * 100.0f / context.Performance.AverageScanPeriodTicks : 0.0f;
                }
                return true;
            case RuntimeSourceKind::Host:
                if (signal == 1) output = context.DeltaTime * 1000.0f;
                else if (signal == 2) output = context.DeltaTime > 0.000001f ? 1.0f / context.DeltaTime : 0.0f;
                else output = context.AppCpu;
                return true;
            case RuntimeSourceKind::USB:
                if (signal == 0) output = context.USBConnected ? 1.0f : 0.0f;
                else if (signal == 1) output = static_cast<float>(context.USBRates.TxKiB);
                else if (signal == 2) output = static_cast<float>(context.USBRates.RxKiB);
                else if (signal == 3) output = static_cast<float>(context.USBRates.TxTransfers);
                else if (signal == 4) output = static_cast<float>(context.USBRates.RxTransfers);
                else if (signal == 5) output = static_cast<float>(context.USB.TxErrors + context.USB.RxErrors);
                else if (signal == 6) output = static_cast<float>(context.USB.TxBytes / (1024.0 * 1024.0));
                else if (signal == 7) output = static_cast<float>(context.USB.RxBytes / (1024.0 * 1024.0));
                else if (signal == 8) output = static_cast<float>(context.USB.TxErrors);
                else output = static_cast<float>(context.USB.RxErrors);
                return true;
            case RuntimeSourceKind::RGB:
            {
                if (!context.Framebuffer) { binding.Error = "no framebuffer"; return false; }
                float r = 0.0f, g = 0.0f, b = 0.0f, lit = 0.0f, peak = 0.0f, peakLuma = 0.0f;
                for (std::size_t row = 0; row < ActiveProbeRows; ++row)
                    for (std::size_t column = 0; column < Columns; ++column)
                    {
                        const auto& color = (*context.Framebuffer)[row * Columns + column];
                        r += color.R; g += color.G; b += color.B;
                        if (color.R || color.G || color.B) lit += 1.0f;
                        peak = std::max(peak, std::max({color.R, color.G, color.B}) / 255.0f);
                        peakLuma = std::max(peakLuma, (color.R * 0.2126f + color.G * 0.7152f + color.B * 0.0722f) / 255.0f);
                    }
                constexpr float Count = static_cast<float>(ActiveProbeRows * Columns);
                r /= 255.0f * Count; g /= 255.0f * Count; b /= 255.0f * Count;
                if (signal == 1) output = lit / Count;
                else if (signal == 2) output = r;
                else if (signal == 3) output = g;
                else if (signal == 4) output = b;
                else if (signal == 5) output = peak;
                else if (signal == 6) output = peakLuma;
                else output = r * 0.2126f + g * 0.7152f + b * 0.0722f;
                return true;
            }
            case RuntimeSourceKind::NativeProcess:
                return readNativeBinding(binding, output);
            case RuntimeSourceKind::NativeAddress:
                return readNativeAddressBinding(binding, output);
            case RuntimeSourceKind::BindingStatus:
            {
                RuntimeBinding* target = findBinding(binding.StatusBindingId);
                if (!target || target == &binding) { binding.Error = "status binding target is missing"; return false; }
                if (signal == 0) output = target->HasValue ? 1.0f : 0.0f;
                else if (signal == 1) output = target->LastReadSucceeded ? 1.0f : 0.0f;
                else if (signal == 2) output = target->Enabled && target->RuntimeEnabled ? 1.0f : 0.0f;
                else if (signal == 3) output = target->ProcessId > 0 && runtimeProcessIsAlive(static_cast<pid_t>(target->ProcessId)) ? 1.0f : 0.0f;
                else if (signal == 4)
                {
                    if (target->Source != RuntimeSourceKind::NativeProcess && target->Source != RuntimeSourceKind::NativeAddress) output = target->HasValue ? 1.0f : 0.0f;
                    else if (target->AddressMode == ProcessAddressMode::Signature) output = target->SignatureResolvedAddress != 0 ? 1.0f : 0.0f;
                    else output = target->LastReadSucceeded ? 1.0f : 0.0f;
                }
                else if (signal == 5) output = (target->Source == RuntimeSourceKind::NativeProcess || target->Source == RuntimeSourceKind::NativeAddress) && target->AddressMode == ProcessAddressMode::Signature && target->SignatureResolve == SignatureResultMode::RegisterRelativeCapture && target->SignatureCapturedRegister != 0 ? 1.0f : 0.0f;
                else if (signal == 6) output = target->Error.empty() ? 0.0f : 1.0f;
                else if (signal == 7) output = target->LastSuccessTime > 0.0 ? static_cast<float>(std::max(context.Time - target->LastSuccessTime, 0.0)) : std::numeric_limits<float>::infinity();
                else if (signal == 8) output = static_cast<float>(target->Priority);
                else output = target->HasAddress ? 1.0f : 0.0f;
                binding.Error.clear();
                return true;
            }
            case RuntimeSourceKind::BindingValue:
            {
                RuntimeBinding* target = findBinding(binding.ValueBindingId);
                if (!target || target == &binding) { binding.HasAddress = false; binding.AddressValue = 0; binding.Error = "source binding is missing"; return false; }
                binding.HasAddress = target->HasAddress;
                binding.AddressValue = target->HasAddress ? target->AddressValue : 0;
                binding.AddressProvenance = target->AddressProvenance;
                binding.HasString = target->HasString;
                if (target->HasString) binding.StringValue = target->StringValue;
                if (target->ProcessId > 0) binding.ProcessId = target->ProcessId;
                if (signal == 1) output = target->RawValue;
                else if (signal == 2) output = target->HasValue ? 1.0f : 0.0f;
                else if (signal == 3) output = target->LastReadSucceeded ? 1.0f : 0.0f;
                else if (signal == 4) output = target->Enabled && target->RuntimeEnabled ? 1.0f : 0.0f;
                else if (signal == 5) output = static_cast<float>(target->Priority);
                else
                {
                    if (!target->HasValue) { binding.Error = "source binding has no value yet"; return false; }
                    output = target->Value;
                }
                binding.Error.clear();
                return true;
            }
            case RuntimeSourceKind::ShaderState:
                binding.HasString = signal == 0 || signal == 9;
                if (signal == 0) binding.StringValue = context.CurrentShaderId; else if (signal == 9) binding.StringValue = _previousShaderId;
                if (signal == 1) output = context.CurrentShaderPreset == 0 ? 1.0f : 0.0f;
                else if (signal == 2) output = context.ShaderTransitionActive ? 1.0f : 0.0f;
                else if (signal == 3) output = context.ShaderTransitionProgress;
                else if (signal == 4) output = static_cast<float>(context.BaseColorMode);
                else if (signal == 5) output = context.GlobalBrightness;
                else if (signal == 6) output = context.SendFramebuffer ? 1.0f : 0.0f;
                else if (signal == 7) output = static_cast<float>(context.ShaderFramebufferWidth);
                else if (signal == 8) output = static_cast<float>(context.ShaderFramebufferHeight);
                else if (signal == 9) output = static_cast<float>(_previousShaderPreset);
                else output = static_cast<float>(context.CurrentShaderPreset);
                return true;
            case RuntimeSourceKind::ControlStatus:
            {
                RuntimeControlRule* control = findControl(binding.ControlStatusId);
                if (!control) { binding.Error = "control target is missing"; return false; }
                if (signal == 1) output = control->TriggeredThisFrame ? 1.0f : 0.0f;
                else if (signal == 2) output = control->Enabled && control->RuntimeEnabled ? 1.0f : 0.0f;
                else if (signal == 3) output = static_cast<float>(control->TriggerCount);
                else if (signal == 4) output = control->LastTriggerTime > 0.0 ? static_cast<float>(std::max(context.Time - control->LastTriggerTime, 0.0)) : std::numeric_limits<float>::infinity();
                else if (signal == 5) output = static_cast<float>(control->Priority);
                else if (signal == 6)
                {
                    const RuntimeBinding* sourceBinding = findBinding(control->SourceBindingId);
                    if (!sourceBinding || !sourceBinding->HasValue) { binding.Error = "control source binding has no value"; return false; }
                    output = sourceBinding->Value;
                }
                else output = control->ConditionActive ? 1.0f : 0.0f;
                binding.Error.clear();
                return true;
            }
            case RuntimeSourceKind::Aggregate:
                return readAggregate(binding, output);
            case RuntimeSourceKind::MassCompare:
                return readMassCompare(binding, output);
            case RuntimeSourceKind::ObjectField:
                return readObjectField(binding, output);
            case RuntimeSourceKind::ObjectStatus:
            {
                RuntimeObjectPointer* pointer = findPointer(binding.ObjectPointerId);
                if (!pointer && binding.ObjectId != 0)
                {
                    const auto it = std::ranges::find_if(_pointers, [&](const RuntimeObjectPointer& candidate) { return candidate.DescriptorId == binding.ObjectId; });
                    if (it != _pointers.end()) { pointer = &*it; binding.ObjectPointerId = pointer->Id; }
                }
                if (!pointer) { binding.HasAddress = false; binding.AddressValue = 0; binding.Error = "pointer assignment is missing"; output = 0.0f; return signal == 0; }
                RuntimeObjectDescriptor* object = findObject(pointer->DescriptorId);
                if (!object) { binding.HasAddress = false; binding.AddressValue = 0; binding.Error = "descriptor model is missing"; output = 0.0f; return signal == 0; }
                const bool resolved = resolveObjectPointer(*pointer);
                runtimeObjectFieldOffset(*object, 0, &object->Size);
                binding.ObjectId = object->Id;
                binding.HasAddress = resolved;
                binding.AddressValue = resolved ? pointer->Address : 0;
                binding.AddressProvenance = pointer->Provenance;
                if (pointer->ProcessId > 0) binding.ProcessId = pointer->ProcessId;
                if (signal == 1) output = static_cast<float>(object->Size);
                else if (signal == 2) output = static_cast<float>(std::count_if(object->Fields.begin(), object->Fields.end(), [](const auto& field) { return field.Enabled; }));
                else if (signal == 3) output = pointer->ProcessId > 0 && runtimeProcessIsAlive(pointer->ProcessId) ? 1.0f : 0.0f;
                else if (signal == 4) { const RuntimeBinding* base = findBinding(pointer->BaseBindingId); output = base && base->HasAddress ? 1.0f : 0.0f; }
                else output = resolved ? 1.0f : 0.0f;
                if (!resolved) binding.Error = pointer->Status; else binding.Error.clear();
                return true;
            }
            case RuntimeSourceKind::ValueBank:
            {
                RuntimeValueBankEntry* value = findBankValue(binding.BankValueId);
                if (!value || !value->Enabled || !value->HasValue) { binding.Error = "value bank entry is missing, disabled, or empty"; return false; }
                binding.HasAddress = value->Type == RuntimeBankValueType::Address && value->Address != 0;
                binding.AddressValue = binding.HasAddress ? value->Address : 0;
                binding.HasString = value->Type == RuntimeBankValueType::String;
                if (binding.HasString) binding.StringValue = value->String;
                if (signal == 1) output = value->Type == RuntimeBankValueType::Boolean ? (value->Boolean ? 1.0f : 0.0f) : bankNumericValue(*value) >= 0.5f ? 1.0f : 0.0f;
                else if (signal == 2) output = value->Type == RuntimeBankValueType::Integer ? static_cast<float>(value->Integer) : bankNumericValue(*value);
                else if (signal == 3) output = value->HasValue ? 1.0f : 0.0f;
                else if (signal == 4) output = value->ChangedThisFrame ? 1.0f : 0.0f;
                else if (signal == 5) output = value->Type == RuntimeBankValueType::String ? static_cast<float>(std::strlen(value->String)) : 0.0f;
                else if (signal == 6) output = value->Type == RuntimeBankValueType::Address && value->Address != 0 ? 1.0f : 0.0f;
                else output = bankNumericValue(*value);
                binding.Error.clear();
                return true;
            }
            case RuntimeSourceKind::StringConstant:
                binding.StringValue = binding.StringConstant; binding.HasString = true; output = signal == 1 ? static_cast<float>(binding.StringValue.size()) : (binding.StringValue.empty() ? 0.0f : 1.0f); binding.Error.clear(); return true;
            case RuntimeSourceKind::ProfileState:
            {
                if (signal == 0) { output = static_cast<float>(_activeProfileId); binding.Error.clear(); return true; }
                const RuntimeBindingProfile* profile = findProfile(binding.ProfileId);
                if (!profile) { binding.Error = "profile is missing"; return false; }
                if (signal == 1) output = _activeProfileId == profile->Id ? 1.0f : 0.0f;
                else if (signal == 2) output = profile->Enabled ? 1.0f : 0.0f;
                else if (signal == 3) output = static_cast<float>(profile->BindingIds.size());
                else output = static_cast<float>(profile->ControlIds.size());
                binding.StringValue = profile->Name; binding.HasString = true; binding.Error.clear(); return true;
            }
            }
            return false;
        }

        std::filesystem::path _path = runtimeBindingsPath();
        std::vector<RuntimeBinding> _bindings;
        std::vector<RuntimeControlRule> _controls;
        std::vector<RuntimeObjectDescriptor> _objects;
        std::vector<RuntimeObjectPointer> _pointers;
        std::vector<RuntimeValueBankEntry> _bank;
        std::vector<RuntimeBindingProfile> _profiles;
        std::uint64_t _nextBindingId = 1;
        std::uint64_t _nextControlId = 1;
        std::uint64_t _nextBankValueId = 1;
        std::uint64_t _nextProfileId = 1;
        std::uint64_t _activeProfileId = 0;
        std::uint64_t _nextObjectId = 1;
        std::uint64_t _nextObjectFieldId = 1;
        std::uint64_t _nextPointerId = 1;
        int _controlPassLimit = 6;
        std::uint64_t _revision = 0;
        std::uint64_t _savedRevision = 0;
        USBStatsSnapshot _lastUSB{};
        RuntimeUSBRates _usbRates{};
        RuntimeControlOutput _pendingOutput{};
        double _frameTime = -1.0;
        int _observedShaderPreset = -1;
        int _previousShaderPreset = 0;
        std::string _observedShaderId;
        std::string _previousShaderId;
        double _rateTime = 0.0;
        double _lastRuntimeTime = 0.0;
    };
}
