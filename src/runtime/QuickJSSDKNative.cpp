#include "QuickJSInternal.hpp"
#include "quartz/client/runtime/JavaScriptRuntime.hpp"
#include "quartz/client/runtime/RuntimeTypes.hpp"
#include "quartz/client/shader/ShaderFramebuffer.hpp"
#include <quickjs.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

namespace quartz::client
{
    namespace
    {
        RuntimeQuickJSContext* state(JSContext* ctx) noexcept { return static_cast<RuntimeQuickJSContext*>(JS_GetContextOpaque(ctx)); }

        bool toString(JSContext* ctx, JSValueConst value, std::string& output)
        {
            const char* raw = JS_ToCString(ctx, value); if (!raw) return false; output = raw; JS_FreeCString(ctx, raw); return true;
        }

        JSValue property(JSContext* ctx, JSValueConst object, const char* name) { return JS_GetPropertyStr(ctx, object, name); }

        bool stringProperty(JSContext* ctx, JSValueConst object, const char* name, std::string& output)
        {
            JSValue value = property(ctx, object, name); if (JS_IsUndefined(value)) { JS_FreeValue(ctx, value); return false; }
            const bool ok = toString(ctx, value, output); JS_FreeValue(ctx, value); return ok;
        }

        bool numberProperty(JSContext* ctx, JSValueConst object, const char* name, double& output)
        {
            JSValue value = property(ctx, object, name); if (JS_IsUndefined(value)) { JS_FreeValue(ctx, value); return false; }
            const int ok = JS_ToFloat64(ctx, &output, value); JS_FreeValue(ctx, value); return ok >= 0 && std::isfinite(output);
        }

        std::string normalized(std::string value)
        {
            std::erase_if(value, [](const unsigned char c) { return c == ' ' || c == '_' || c == '-' || c == ':' || c == '.'; });
            std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); }); return value;
        }

        RuntimeScriptPropertyType propertyType(const std::string_view type)
        {
            if (type == "boolean") return RuntimeScriptPropertyType::Boolean;
            if (type == "int32") return RuntimeScriptPropertyType::Int32;
            if (type == "uint32") return RuntimeScriptPropertyType::UInt32;
            if (type == "float32") return RuntimeScriptPropertyType::Float32;
            if (type == "float64") return RuntimeScriptPropertyType::Float64;
            if (type == "shader") return RuntimeScriptPropertyType::Shader;
            if (type == "file") return RuntimeScriptPropertyType::File;
            if (type == "directory") return RuntimeScriptPropertyType::Directory;
            if (type == "key") return RuntimeScriptPropertyType::Key;
            if (type == "enum") return RuntimeScriptPropertyType::Enum;
            return RuntimeScriptPropertyType::String;
        }

        RuntimeScriptProperty* propertyFor(RuntimeScript& script, const std::string_view id)
        {
            const auto it = std::ranges::find(script.Properties, id, &RuntimeScriptProperty::Id); return it == script.Properties.end() ? nullptr : &*it;
        }

        JSValue propertyValue(JSContext* ctx, const RuntimeScriptProperty& value)
        {
            switch (value.Type)
            {
            case RuntimeScriptPropertyType::Boolean: return JS_NewBool(ctx, value.BoolValue);
            case RuntimeScriptPropertyType::Int32: return JS_NewInt32(ctx, static_cast<std::int32_t>(value.NumberValue));
            case RuntimeScriptPropertyType::UInt32: return JS_NewUint32(ctx, static_cast<std::uint32_t>(value.NumberValue));
            case RuntimeScriptPropertyType::Float32:
            case RuntimeScriptPropertyType::Float64: return JS_NewFloat64(ctx, value.NumberValue);
            case RuntimeScriptPropertyType::Key: return value.KeyIsNumber ? JS_NewInt32(ctx, static_cast<std::int32_t>(value.NumberValue)) : JS_NewString(ctx, value.StringValue.c_str());
            default: return JS_NewString(ctx, value.StringValue.c_str());
            }
        }

        bool assignPropertyValue(JSContext* ctx, RuntimeScriptProperty& target, JSValueConst value)
        {
            switch (target.Type)
            {
            case RuntimeScriptPropertyType::Boolean:
            {
                const int boolean = JS_ToBool(ctx, value); if (boolean < 0) return false; target.BoolValue = boolean != 0; return true;
            }
            case RuntimeScriptPropertyType::Int32:
            case RuntimeScriptPropertyType::UInt32:
            case RuntimeScriptPropertyType::Float32:
            case RuntimeScriptPropertyType::Float64:
            {
                double number = 0.0; if (JS_ToFloat64(ctx, &number, value) < 0 || !std::isfinite(number)) return false;
                if (target.Type == RuntimeScriptPropertyType::Int32) number = std::clamp(std::round(number), static_cast<double>(std::numeric_limits<std::int32_t>::min()), static_cast<double>(std::numeric_limits<std::int32_t>::max()));
                else if (target.Type == RuntimeScriptPropertyType::UInt32) number = std::clamp(std::round(number), 0.0, static_cast<double>(std::numeric_limits<std::uint32_t>::max()));
                if (target.HasMin) number = std::max(number, target.Min); if (target.HasMax) number = std::min(number, target.Max); target.NumberValue = number; return true;
            }
            case RuntimeScriptPropertyType::Key:
                if (JS_IsNumber(value)) { std::int32_t key = 0; if (JS_ToInt32(ctx, &key, value) < 0) return false; target.NumberValue = key; target.KeyIsNumber = true; return true; }
                target.KeyIsNumber = false; return toString(ctx, value, target.StringValue);
            default:
            {
                std::string text; if (!toString(ctx, value, text)) return false;
                if (target.Type == RuntimeScriptPropertyType::Enum && !target.EnumValues.empty() && std::ranges::find(target.EnumValues, text) == target.EnumValues.end()) return false;
                target.StringValue = std::move(text); return true;
            }
            }
        }

        void assignDefault(RuntimeScriptProperty& target)
        {
            target.StringValue = target.DefaultString; target.NumberValue = target.DefaultNumber; target.BoolValue = target.DefaultBool; target.KeyIsNumber = target.DefaultKeyIsNumber;
        }

        JSValue savedProperty(JSContext* ctx, JSValueConst sdk, const std::string& id)
        {
            JSValue storage = JS_GetPropertyStr(ctx, sdk, "__storage"); if (!JS_IsObject(storage)) { JS_FreeValue(ctx, storage); return JS_UNDEFINED; }
            JSValue properties = JS_GetPropertyStr(ctx, storage, "__quartzProperties"); JS_FreeValue(ctx, storage);
            if (!JS_IsObject(properties)) { JS_FreeValue(ctx, properties); return JS_UNDEFINED; }
            JSValue result = JS_GetPropertyStr(ctx, properties, id.c_str()); JS_FreeValue(ctx, properties); return result;
        }

        JSValue jsPropertyRegister(JSContext* ctx, JSValueConst thisValue, const int argc, JSValueConst* argv)
        {
            auto* context = state(ctx); if (!context || !context->Script || argc < 3 || !JS_IsObject(argv[2])) return JS_ThrowTypeError(ctx, "Property registration requires type, id and options");
            std::string typeName, id; if (!toString(ctx, argv[0], typeName) || !toString(ctx, argv[1], id) || id.empty()) return JS_ThrowTypeError(ctx, "Property id must be a non-empty string");
            RuntimeScript& script = *context->Script; RuntimeScriptProperty* current = propertyFor(script, id); const RuntimeScriptPropertyType type = propertyType(typeName);
            if (!current)
            {
                script.Properties.emplace_back(); current = &script.Properties.back(); current->Id = id; current->Type = type; current->Revision = 1;
                JSValue defaultValue = JS_GetPropertyStr(ctx, argv[2], "default"); if (JS_IsUndefined(defaultValue)) { JS_FreeValue(ctx, defaultValue); return JS_ThrowTypeError(ctx, "Property options.default is required"); }
                if (!assignPropertyValue(ctx, *current, defaultValue)) { JS_FreeValue(ctx, defaultValue); return JS_ThrowTypeError(ctx, "Property default value has the wrong type"); }
                current->DefaultString = current->StringValue; current->DefaultNumber = current->NumberValue; current->DefaultBool = current->BoolValue; current->DefaultKeyIsNumber = current->KeyIsNumber; JS_FreeValue(ctx, defaultValue);
                JSValue saved = savedProperty(ctx, thisValue, id); if (!JS_IsUndefined(saved)) assignPropertyValue(ctx, *current, saved); JS_FreeValue(ctx, saved);
            }
            else current->Type = type;
            std::string text; if (stringProperty(ctx, argv[2], "label", text)) current->Label = std::move(text); else if (current->Label.empty()) current->Label = id;
            if (stringProperty(ctx, argv[2], "group", text)) current->Group = std::move(text); if (stringProperty(ctx, argv[2], "description", text)) current->Description = std::move(text);
            double number = 0.0; current->HasMin = numberProperty(ctx, argv[2], "min", number); if (current->HasMin) current->Min = number; current->HasMax = numberProperty(ctx, argv[2], "max", number); if (current->HasMax) current->Max = number; if (numberProperty(ctx, argv[2], "step", number)) current->Step = std::max(number, 0.000001);
            if (type == RuntimeScriptPropertyType::Enum)
            {
                JSValue values = JS_GetPropertyStr(ctx, argv[2], "values"); if (JS_IsArray(ctx, values))
                {
                    JSValue length = JS_GetPropertyStr(ctx, values, "length"); std::uint32_t count = 0; JS_ToUint32(ctx, &count, length); JS_FreeValue(ctx, length); current->EnumValues.clear();
                    for (std::uint32_t i = 0; i < count; ++i) { JSValue value = JS_GetPropertyUint32(ctx, values, i); std::string item; if (toString(ctx, value, item)) current->EnumValues.push_back(std::move(item)); JS_FreeValue(ctx, value); }
                }
                JS_FreeValue(ctx, values);
            }
            if (context->JavaScript) context->JavaScript->markChanged(); return JS_TRUE;
        }

        JSValue jsPropertyGet(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            auto* context = state(ctx); if (!context || !context->Script || argc < 1) return JS_UNDEFINED; std::string id; if (!toString(ctx, argv[0], id)) return JS_EXCEPTION; const auto* value = propertyFor(*context->Script, id); return value ? propertyValue(ctx, *value) : JS_UNDEFINED;
        }

        JSValue jsPropertySet(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            auto* context = state(ctx); if (!context || !context->Script || argc < 2) return JS_FALSE; std::string id; if (!toString(ctx, argv[0], id)) return JS_EXCEPTION; auto* target = propertyFor(*context->Script, id); if (!target) return JS_FALSE;
            const std::string oldString = target->StringValue; const double oldNumber = target->NumberValue; const bool oldBool = target->BoolValue; const bool oldKey = target->KeyIsNumber;
            if (!assignPropertyValue(ctx, *target, argv[1])) return JS_ThrowTypeError(ctx, "invalid value for Property '%s'", id.c_str());
            if (oldString != target->StringValue || oldNumber != target->NumberValue || oldBool != target->BoolValue || oldKey != target->KeyIsNumber) { ++target->Revision; if (context->JavaScript) context->JavaScript->markChanged(); }
            return JS_TRUE;
        }

        JSValue jsPropertyReset(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            auto* context = state(ctx); if (!context || !context->Script || argc < 1) return JS_FALSE; std::string id; if (!toString(ctx, argv[0], id)) return JS_EXCEPTION; auto* target = propertyFor(*context->Script, id); if (!target) return JS_FALSE;
            assignDefault(*target); ++target->Revision; if (context->JavaScript) context->JavaScript->markChanged(); return JS_TRUE;
        }

        JSValue jsScriptConfigure(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            auto* context = state(ctx); if (!context || !context->Script || argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "Script.configure(configuration) requires an object"); RuntimeScript& script = *context->Script;
            std::string id; if (!stringProperty(ctx, argv[0], "id", id) || id.empty()) return JS_ThrowTypeError(ctx, "Script.configure.id is required"); bool changed = script.StableId != id; script.StableId = std::move(id);
            if (!script.DefaultsApplied)
            {
                if (!script.LoadedFromConfig)
                {
                    std::string name; if (stringProperty(ctx, argv[0], "name", name) && !name.empty()) { std::snprintf(script.Name, sizeof(script.Name), "%s", name.c_str()); changed = true; }
                    double value = 0.0; if (numberProperty(ctx, argv[0], "updateRate", value)) { script.UpdateHz = std::clamp(static_cast<float>(value), 0.5f, 500.0f); changed = true; }
                    if (numberProperty(ctx, argv[0], "timeout", value)) { script.TimeoutMs = std::clamp(static_cast<float>(value), 0.1f, 100.0f); changed = true; }
                    if (numberProperty(ctx, argv[0], "priority", value)) { value = std::clamp(std::round(value), static_cast<double>(std::numeric_limits<int>::min()), static_cast<double>(std::numeric_limits<int>::max())); script.Priority = static_cast<int>(value); changed = true; }
                }
                script.DefaultsApplied = true;
            }
            if (changed && context->JavaScript) context->JavaScript->markChanged(); return JS_TRUE;
        }

        JSValue jsScriptReload(JSContext* ctx, JSValueConst, int, JSValueConst*) { auto* context = state(ctx); if (!context || !context->Script) return JS_FALSE; context->Script->ReloadRequested = true; return JS_TRUE; }
        JSValue jsScriptId(JSContext* ctx, JSValueConst, int, JSValueConst*) { auto* context = state(ctx); if (!context || !context->Script) return JS_UNDEFINED; if (!context->Script->StableId.empty()) return JS_NewString(ctx, context->Script->StableId.c_str()); return JS_NewString(ctx, std::to_string(context->Script->Id).c_str()); }

        JSValue runtimeShader(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            auto* context = state(ctx); if (!context || !context->Output || argc < 1) return JS_FALSE; if (context->JavaScript && context->Script && !context->JavaScript->canWriteShader(context->Script->Id)) return JS_FALSE; std::string id; if (!toString(ctx, argv[0], id)) return JS_EXCEPTION; context->Output->ShaderId = id; context->Output->ShaderPresetIndex.reset();
            if (argc > 1 && !JS_IsUndefined(argv[1])) { double seconds = 0.0; if (JS_ToFloat64(ctx, &seconds, argv[1]) < 0) return JS_EXCEPTION; context->Output->ShaderTransitionSeconds = static_cast<float>(std::max(seconds, 0.0)); } return JS_TRUE;
        }

        JSValue runtimeShaderPreset(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            auto* context = state(ctx); if (!context || !context->Output || argc < 1) return JS_FALSE; if (context->JavaScript && context->Script && !context->JavaScript->canWriteShader(context->Script->Id)) return JS_FALSE; std::int32_t index = 0; if (JS_ToInt32(ctx, &index, argv[0]) < 0) return JS_EXCEPTION; context->Output->ShaderPresetIndex = index; context->Output->ShaderId.reset();
            if (argc > 1 && !JS_IsUndefined(argv[1])) { double seconds = 0.0; if (JS_ToFloat64(ctx, &seconds, argv[1]) < 0) return JS_EXCEPTION; context->Output->ShaderTransitionSeconds = static_cast<float>(std::max(seconds, 0.0)); } return JS_TRUE;
        }

        JSValue runtimeBrightness(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv) { auto* context = state(ctx); if (!context || !context->Output || argc < 1) return JS_FALSE; double value = 0.0; if (JS_ToFloat64(ctx, &value, argv[0]) < 0 || !std::isfinite(value)) return JS_ThrowTypeError(ctx, "brightness must be finite"); context->Output->GlobalBrightness = static_cast<float>(value); return JS_TRUE; }
        JSValue runtimeSend(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv) { auto* context = state(ctx); if (!context || !context->Output || argc < 1) return JS_FALSE; const int value = JS_ToBool(ctx, argv[0]); if (value < 0) return JS_EXCEPTION; context->Output->SendFramebuffer = value != 0; return JS_TRUE; }
        JSValue runtimeBaseMode(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv) { auto* context = state(ctx); if (!context || !context->Output || argc < 1) return JS_FALSE; std::int32_t value = 0; if (JS_ToInt32(ctx, &value, argv[0]) < 0) return JS_EXCEPTION; context->Output->BaseColorMode = value; return JS_TRUE; }
        JSValue runtimeMaterial(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv) { auto* context = state(ctx); if (!context || !context->Shader || argc < 3) return JS_FALSE; std::string id; if (!toString(ctx, argv[0], id)) return JS_EXCEPTION; std::int32_t component = 0; double value = 0.0; if (JS_ToInt32(ctx, &component, argv[1]) < 0 || JS_ToFloat64(ctx, &value, argv[2]) < 0) return JS_EXCEPTION; return JS_NewBool(ctx, context->Shader->setMaterialParameter(id, component, static_cast<float>(value))); }
        JSValue runtimeCurrentShader(JSContext* ctx, JSValueConst, int, JSValueConst*) { auto* context = state(ctx); return context && context->SignalContext ? JS_NewString(ctx, context->SignalContext->CurrentShaderId.c_str()) : JS_UNDEFINED; }
        JSValue runtimePreviousShader(JSContext* ctx, JSValueConst, int, JSValueConst*) { auto* context = state(ctx); return context && context->SignalContext ? JS_NewString(ctx, context->SignalContext->PreviousShaderId.c_str()) : JS_UNDEFINED; }

        JSValue runtimeClear(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            auto* context = state(ctx); if (!context || !context->Output) return JS_FALSE; std::string target = "all"; if (argc > 0 && !JS_IsUndefined(argv[0]) && !toString(ctx, argv[0], target)) return JS_EXCEPTION; target = normalized(std::move(target));
            if (target == "all") *context->Output = {};
            else if (target == "shader") { context->Output->ShaderId.reset(); context->Output->ShaderPresetIndex.reset(); }
            else if (target == "brightness") context->Output->GlobalBrightness.reset();
            else if (target == "framebuffer" || target == "sendframebuffer") context->Output->SendFramebuffer.reset();
            else if (target == "basecolor" || target == "basecolormode") context->Output->BaseColorMode.reset();
            else return JS_FALSE; return JS_TRUE;
        }

        JSValue shaderMutexLock(JSContext* ctx, JSValueConst, int, JSValueConst*) { auto* context = state(ctx); return JS_NewBool(ctx, context && context->JavaScript && context->Script && context->JavaScript->lockShaderMutex(context->Script->Id)); }
        JSValue shaderMutexUnlock(JSContext* ctx, JSValueConst, int, JSValueConst*) { auto* context = state(ctx); return JS_NewBool(ctx, context && context->JavaScript && context->Script && context->JavaScript->unlockShaderMutex(context->Script->Id)); }
        JSValue shaderMutexLocked(JSContext* ctx, JSValueConst, int, JSValueConst*) { auto* context = state(ctx); return JS_NewBool(ctx, context && context->JavaScript && context->JavaScript->shaderMutexLocked()); }
        JSValue shaderMutexOwned(JSContext* ctx, JSValueConst, int, JSValueConst*) { auto* context = state(ctx); return JS_NewBool(ctx, context && context->JavaScript && context->Script && context->JavaScript->ownsShaderMutex(context->Script->Id)); }
    }

    void runtimeInstallQuickJSSDKNativeApi(JSContext* ctx, JSValueConst api)
    {
        JSValue runtime = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, runtime, "shader", JS_NewCFunction(ctx, runtimeShader, "shader", 2)); JS_SetPropertyStr(ctx, runtime, "shaderPreset", JS_NewCFunction(ctx, runtimeShaderPreset, "shaderPreset", 2));
        JS_SetPropertyStr(ctx, runtime, "brightness", JS_NewCFunction(ctx, runtimeBrightness, "brightness", 1)); JS_SetPropertyStr(ctx, runtime, "sendFramebuffer", JS_NewCFunction(ctx, runtimeSend, "sendFramebuffer", 1)); JS_SetPropertyStr(ctx, runtime, "baseColorMode", JS_NewCFunction(ctx, runtimeBaseMode, "baseColorMode", 1));
        JS_SetPropertyStr(ctx, runtime, "material", JS_NewCFunction(ctx, runtimeMaterial, "material", 3)); JS_SetPropertyStr(ctx, runtime, "currentShader", JS_NewCFunction(ctx, runtimeCurrentShader, "currentShader", 0)); JS_SetPropertyStr(ctx, runtime, "previousShader", JS_NewCFunction(ctx, runtimePreviousShader, "previousShader", 0)); JS_SetPropertyStr(ctx, runtime, "clear", JS_NewCFunction(ctx, runtimeClear, "clear", 1));
        JS_SetPropertyStr(ctx, api, "runtime", runtime);

        JSValue shaderMutex = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, shaderMutex, "lock", JS_NewCFunction(ctx, shaderMutexLock, "lock", 0)); JS_SetPropertyStr(ctx, shaderMutex, "unlock", JS_NewCFunction(ctx, shaderMutexUnlock, "unlock", 0));
        JS_SetPropertyStr(ctx, shaderMutex, "locked", JS_NewCFunction(ctx, shaderMutexLocked, "locked", 0)); JS_SetPropertyStr(ctx, shaderMutex, "owned", JS_NewCFunction(ctx, shaderMutexOwned, "owned", 0)); JS_SetPropertyStr(ctx, api, "shaderMutex", shaderMutex);

        JSValue sdk = JS_NewObject(ctx); JSValue storage = JS_GetPropertyStr(ctx, api, "storage"); JS_DefinePropertyValueStr(ctx, sdk, "__storage", storage, 0);
        JS_SetPropertyStr(ctx, sdk, "propertyRegister", JS_NewCFunction(ctx, jsPropertyRegister, "propertyRegister", 3)); JS_SetPropertyStr(ctx, sdk, "propertyGet", JS_NewCFunction(ctx, jsPropertyGet, "propertyGet", 1)); JS_SetPropertyStr(ctx, sdk, "propertySet", JS_NewCFunction(ctx, jsPropertySet, "propertySet", 2)); JS_SetPropertyStr(ctx, sdk, "propertyReset", JS_NewCFunction(ctx, jsPropertyReset, "propertyReset", 1));
        JS_SetPropertyStr(ctx, sdk, "scriptConfigure", JS_NewCFunction(ctx, jsScriptConfigure, "scriptConfigure", 1)); JS_SetPropertyStr(ctx, sdk, "scriptReload", JS_NewCFunction(ctx, jsScriptReload, "scriptReload", 0)); JS_SetPropertyStr(ctx, sdk, "scriptId", JS_NewCFunction(ctx, jsScriptId, "scriptId", 0)); JS_SetPropertyStr(ctx, api, "sdk", sdk);
    }
}
