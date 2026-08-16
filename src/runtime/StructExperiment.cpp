#include "quartz/client/runtime/StructExperiment.hpp"
#include "QuickJSInternal.hpp"
#include <quickjs.h>
#include <charconv>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>

namespace quartz::client
{
    namespace
    {
        constexpr std::size_t ExperimentMemoryLimit = 16ULL * 1024ULL * 1024ULL;
        constexpr std::size_t ExperimentStackLimit = 256ULL * 1024ULL;
        constexpr std::size_t MaximumFields = 1024;
        constexpr std::size_t MaximumDepth = 8;

        std::string exceptionText(JSContext* ctx)
        {
            JSValue exception = JS_GetException(ctx); std::string result = "QuickJS exception";
            if (const char* text = JS_ToCString(ctx, exception)) { result = text; JS_FreeCString(ctx, text); }
            if (JS_IsObject(exception))
            {
                JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
                if (!JS_IsUndefined(stack)) if (const char* text = JS_ToCString(ctx, stack)) { if (*text && result.find(text) == std::string::npos) { result += '\n'; result += text; } JS_FreeCString(ctx, text); }
                JS_FreeValue(ctx, stack);
            }
            JS_FreeValue(ctx, exception); return result;
        }

        bool integerValue(JSContext* ctx, JSValueConst value, std::uint64_t& result)
        {
            if (JS_IsBigInt(ctx, value))
            {
                const char* text = JS_ToCString(ctx, value); if (!text) return false;
                const char* end = text + std::strlen(text); const auto [ptr, ec] = std::from_chars(text, end, result, 10); JS_FreeCString(ctx, text); return ec == std::errc{} && ptr == end;
            }
            if (!JS_IsNumber(value)) return false;
            double raw = 0.0; if (JS_ToFloat64(ctx, &raw, value) < 0 || raw < 0.0 || raw > static_cast<double>(std::numeric_limits<std::uint64_t>::max()) || raw != std::floor(raw)) return false;
            result = static_cast<std::uint64_t>(raw); return true;
        }

        bool stringValue(JSContext* ctx, JSValueConst object, const char* property, std::string& result, std::string& error)
        {
            JSValue value = JS_GetPropertyStr(ctx, object, property);
            if (JS_IsException(value)) { error = exceptionText(ctx); return false; }
            if (!JS_IsString(value)) { JS_FreeValue(ctx, value); error = std::string(property) + " must be a string"; return false; }
            const char* text = JS_ToCString(ctx, value); if (!text) { JS_FreeValue(ctx, value); error = exceptionText(ctx); return false; }
            result = text; JS_FreeCString(ctx, text); JS_FreeValue(ctx, value); return true;
        }

        bool integerProperty(JSContext* ctx, JSValueConst object, const char* property, std::uint64_t& result, std::string& error, const bool optional = false)
        {
            JSValue value = JS_GetPropertyStr(ctx, object, property);
            if (JS_IsException(value)) { error = exceptionText(ctx); return false; }
            if (optional && JS_IsUndefined(value)) { JS_FreeValue(ctx, value); result = 0; return true; }
            const bool ok = integerValue(ctx, value, result); JS_FreeValue(ctx, value);
            if (!ok) error = std::string(property) + " must be a non-negative integer or BigInt";
            return ok;
        }

        std::optional<StructExperimentFieldKind> kindFromString(const std::string_view kind) noexcept
        {
            if (kind == "i8") return StructExperimentFieldKind::I8; if (kind == "u8") return StructExperimentFieldKind::U8;
            if (kind == "i16") return StructExperimentFieldKind::I16; if (kind == "u16") return StructExperimentFieldKind::U16;
            if (kind == "i32") return StructExperimentFieldKind::I32; if (kind == "u32") return StructExperimentFieldKind::U32;
            if (kind == "i64") return StructExperimentFieldKind::I64; if (kind == "u64") return StructExperimentFieldKind::U64;
            if (kind == "f32") return StructExperimentFieldKind::F32; if (kind == "f64") return StructExperimentFieldKind::F64;
            if (kind == "bool") return StructExperimentFieldKind::Bool; if (kind == "pointer") return StructExperimentFieldKind::Pointer;
            if (kind == "struct") return StructExperimentFieldKind::Struct; if (kind == "array") return StructExperimentFieldKind::Array;
            return std::nullopt;
        }

        std::size_t scalarSize(const StructExperimentFieldKind kind) noexcept
        {
            switch (kind)
            {
            case StructExperimentFieldKind::I8: case StructExperimentFieldKind::U8: case StructExperimentFieldKind::Bool: return 1;
            case StructExperimentFieldKind::I16: case StructExperimentFieldKind::U16: return 2;
            case StructExperimentFieldKind::I32: case StructExperimentFieldKind::U32: case StructExperimentFieldKind::F32: return 4;
            case StructExperimentFieldKind::I64: case StructExperimentFieldKind::U64: case StructExperimentFieldKind::F64: return 8;
            default: return 0;
            }
        }

        bool parseDefinition(JSContext* ctx, JSValueConst value, StructExperimentDefinition& definition, std::string& error, std::size_t depth, std::size_t& fieldCount);

        bool parseField(JSContext* ctx, JSValueConst value, StructExperimentField& field, std::string& error, const std::size_t depth, std::size_t& fieldCount)
        {
            if (!JS_IsObject(value)) { error = "Struct field descriptor must be an object"; return false; }
            std::string kindText; if (!stringValue(ctx, value, "kind", kindText, error)) return false;
            const auto kind = kindFromString(kindText); if (!kind) { error = "unknown field kind: " + kindText; return false; }
            field.Kind = *kind; std::uint64_t offset = 0; if (!integerProperty(ctx, value, "offset", offset, error)) return false;
            if (offset > std::numeric_limits<std::uintptr_t>::max()) { error = "field offset is outside uintptr_t range"; return false; }
            field.Offset = static_cast<std::uintptr_t>(offset); field.Size = scalarSize(field.Kind);

            if (field.Kind == StructExperimentFieldKind::Pointer || field.Kind == StructExperimentFieldKind::Struct)
            {
                JSValue nested = JS_GetPropertyStr(ctx, value, "struct");
                if (JS_IsException(nested)) { error = exceptionText(ctx); return false; }
                if (!JS_IsUndefined(nested))
                {
                    field.Nested = std::make_shared<StructExperimentDefinition>();
                    const bool ok = parseDefinition(ctx, nested, *field.Nested, error, depth + 1, fieldCount); JS_FreeValue(ctx, nested); if (!ok) return false;
                }
                else { JS_FreeValue(ctx, nested); if (field.Kind == StructExperimentFieldKind::Struct) { error = "Field.Struct requires a Struct.define(...) type"; return false; } }
            }
            else if (field.Kind == StructExperimentFieldKind::Array)
            {
                std::uint64_t count = 0; if (!integerProperty(ctx, value, "count", count, error)) return false;
                if (count > 4096) { error = "array count is too large for object experiments"; return false; }
                field.Count = static_cast<std::size_t>(count);
                JSValue element = JS_GetPropertyStr(ctx, value, "element");
                if (JS_IsException(element)) { error = exceptionText(ctx); return false; }
                field.Element = std::make_shared<StructExperimentField>();
                const bool ok = parseField(ctx, element, *field.Element, error, depth + 1, fieldCount); JS_FreeValue(ctx, element); if (!ok) return false;
                if (field.Element->Size == 0 && field.Element->Kind != StructExperimentFieldKind::Pointer) { error = "array elements must have a fixed scalar size or be pointers"; return false; }
            }
            return true;
        }

        bool parseDefinition(JSContext* ctx, JSValueConst value, StructExperimentDefinition& definition, std::string& error, const std::size_t depth, std::size_t& fieldCount)
        {
            if (depth > MaximumDepth) { error = "Struct nesting is too deep"; return false; }
            if (!JS_IsObject(value)) { error = "script must return Struct.define({...})"; return false; }
            JSValue fields = JS_GetPropertyStr(ctx, value, "fields");
            if (JS_IsException(fields)) { error = exceptionText(ctx); return false; }
            if (!JS_IsObject(fields)) { JS_FreeValue(ctx, fields); error = "script result is not a Struct.define({...}) value"; return false; }

            JSPropertyEnum* properties = nullptr; std::uint32_t count = 0;
            if (JS_GetOwnPropertyNames(ctx, &properties, &count, fields, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) { JS_FreeValue(ctx, fields); error = exceptionText(ctx); return false; }
            definition.Fields.clear(); definition.Fields.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (++fieldCount > MaximumFields) { error = "Struct definition contains too many fields"; break; }
                const char* name = JS_AtomToCString(ctx, properties[i].atom); if (!name) { error = exceptionText(ctx); break; }
                JSValue descriptor = JS_GetProperty(ctx, fields, properties[i].atom);
                StructExperimentField field; field.Name = name; JS_FreeCString(ctx, name);
                if (JS_IsException(descriptor)) { error = exceptionText(ctx); JS_FreeValue(ctx, descriptor); break; }
                const bool ok = parseField(ctx, descriptor, field, error, depth, fieldCount); JS_FreeValue(ctx, descriptor); if (!ok) break;
                definition.Fields.emplace_back(std::move(field));
            }
            js_free(ctx, properties); JS_FreeValue(ctx, fields); return error.empty();
        }
    }

    bool runtimeEvaluateStructExperiment(const std::string_view source, StructExperimentDefinition& definition, std::string& error)
    {
        std::string transpiled;
        if (!runtimeTranspileTypeScript(source, transpiled, error)) return false;
        const std::string wrapped = std::string(R"JS("use strict";
const __quartzSizes = Object.freeze({i8:1n,u8:1n,i16:2n,u16:2n,i32:4n,u32:4n,i64:8n,u64:8n,f32:4n,f64:8n,bool:1n});
const __quartzAddress = value => typeof value === "bigint" ? value : BigInt(value);
const __quartzScalar = (kind, offset) => Object.freeze({kind, offset:__quartzAddress(offset), size:__quartzSizes[kind]});
const Field = Object.freeze({
    Int8:offset=>__quartzScalar("i8",offset), UInt8:offset=>__quartzScalar("u8",offset),
    Int16:offset=>__quartzScalar("i16",offset), UInt16:offset=>__quartzScalar("u16",offset),
    Int32:offset=>__quartzScalar("i32",offset), UInt32:offset=>__quartzScalar("u32",offset),
    Int64:offset=>__quartzScalar("i64",offset), UInt64:offset=>__quartzScalar("u64",offset),
    Float32:offset=>__quartzScalar("f32",offset), Float64:offset=>__quartzScalar("f64",offset), Boolean:offset=>__quartzScalar("bool",offset),
    Pointer:(offset,struct)=>Object.freeze({kind:"pointer",offset:__quartzAddress(offset),struct}),
    Struct:(offset,struct)=>Object.freeze({kind:"struct",offset:__quartzAddress(offset),struct}),
    Array:(offset,element,count)=>Object.freeze({kind:"array",offset:__quartzAddress(offset),element,count})
});
const Struct = Object.freeze({define(fields){return Object.freeze({fields:Object.freeze({...fields})});}});
(() => {
)JS") + transpiled + "\n})()";

        struct Deadline { std::chrono::steady_clock::time_point End; } deadline{std::chrono::steady_clock::now() + std::chrono::milliseconds(25)};
        JSRuntime* runtime = JS_NewRuntime(); if (!runtime) { error = "could not create QuickJS runtime"; return false; }
        JS_SetRuntimeInfo(runtime, "Quartz object experiment"); JS_SetMemoryLimit(runtime, ExperimentMemoryLimit); JS_SetMaxStackSize(runtime, ExperimentStackLimit); JS_SetCanBlock(runtime, false);
        JS_SetInterruptHandler(runtime, [](JSRuntime*, void* opaque) { return std::chrono::steady_clock::now() >= static_cast<Deadline*>(opaque)->End ? 1 : 0; }, &deadline);
        JSContext* ctx = JS_NewContext(runtime); if (!ctx) { JS_FreeRuntime(runtime); error = "could not create QuickJS context"; return false; }
        JSValue result = JS_Eval(ctx, wrapped.data(), wrapped.size(), "<object-experiment.ts>", JS_EVAL_TYPE_GLOBAL);
        bool ok = false;
        if (JS_IsException(result)) error = exceptionText(ctx);
        else { std::size_t fieldCount = 0; error.clear(); ok = parseDefinition(ctx, result, definition, error, 0, fieldCount); }
        JS_FreeValue(ctx, result); JS_FreeContext(ctx); JS_FreeRuntime(runtime); return ok;
    }

    const char* runtimeStructExperimentFieldKindName(const StructExperimentFieldKind kind) noexcept
    {
        switch (kind)
        {
        case StructExperimentFieldKind::I8: return "int8"; case StructExperimentFieldKind::U8: return "uint8";
        case StructExperimentFieldKind::I16: return "int16"; case StructExperimentFieldKind::U16: return "uint16";
        case StructExperimentFieldKind::I32: return "int32"; case StructExperimentFieldKind::U32: return "uint32";
        case StructExperimentFieldKind::I64: return "int64"; case StructExperimentFieldKind::U64: return "uint64";
        case StructExperimentFieldKind::F32: return "float32"; case StructExperimentFieldKind::F64: return "float64";
        case StructExperimentFieldKind::Bool: return "bool"; case StructExperimentFieldKind::Pointer: return "pointer";
        case StructExperimentFieldKind::Struct: return "struct"; case StructExperimentFieldKind::Array: return "array";
        }
        return "unknown";
    }
}
