#include "quartz/client/runtime/QuickJS.hpp"
#include "quartz/client/runtime/RuntimeBindingEngine.hpp"
#include <quickjs.h>
#include <charconv>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace quartz::client
{
    namespace
    {
        constexpr std::size_t RuntimeMemoryLimit = 64ULL * 1024ULL * 1024ULL;
        constexpr std::size_t RuntimeStackLimit = 512ULL * 1024ULL;
        constexpr double MaximumSafeInteger = 9007199254740991.0;

        std::uint64_t hashScript(const std::string_view text) noexcept
        {
            std::uint64_t hash = 14695981039346656037ULL;
            for (const unsigned char c : text) { hash ^= c; hash *= 1099511628211ULL; }
            return hash;
        }

        struct QuickJSRuntime;
        struct ScriptInstance
        {
            QuickJSRuntime* Owner = nullptr;
            RuntimeBindingEngine* Engine = nullptr;
            RuntimeBinding* Binding = nullptr;
            const RuntimeSignalContext* SignalContext = nullptr;
            JSContext* Context = nullptr;
            JSValue Function = JS_UNDEFINED;
            JSValue Api = JS_UNDEFINED;
            std::uint64_t SourceHash = 0;
            ~ScriptInstance()
            {
                if (!Context) return;
                JS_FreeValue(Context, Function); JS_FreeValue(Context, Api); JS_FreeContext(Context);
            }
        };

        const RuntimeBinding* resolveBinding(JSContext* ctx, JSValueConst value)
        {
            auto* instance = static_cast<ScriptInstance*>(JS_GetContextOpaque(ctx));
            if (!instance || !instance->Engine) return nullptr;
            if (JS_IsNumber(value) || JS_IsBigInt(ctx, value))
            {
                std::int64_t id = 0;
                if (JS_ToInt64Ext(ctx, &id, value) < 0 || id <= 0) return nullptr;
                return instance->Engine->findBinding(static_cast<std::uint64_t>(id));
            }
            const char* name = JS_ToCString(ctx, value);
            if (!name) return nullptr;
            const RuntimeBinding* result = instance->Engine->findBindingByName(name); JS_FreeCString(ctx, name); return result;
        }

        const RuntimeControlRule* resolveControl(JSContext* ctx, JSValueConst value)
        {
            auto* instance = static_cast<ScriptInstance*>(JS_GetContextOpaque(ctx));
            if (!instance || !instance->Engine) return nullptr;
            if (JS_IsNumber(value) || JS_IsBigInt(ctx, value))
            {
                std::int64_t id = 0;
                if (JS_ToInt64Ext(ctx, &id, value) < 0 || id <= 0) return nullptr;
                return instance->Engine->findControl(static_cast<std::uint64_t>(id));
            }
            const char* name = JS_ToCString(ctx, value);
            if (!name) return nullptr;
            const RuntimeControlRule* result = instance->Engine->findControlByName(name); JS_FreeCString(ctx, name); return result;
        }

        const RuntimeValueBankEntry* resolveBank(JSContext* ctx, JSValueConst value)
        {
            auto* instance = static_cast<ScriptInstance*>(JS_GetContextOpaque(ctx));
            if (!instance || !instance->Engine) return nullptr;
            if (JS_IsNumber(value) || JS_IsBigInt(ctx, value))
            {
                std::int64_t id = 0;
                if (JS_ToInt64Ext(ctx, &id, value) < 0 || id <= 0) return nullptr;
                return instance->Engine->findBankValue(static_cast<std::uint64_t>(id));
            }
            const char* name = JS_ToCString(ctx, value);
            if (!name) return nullptr;
            const RuntimeValueBankEntry* result = instance->Engine->findBankValueByName(name); JS_FreeCString(ctx, name); return result;
        }

        JSValue jsBinding(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            const RuntimeBinding* binding = argc > 0 ? resolveBinding(ctx, argv[0]) : nullptr;
            return binding && binding->Enabled && binding->HasValue ? JS_NewFloat64(ctx, binding->Value) : JS_UNDEFINED;
        }

        JSValue jsRaw(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            const RuntimeBinding* binding = argc > 0 ? resolveBinding(ctx, argv[0]) : nullptr;
            return binding && binding->Enabled && binding->HasValue ? JS_NewFloat64(ctx, binding->RawValue) : JS_UNDEFINED;
        }

        JSValue jsText(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            const RuntimeBinding* binding = argc > 0 ? resolveBinding(ctx, argv[0]) : nullptr;
            return binding && binding->Enabled && binding->HasString ? JS_NewString(ctx, binding->StringValue.c_str()) : JS_UNDEFINED;
        }

        JSValue jsAddress(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            const RuntimeBinding* binding = argc > 0 ? resolveBinding(ctx, argv[0]) : nullptr;
            return binding && binding->Enabled && binding->HasAddress ? JS_NewBigUint64(ctx, static_cast<std::uint64_t>(binding->AddressValue)) : JS_UNDEFINED;
        }

        JSValue jsBank(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            const RuntimeValueBankEntry* value = argc > 0 ? resolveBank(ctx, argv[0]) : nullptr;
            if (!value || !value->Enabled || !value->HasValue) return JS_UNDEFINED;
            switch (value->Type)
            {
            case RuntimeBankValueType::Number: return JS_NewFloat64(ctx, value->Number);
            case RuntimeBankValueType::Integer:
                return std::abs(static_cast<double>(value->Integer)) <= MaximumSafeInteger ? JS_NewFloat64(ctx, static_cast<double>(value->Integer)) : JS_NewBigInt64(ctx, value->Integer);
            case RuntimeBankValueType::Boolean: return JS_NewBool(ctx, value->Boolean);
            case RuntimeBankValueType::String: return JS_NewString(ctx, value->String);
            case RuntimeBankValueType::Address: return value->Address ? JS_NewBigUint64(ctx, static_cast<std::uint64_t>(value->Address)) : JS_UNDEFINED;
            }
            return JS_UNDEFINED;
        }

        JSValue jsControl(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            const RuntimeControlRule* control = argc > 0 ? resolveControl(ctx, argv[0]) : nullptr;
            return control && control->Enabled && control->RuntimeEnabled ? JS_NewBool(ctx, control->ConditionActive) : JS_UNDEFINED;
        }

        JSValue jsTriggered(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            const RuntimeControlRule* control = argc > 0 ? resolveControl(ctx, argv[0]) : nullptr;
            return control && control->Enabled && control->RuntimeEnabled ? JS_NewBool(ctx, control->TriggeredThisFrame) : JS_UNDEFINED;
        }

        JSValue jsLog(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            auto* instance = static_cast<ScriptInstance*>(JS_GetContextOpaque(ctx));
            if (!instance || !instance->Binding) return JS_UNDEFINED;
            std::string text;
            for (int i = 0; i < argc; ++i)
            {
                const char* value = JS_ToCString(ctx, argv[i]);
                if (!value) return JS_EXCEPTION;
                if (!text.empty()) text.push_back(' ');
                if (text.size() < 1024) text.append(value, std::min<std::size_t>(std::strlen(value), 1024 - text.size()));
                JS_FreeCString(ctx, value);
            }
            instance->Binding->ScriptLastLog = std::move(text);
            return JS_UNDEFINED;
        }

        std::string exceptionText(JSContext* ctx)
        {
            JSValue exception = JS_GetException(ctx);
            std::string result = "QuickJS exception";
            if (const char* text = JS_ToCString(ctx, exception)) { result = text; JS_FreeCString(ctx, text); }
            if (JS_IsObject(exception))
            {
                JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
                if (!JS_IsUndefined(stack)) if (const char* text = JS_ToCString(ctx, stack)) { if (*text && result.find(text) == std::string::npos) { result += "\n"; result += text; } JS_FreeCString(ctx, text); }
                JS_FreeValue(ctx, stack);
            }
            JS_FreeValue(ctx, exception);
            return result;
        }

        bool valueToAddress(JSContext* ctx, JSValueConst value, std::uintptr_t& address)
        {
            if (JS_IsBigInt(ctx, value))
            {
                const char* text = JS_ToCString(ctx, value);
                if (!text) return false;
                std::uint64_t raw = 0; const char* end = text + std::strlen(text); const auto [ptr, ec] = std::from_chars(text, end, raw, 10); JS_FreeCString(ctx, text);
                if (ec != std::errc{} || ptr != end || raw > std::numeric_limits<std::uintptr_t>::max()) return false;
                address = static_cast<std::uintptr_t>(raw); return true;
            }
            if (!JS_IsNumber(value)) return false;
            double raw = 0.0;
            if (JS_ToFloat64(ctx, &raw, value) < 0 || !std::isfinite(raw) || raw < 0.0 || std::floor(raw) != raw || raw > MaximumSafeInteger || raw > static_cast<double>(std::numeric_limits<std::uintptr_t>::max())) return false;
            address = static_cast<std::uintptr_t>(raw); return true;
        }

        bool valueToNumber(JSContext* ctx, JSValueConst value, float& output)
        {
            if (JS_IsBool(value)) { const int result = JS_ToBool(ctx, value); if (result < 0) return false; output = result ? 1.0f : 0.0f; return true; }
            if (!JS_IsNumber(value)) return false;
            double raw = 0.0; if (JS_ToFloat64(ctx, &raw, value) < 0 || !std::isfinite(raw)) return false; output = static_cast<float>(raw); return std::isfinite(output);
        }

        bool applyScriptResult(JSContext* ctx, JSValueConst value, RuntimeBinding& binding, float& output, std::string& error)
        {
            binding.HasAddress = false; binding.AddressValue = 0; binding.AddressProvenance.clear();
            if (valueToNumber(ctx, value, output)) return true;
            if (JS_IsString(value))
            {
                const char* text = JS_ToCString(ctx, value); if (!text) { error = exceptionText(ctx); return false; }
                binding.StringValue = text; binding.HasString = true; output = binding.StringValue.empty() ? 0.0f : 1.0f; JS_FreeCString(ctx, text); return true;
            }
            std::uintptr_t address = 0;
            if (valueToAddress(ctx, value, address))
            {
                binding.HasAddress = address != 0; binding.AddressValue = address; if (binding.HasAddress) binding.AddressProvenance = {"QuickJS script result"}; output = address ? 1.0f : 0.0f; return true;
            }
            if (!JS_IsObject(value)) { error = "QuickJS script must return a number, bool, string, address BigInt, or { value, string, address }"; return false; }

            bool produced = false, numericProduced = false;
            JSValue numeric = JS_GetPropertyStr(ctx, value, "value");
            if (JS_IsException(numeric)) { error = exceptionText(ctx); return false; }
            if (!JS_IsUndefined(numeric)) { if (!valueToNumber(ctx, numeric, output)) { JS_FreeValue(ctx, numeric); error = "QuickJS result.value must be a finite number or bool"; return false; } produced = numericProduced = true; }
            JS_FreeValue(ctx, numeric);

            JSValue stringValue = JS_GetPropertyStr(ctx, value, "string");
            if (JS_IsException(stringValue)) { error = exceptionText(ctx); return false; }
            if (JS_IsUndefined(stringValue)) { JS_FreeValue(ctx, stringValue); stringValue = JS_GetPropertyStr(ctx, value, "text"); if (JS_IsException(stringValue)) { error = exceptionText(ctx); return false; } }
            if (!JS_IsUndefined(stringValue))
            {
                if (!JS_IsString(stringValue)) { JS_FreeValue(ctx, stringValue); error = "QuickJS result.string must be a string"; return false; }
                const char* text = JS_ToCString(ctx, stringValue); if (!text) { JS_FreeValue(ctx, stringValue); error = exceptionText(ctx); return false; }
                binding.StringValue = text; binding.HasString = true; JS_FreeCString(ctx, text); produced = true;
            }
            JS_FreeValue(ctx, stringValue);

            JSValue addressValue = JS_GetPropertyStr(ctx, value, "address");
            if (JS_IsException(addressValue)) { error = exceptionText(ctx); return false; }
            if (!JS_IsUndefined(addressValue))
            {
                if (!valueToAddress(ctx, addressValue, address)) { JS_FreeValue(ctx, addressValue); error = "QuickJS result.address must be a non-negative integer or BigInt"; return false; }
                binding.HasAddress = address != 0; binding.AddressValue = address; if (binding.HasAddress) binding.AddressProvenance = {"QuickJS script result"}; produced = true;
            }
            JS_FreeValue(ctx, addressValue);
            if (!produced) { error = "QuickJS result object did not contain value, string/text, or address"; return false; }
            if (!numericProduced) output = binding.HasAddress ? 1.0f : binding.HasString && !binding.StringValue.empty() ? 1.0f : 0.0f;
            return true;
        }

        struct QuickJSRuntime
        {
            JSRuntime* Runtime = nullptr;
            std::unordered_map<std::uint64_t, std::unique_ptr<ScriptInstance>> Instances;
            std::chrono::steady_clock::time_point Deadline{};
            bool DeadlineActive = false;
            bool Interrupted = false;
            std::string Error;

            QuickJSRuntime()
            {
                Runtime = JS_NewRuntime();
                if (!Runtime) { Error = "could not create QuickJS runtime"; return; }
                JS_SetRuntimeInfo(Runtime, "Quartz scripted bindings"); JS_SetMemoryLimit(Runtime, RuntimeMemoryLimit); JS_SetMaxStackSize(Runtime, RuntimeStackLimit); JS_SetCanBlock(Runtime, false); JS_SetInterruptHandler(Runtime, interrupt, this);
            }
            ~QuickJSRuntime() { Instances.clear(); if (Runtime) JS_FreeRuntime(Runtime); }

            static int interrupt(JSRuntime*, void* opaque)
            {
                auto* self = static_cast<QuickJSRuntime*>(opaque);
                if (!self || !self->DeadlineActive || std::chrono::steady_clock::now() < self->Deadline) return 0;
                self->Interrupted = true; return 1;
            }

            void beginDeadline(const float milliseconds)
            {
                Interrupted = false; DeadlineActive = true; Deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(std::clamp(milliseconds, 0.1f, 100.0f)));
            }
            void endDeadline() noexcept { DeadlineActive = false; }

            ScriptInstance* instance(RuntimeBinding& binding)
            {
                if (!Runtime) return nullptr;
                auto [it, inserted] = Instances.try_emplace(binding.Id);
                if (!inserted) return it->second.get();
                auto created = std::make_unique<ScriptInstance>(); created->Owner = this; created->Context = JS_NewContext(Runtime);
                if (!created->Context) { Error = "could not create QuickJS context"; Instances.erase(it); return nullptr; }
                JS_SetContextOpaque(created->Context, created.get()); created->Api = JS_NewObject(created->Context);
                if (JS_IsException(created->Api)) { Error = exceptionText(created->Context); Instances.erase(it); return nullptr; }
                JS_SetPropertyStr(created->Context, created->Api, "binding", JS_NewCFunction(created->Context, jsBinding, "binding", 1));
                JS_SetPropertyStr(created->Context, created->Api, "raw", JS_NewCFunction(created->Context, jsRaw, "raw", 1));
                JS_SetPropertyStr(created->Context, created->Api, "text", JS_NewCFunction(created->Context, jsText, "text", 1));
                JS_SetPropertyStr(created->Context, created->Api, "address", JS_NewCFunction(created->Context, jsAddress, "address", 1));
                JS_SetPropertyStr(created->Context, created->Api, "bank", JS_NewCFunction(created->Context, jsBank, "bank", 1));
                JS_SetPropertyStr(created->Context, created->Api, "control", JS_NewCFunction(created->Context, jsControl, "control", 1));
                JS_SetPropertyStr(created->Context, created->Api, "triggered", JS_NewCFunction(created->Context, jsTriggered, "triggered", 1));
                JS_SetPropertyStr(created->Context, created->Api, "log", JS_NewCFunction(created->Context, jsLog, "log", 1));
                JS_DefinePropertyValueStr(created->Context, created->Api, "state", JS_NewObject(created->Context), JS_PROP_ENUMERABLE);
                auto* result = created.get(); it->second = std::move(created); return result;
            }

            bool compile(ScriptInstance& instance, RuntimeBinding& binding, std::string& error)
            {
                const std::string_view body(binding.Script); const std::uint64_t hash = hashScript(body);
                if (instance.SourceHash == hash && JS_IsFunction(instance.Context, instance.Function)) return true;
                JS_FreeValue(instance.Context, instance.Function); instance.Function = JS_UNDEFINED; instance.SourceHash = hash;
                std::string source = "(function(q){\\n\\\"use strict\\\";\\n"; source.append(body); source += "\\n})";
                const std::string filename = "quartz-binding-" + std::to_string(binding.Id) + ".js";
                beginDeadline(binding.ScriptTimeoutMs); JSValue function = JS_Eval(instance.Context, source.c_str(), source.size(), filename.c_str(), JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT); endDeadline();
                if (JS_IsException(function)) { if (Interrupted) { JSValue exception = JS_GetException(instance.Context); JS_FreeValue(instance.Context, exception); ++binding.ScriptTimeoutCount; error = "QuickJS compile timed out"; } else error = exceptionText(instance.Context); return false; }
                if (!JS_IsFunction(instance.Context, function)) { JS_FreeValue(instance.Context, function); error = "QuickJS source did not compile to a function"; return false; }
                instance.Function = function; ++binding.ScriptCompileCount; return true;
            }

            bool evaluate(RuntimeBindingEngine& engine, RuntimeBinding& binding, const RuntimeSignalContext& signalContext, float& output)
            {
                ScriptInstance* script = instance(binding);
                if (!script) { binding.Error = Error.empty() ? "QuickJS runtime is unavailable" : Error; return false; }
                script->Engine = &engine; script->Binding = &binding; script->SignalContext = &signalContext; binding.ScriptLastLog.clear();
                std::string error;
                if (!compile(*script, binding, error)) { binding.Error = std::move(error); return false; }
                JS_SetPropertyStr(script->Context, script->Api, "time", JS_NewFloat64(script->Context, signalContext.Time));
                JS_SetPropertyStr(script->Context, script->Api, "deltaTime", JS_NewFloat64(script->Context, signalContext.DeltaTime));
                JS_SetPropertyStr(script->Context, script->Api, "previous", JS_NewFloat64(script->Context, binding.HasValue ? binding.Value : 0.0f));
                JS_SetPropertyStr(script->Context, script->Api, "previousRaw", JS_NewFloat64(script->Context, binding.HasValue ? binding.RawValue : 0.0f));
                JS_SetPropertyStr(script->Context, script->Api, "id", JS_NewBigUint64(script->Context, binding.Id));
                JS_SetPropertyStr(script->Context, script->Api, "name", JS_NewString(script->Context, binding.Name));
                JSValue argument = JS_DupValue(script->Context, script->Api);
                const auto started = std::chrono::steady_clock::now(); beginDeadline(binding.ScriptTimeoutMs); JSValue result = JS_Call(script->Context, script->Function, JS_UNDEFINED, 1, &argument); endDeadline();
                binding.ScriptLastMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(); ++binding.ScriptRunCount; JS_FreeValue(script->Context, argument);
                if (JS_IsException(result)) { if (Interrupted) { JSValue exception = JS_GetException(script->Context); JS_FreeValue(script->Context, exception); ++binding.ScriptTimeoutCount; binding.Error = "QuickJS execution timed out after " + std::to_string(binding.ScriptTimeoutMs) + " ms"; } else binding.Error = exceptionText(script->Context); return false; }
                const bool success = applyScriptResult(script->Context, result, binding, output, error); JS_FreeValue(script->Context, result);
                if (!success) { binding.Error = std::move(error); return false; }
                binding.Error.clear(); return true;
            }

            void reset(const std::uint64_t id) noexcept { Instances.erase(id); if (Runtime) JS_RunGC(Runtime); }
        };

        QuickJSRuntime& scriptRuntime() { static QuickJSRuntime runtime; return runtime; }
    }

    bool runtimeEvaluateScriptBinding(RuntimeBindingEngine& engine, RuntimeBinding& binding, const RuntimeSignalContext& context, float& output)
    {
        return scriptRuntime().evaluate(engine, binding, context, output);
    }

    void runtimeResetScriptBinding(const std::uint64_t bindingId) noexcept { scriptRuntime().reset(bindingId); }
}
