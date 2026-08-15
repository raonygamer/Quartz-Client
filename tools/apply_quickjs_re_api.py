#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def read(path): return (ROOT / path).read_text()
def write(path, text):
    target = ROOT / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text)
def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1: raise RuntimeError(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)

# Shared internal state so the low-level RE API can live outside the already-large core QuickJS TU.
write("src/runtime/QuickJSInternal.hpp", r'''#pragma once
#include <quickjs.h>
#include <chrono>

namespace quartz::client
{
    class RuntimeBindingEngine;
    struct RuntimeBinding;
    struct RuntimeSignalContext;

    struct RuntimeQuickJSDeadline
    {
        std::chrono::steady_clock::time_point Deadline{};
        bool Active = false;
        bool Interrupted = false;
    };

    struct RuntimeQuickJSContext
    {
        RuntimeBindingEngine* Engine = nullptr;
        RuntimeBinding* Binding = nullptr;
        const RuntimeSignalContext* SignalContext = nullptr;
        RuntimeQuickJSDeadline* Execution = nullptr;
    };

    inline bool runtimeQuickJSDeadlineExpired(RuntimeQuickJSContext& context) noexcept
    {
        if (!context.Execution || !context.Execution->Active || std::chrono::steady_clock::now() < context.Execution->Deadline) return false;
        context.Execution->Interrupted = true;
        return true;
    }

    void runtimeInstallQuickJSLowLevelApi(JSContext* ctx, JSValueConst api);
}
''')

write("src/runtime/QuickJSApi.cpp", r'''#include "QuickJSInternal.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/native/NativeDisassembly.hpp"
#include "quartz/client/runtime/QuickJS.hpp"
#include <libhat/scanner.hpp>
#include <libhat/signature.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quartz::client
{
    namespace
    {
        constexpr std::size_t MaximumByteTransfer = 1024 * 1024;
        constexpr std::size_t SignatureReadChunk = 2 * 1024 * 1024;
        constexpr std::uint32_t MaximumLoopIterations = 100'000;
        constexpr std::uint32_t MaximumDisassemblyInstructions = 256;
        constexpr double MaximumSafeInteger = 9007199254740991.0;

        RuntimeQuickJSContext* scriptContext(JSContext* ctx) noexcept { return static_cast<RuntimeQuickJSContext*>(JS_GetContextOpaque(ctx)); }

        JSValue jsError(JSContext* ctx, const std::string& text) { return JS_ThrowInternalError(ctx, "%s", text.c_str()); }

        bool valueToUInt64(JSContext* ctx, JSValueConst value, std::uint64_t& output)
        {
            if (JS_IsBigInt(ctx, value))
            {
                const char* text = JS_ToCString(ctx, value); if (!text) return false;
                const char* end = text + std::strlen(text); const auto [ptr, ec] = std::from_chars(text, end, output, 10); JS_FreeCString(ctx, text);
                return ec == std::errc{} && ptr == end;
            }
            if (!JS_IsNumber(value)) return false;
            double number = 0.0; if (JS_ToFloat64(ctx, &number, value) < 0 || !std::isfinite(number) || number < 0.0 || std::floor(number) != number || number > MaximumSafeInteger) return false;
            output = static_cast<std::uint64_t>(number); return true;
        }

        bool valueToInt64(JSContext* ctx, JSValueConst value, std::int64_t& output)
        {
            if (JS_IsBigInt(ctx, value)) return JS_ToInt64Ext(ctx, &output, value) >= 0;
            if (!JS_IsNumber(value)) return false;
            double number = 0.0; if (JS_ToFloat64(ctx, &number, value) < 0 || !std::isfinite(number) || std::floor(number) != number || number < static_cast<double>(std::numeric_limits<std::int64_t>::min()) || number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) return false;
            output = static_cast<std::int64_t>(number); return true;
        }

        bool valueToAddress(JSContext* ctx, JSValueConst value, std::uintptr_t& output)
        {
            std::uint64_t raw = 0; if (!valueToUInt64(ctx, value, raw) || raw > std::numeric_limits<std::uintptr_t>::max()) return false; output = static_cast<std::uintptr_t>(raw); return true;
        }

        bool valueToPid(JSContext* ctx, JSValueConst value, pid_t& output)
        {
            std::int64_t raw = 0; if (!valueToInt64(ctx, value, raw) || raw <= 0 || raw > std::numeric_limits<pid_t>::max()) return false; output = static_cast<pid_t>(raw); return true;
        }

        bool valueToCount(JSContext* ctx, JSValueConst value, std::uint32_t& output, const std::uint32_t maximum)
        {
            std::uint64_t raw = 0; if (!valueToUInt64(ctx, value, raw) || raw > maximum) return false; output = static_cast<std::uint32_t>(raw); return true;
        }

        JSValue addressValue(JSContext* ctx, const std::uintptr_t address) { return JS_NewBigUint64(ctx, static_cast<std::uint64_t>(address)); }

        template<typename T>
        bool writeScalar(const pid_t pid, const std::uintptr_t address, const T& value, std::string& error)
        {
            std::array<std::uint8_t, sizeof(T)> bytes{}; std::memcpy(bytes.data(), &value, sizeof(T)); return runtimeWriteProcessMemory(pid, address, bytes, error);
        }

        enum class ScalarType { U8, I8, U16, I16, U32, I32, U64, I64, F32, F64, Bool, Pointer };

        bool parseScalarType(JSContext* ctx, JSValueConst value, ScalarType& type)
        {
            if (JS_IsUndefined(value)) { type = ScalarType::U32; return true; }
            const char* raw = JS_ToCString(ctx, value); if (!raw) return false; std::string text(raw); JS_FreeCString(ctx, raw);
            std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (text == "u8") type = ScalarType::U8; else if (text == "i8") type = ScalarType::I8;
            else if (text == "u16") type = ScalarType::U16; else if (text == "i16") type = ScalarType::I16;
            else if (text == "u32") type = ScalarType::U32; else if (text == "i32") type = ScalarType::I32;
            else if (text == "u64") type = ScalarType::U64; else if (text == "i64") type = ScalarType::I64;
            else if (text == "f32" || text == "float") type = ScalarType::F32; else if (text == "f64" || text == "double") type = ScalarType::F64;
            else if (text == "bool" || text == "boolean") type = ScalarType::Bool; else if (text == "ptr" || text == "pointer" || text == "address") type = ScalarType::Pointer;
            else return false;
            return true;
        }

        JSValue jsProcesses(JSContext* ctx, JSValueConst, int, JSValueConst*)
        {
            const auto processes = enumerateRuntimeProcesses(); JSValue array = JS_NewArray(ctx);
            for (std::uint32_t i = 0; i < processes.size(); ++i)
            {
                const auto& process = processes[i]; JSValue object = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, object, "pid", JS_NewInt32(ctx, process.Pid)); JS_SetPropertyStr(ctx, object, "name", JS_NewString(ctx, process.Name.c_str())); JS_SetPropertyStr(ctx, object, "exe", JS_NewString(ctx, process.Exe.c_str())); JS_SetPropertyStr(ctx, object, "title", JS_NewString(ctx, process.Title.c_str())); JS_SetPropertyStr(ctx, object, "commandLine", JS_NewString(ctx, process.CommandLine.c_str())); JS_SetPropertyUint32(ctx, array, i, object);
            }
            return array;
        }

        JSValue jsModules(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            pid_t pid = 0; if (argc < 1 || !valueToPid(ctx, argv[0], pid)) return JS_ThrowTypeError(ctx, "q.re.modules(pid): pid must be a positive integer");
            const auto modules = enumerateRuntimeModules(pid); JSValue array = JS_NewArray(ctx);
            for (std::uint32_t i = 0; i < modules.size(); ++i)
            {
                const auto& module = modules[i]; JSValue object = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, object, "base", addressValue(ctx, module.Base)); JS_SetPropertyStr(ctx, object, "end", addressValue(ctx, module.End)); JS_SetPropertyStr(ctx, object, "size", JS_NewBigUint64(ctx, static_cast<std::uint64_t>(module.End - module.Base))); JS_SetPropertyStr(ctx, object, "name", JS_NewString(ctx, module.Name.c_str())); JS_SetPropertyStr(ctx, object, "path", JS_NewString(ctx, module.Path.c_str())); JS_SetPropertyUint32(ctx, array, i, object);
            }
            return array;
        }

        JSValue jsRegions(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            pid_t pid = 0; if (argc < 1 || !valueToPid(ctx, argv[0], pid)) return JS_ThrowTypeError(ctx, "q.re.regions(pid): pid must be a positive integer");
            const auto regions = enumerateRuntimeRegions(pid); JSValue array = JS_NewArray(ctx);
            for (std::uint32_t i = 0; i < regions.size(); ++i)
            {
                const auto& region = regions[i]; JSValue object = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, object, "base", addressValue(ctx, region.Base)); JS_SetPropertyStr(ctx, object, "end", addressValue(ctx, region.End)); JS_SetPropertyStr(ctx, object, "size", JS_NewBigUint64(ctx, static_cast<std::uint64_t>(region.End - region.Base))); JS_SetPropertyStr(ctx, object, "readable", JS_NewBool(ctx, region.Readable)); JS_SetPropertyStr(ctx, object, "writable", JS_NewBool(ctx, region.Writable)); JS_SetPropertyStr(ctx, object, "executable", JS_NewBool(ctx, region.Executable)); JS_SetPropertyStr(ctx, object, "path", JS_NewString(ctx, region.Path.c_str())); JS_SetPropertyUint32(ctx, array, i, object);
            }
            return array;
        }

        JSValue jsProcessAlive(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            pid_t pid = 0; if (argc < 1 || !valueToPid(ctx, argv[0], pid)) return JS_ThrowTypeError(ctx, "q.re.processAlive(pid): pid must be a positive integer"); return JS_NewBool(ctx, runtimeProcessIsAlive(pid));
        }

        JSValue jsRead(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            pid_t pid = 0; std::uintptr_t address = 0; ScalarType type{};
            if (argc < 2 || !valueToPid(ctx, argv[0], pid) || !valueToAddress(ctx, argv[1], address)) return JS_ThrowTypeError(ctx, "q.re.read(pid, address, type?): invalid pid/address");
            if (!parseScalarType(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, type)) return JS_ThrowTypeError(ctx, "unknown scalar type");
            std::string error;
            switch (type)
            {
            case ScalarType::U8: { std::uint8_t value{}; if (!readProcessMemoryValue(pid, address, value, error)) return jsError(ctx, error); return JS_NewUint32(ctx, value); }
            case ScalarType::I8: { std::int8_t value{}; if (!readProcessMemoryValue(pid, address, value, error)) return jsError(ctx, error); return JS_NewInt32(ctx, value); }
            case ScalarType::U16: { std::uint16_t value{}; if (!readProcessMemoryValue(pid, address, value, error)) return jsError(ctx, error); return JS_NewUint32(ctx, value); }
            case ScalarType::I16: { std::int16_t value{}; if (!readProcessMemoryValue(pid, address, value, error)) return jsError(ctx, error); return JS_NewInt32(ctx, value); }
            case ScalarType::U32: { std::uint32_t value{}; if (!readProcessMemoryValue(pid, address, value, error)) return jsError(ctx, error); return JS_NewUint32(ctx, value); }
            case ScalarType::I32: { std::int32_t value{}; if (!readProcessMemoryValue(pid, address, value, error)) return jsError(ctx, error); return JS_NewInt32(ctx, value); }
            case ScalarType::U64: { std::uint64_t value{}; if (!readProcessMemoryValue(pid, address, value, error)) return jsError(ctx, error); return JS_NewBigUint64(ctx, value); }
            case ScalarType::I64: { std::int64_t value{}; if (!readProcessMemoryValue(pid, address, value, error)) return jsError(ctx, error); return JS_NewBigInt64(ctx, value); }
            case ScalarType::F32: { float value{}; if (!readProcessMemoryValue(pid, address, value, error)) return jsError(ctx, error); return JS_NewFloat64(ctx, value); }
            case ScalarType::F64: { double value{}; if (!readProcessMemoryValue(pid, address, value, error)) return jsError(ctx, error); return JS_NewFloat64(ctx, value); }
            case ScalarType::Bool: { std::uint8_t value{}; if (!readProcessMemoryValue(pid, address, value, error)) return jsError(ctx, error); return JS_NewBool(ctx, value != 0); }
            case ScalarType::Pointer: { const auto mode = runtimeProcessX86Mode(pid); if (mode == RuntimeX86Mode::X86) { std::uint32_t value{}; if (!readProcessMemoryValue(pid, address, value, error)) return jsError(ctx, error); return JS_NewBigUint64(ctx, value); } std::uint64_t value{}; if (!readProcessMemoryValue(pid, address, value, error)) return jsError(ctx, error); return JS_NewBigUint64(ctx, value); }
            }
            return JS_UNDEFINED;
        }

        JSValue jsWrite(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            pid_t pid = 0; std::uintptr_t address = 0; ScalarType type{};
            if (argc < 4 || !valueToPid(ctx, argv[0], pid) || !valueToAddress(ctx, argv[1], address)) return JS_ThrowTypeError(ctx, "q.re.write(pid, address, type, value): invalid pid/address");
            if (!parseScalarType(ctx, argv[2], type)) return JS_ThrowTypeError(ctx, "unknown scalar type");
            std::string error; std::uint64_t unsignedValue = 0; std::int64_t signedValue = 0; double number = 0.0;
#define QUARTZ_WRITE_UNSIGNED(T) do { if (!valueToUInt64(ctx, argv[3], unsignedValue) || unsignedValue > std::numeric_limits<T>::max()) return JS_ThrowRangeError(ctx, "value is out of range"); const T value = static_cast<T>(unsignedValue); if (!writeScalar(pid, address, value, error)) return jsError(ctx, error); } while (false)
#define QUARTZ_WRITE_SIGNED(T) do { if (!valueToInt64(ctx, argv[3], signedValue) || signedValue < std::numeric_limits<T>::min() || signedValue > std::numeric_limits<T>::max()) return JS_ThrowRangeError(ctx, "value is out of range"); const T value = static_cast<T>(signedValue); if (!writeScalar(pid, address, value, error)) return jsError(ctx, error); } while (false)
            switch (type)
            {
            case ScalarType::U8: QUARTZ_WRITE_UNSIGNED(std::uint8_t); break; case ScalarType::I8: QUARTZ_WRITE_SIGNED(std::int8_t); break;
            case ScalarType::U16: QUARTZ_WRITE_UNSIGNED(std::uint16_t); break; case ScalarType::I16: QUARTZ_WRITE_SIGNED(std::int16_t); break;
            case ScalarType::U32: QUARTZ_WRITE_UNSIGNED(std::uint32_t); break; case ScalarType::I32: QUARTZ_WRITE_SIGNED(std::int32_t); break;
            case ScalarType::U64: QUARTZ_WRITE_UNSIGNED(std::uint64_t); break; case ScalarType::I64: QUARTZ_WRITE_SIGNED(std::int64_t); break;
            case ScalarType::F32: if (JS_ToFloat64(ctx, &number, argv[3]) < 0 || !std::isfinite(number)) return JS_ThrowTypeError(ctx, "value must be finite"); { const float value = static_cast<float>(number); if (!writeScalar(pid, address, value, error)) return jsError(ctx, error); } break;
            case ScalarType::F64: if (JS_ToFloat64(ctx, &number, argv[3]) < 0 || !std::isfinite(number)) return JS_ThrowTypeError(ctx, "value must be finite"); if (!writeScalar(pid, address, number, error)) return jsError(ctx, error); break;
            case ScalarType::Bool: { const int boolean = JS_ToBool(ctx, argv[3]); if (boolean < 0) return JS_EXCEPTION; const std::uint8_t value = boolean ? 1 : 0; if (!writeScalar(pid, address, value, error)) return jsError(ctx, error); } break;
            case ScalarType::Pointer: { if (!valueToUInt64(ctx, argv[3], unsignedValue)) return JS_ThrowTypeError(ctx, "pointer value must be an integer/BigInt"); if (runtimeProcessX86Mode(pid) == RuntimeX86Mode::X86) { if (unsignedValue > UINT32_MAX) return JS_ThrowRangeError(ctx, "pointer does not fit x86 target"); const std::uint32_t value = static_cast<std::uint32_t>(unsignedValue); if (!writeScalar(pid, address, value, error)) return jsError(ctx, error); } else { const std::uint64_t value = unsignedValue; if (!writeScalar(pid, address, value, error)) return jsError(ctx, error); } } break;
            }
#undef QUARTZ_WRITE_UNSIGNED
#undef QUARTZ_WRITE_SIGNED
            return JS_TRUE;
        }

        JSValue jsReadBytes(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            pid_t pid = 0; std::uintptr_t address = 0; std::uint32_t length = 0;
            if (argc < 3 || !valueToPid(ctx, argv[0], pid) || !valueToAddress(ctx, argv[1], address) || !valueToCount(ctx, argv[2], length, MaximumByteTransfer)) return JS_ThrowTypeError(ctx, "q.re.readBytes(pid, address, length): invalid argument or length > 1 MiB");
            std::vector<std::uint8_t> bytes(length); std::string error; if (length && !readProcessMemoryBlock(pid, address, bytes, error)) return jsError(ctx, error);
            JSValue array = JS_NewArray(ctx); for (std::uint32_t i = 0; i < length; ++i) JS_SetPropertyUint32(ctx, array, i, JS_NewUint32(ctx, bytes[i])); return array;
        }

        bool byteArray(JSContext* ctx, JSValueConst value, std::vector<std::uint8_t>& bytes, std::string& error)
        {
            if (JS_IsString(value))
            {
                const char* text = JS_ToCString(ctx, value); if (!text) return false; const bool ok = runtimeParseHexBytes(text, bytes, error); JS_FreeCString(ctx, text); return ok && bytes.size() <= MaximumByteTransfer;
            }
            if (!JS_IsArray(ctx, value)) { error = "bytes must be a numeric array or hexadecimal string"; return false; }
            JSValue lengthValue = JS_GetPropertyStr(ctx, value, "length"); std::uint64_t length = 0; const int lengthOk = JS_ToIndex(ctx, &length, lengthValue); JS_FreeValue(ctx, lengthValue);
            if (lengthOk < 0 || length > MaximumByteTransfer) { error = "byte array exceeds 1 MiB"; return false; }
            bytes.resize(static_cast<std::size_t>(length));
            for (std::uint32_t i = 0; i < length; ++i)
            {
                JSValue item = JS_GetPropertyUint32(ctx, value, i); std::uint32_t byte = 0; const int ok = JS_ToUint32(ctx, &byte, item); JS_FreeValue(ctx, item); if (ok < 0 || byte > 0xFF) { error = "byte array values must be 0..255"; return false; } bytes[i] = static_cast<std::uint8_t>(byte);
            }
            return true;
        }

        JSValue jsWriteBytes(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            pid_t pid = 0; std::uintptr_t address = 0; if (argc < 3 || !valueToPid(ctx, argv[0], pid) || !valueToAddress(ctx, argv[1], address)) return JS_ThrowTypeError(ctx, "q.re.writeBytes(pid, address, bytes): invalid pid/address");
            std::vector<std::uint8_t> bytes; std::string error; if (!byteArray(ctx, argv[2], bytes, error)) return JS_ThrowTypeError(ctx, "%s", error.c_str()); if (!runtimeWriteProcessMemory(pid, address, bytes, error)) return jsError(ctx, error); return JS_TRUE;
        }

        JSValue jsSignature(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            pid_t pid = 0; if (argc < 2 || !valueToPid(ctx, argv[0], pid)) return JS_ThrowTypeError(ctx, "q.re.signature(pid, pattern, executableOnly?): invalid pid");
            const char* patternText = JS_ToCString(ctx, argv[1]); if (!patternText) return JS_EXCEPTION; std::vector<std::uint8_t> bytes, masks; std::string error; const bool parsed = parseRuntimeHexPattern(patternText, bytes, masks, error); JS_FreeCString(ctx, patternText); if (!parsed || bytes.empty()) return JS_ThrowTypeError(ctx, "%s", error.empty() ? "empty signature" : error.c_str());
            bool executableOnly = true; if (argc > 2 && !JS_IsUndefined(argv[2])) { const int value = JS_ToBool(ctx, argv[2]); if (value < 0) return JS_EXCEPTION; executableOnly = value != 0; }
            hat::signature signature; signature.reserve(bytes.size()); for (std::size_t i = 0; i < bytes.size(); ++i) signature.emplace_back(static_cast<std::byte>(bytes[i]), static_cast<std::byte>(masks[i]));
            const std::size_t overlap = signature.size() > 1 ? signature.size() - 1 : 0; const hat::scan_hint hint = executableOnly ? hat::scan_hint::x86_64 : hat::scan_hint::none; std::vector<std::uint8_t> buffer; auto* state = scriptContext(ctx);
            for (const auto& region : enumerateRuntimeRegions(pid))
            {
                if (!region.Readable || (executableOnly && !region.Executable)) continue; std::uintptr_t cursor = region.Base;
                while (cursor < region.End)
                {
                    if (state && runtimeQuickJSDeadlineExpired(*state)) return JS_ThrowInternalError(ctx, "QuickJS execution deadline exceeded during signature scan");
                    const std::size_t remaining = static_cast<std::size_t>(region.End - cursor); const std::size_t readSize = std::min(remaining, SignatureReadChunk + overlap); if (readSize < signature.size()) break; buffer.resize(readSize);
                    std::string readError; if (!readProcessMemoryBlock(pid, cursor, buffer, readError)) break; const std::span<const std::byte> data{reinterpret_cast<const std::byte*>(buffer.data()), buffer.size()}; const auto match = hat::find_pattern(data, signature, hat::scan_alignment::X1, hint);
                    if (match.has_result()) return addressValue(ctx, cursor + static_cast<std::size_t>(match.get() - data.data())); const std::size_t step = readSize > overlap ? readSize - overlap : readSize; if (!step) break; cursor += step;
                }
            }
            return JS_UNDEFINED;
        }

        JSValue jsDisassemble(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            pid_t pid = 0; std::uintptr_t address = 0; std::uint32_t count = 16; if (argc < 2 || !valueToPid(ctx, argv[0], pid) || !valueToAddress(ctx, argv[1], address)) return JS_ThrowTypeError(ctx, "q.re.disassemble(pid, address, count?): invalid pid/address");
            if (argc > 2 && !valueToCount(ctx, argv[2], count, MaximumDisassemblyInstructions)) return JS_ThrowRangeError(ctx, "disassembly count must be 0..256");
            const RuntimeX86Mode mode = runtimeProcessX86Mode(pid); JSValue array = JS_NewArray(ctx); std::uintptr_t cursor = address; auto* state = scriptContext(ctx);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (state && runtimeQuickJSDeadlineExpired(*state)) { JS_FreeValue(ctx, array); return JS_ThrowInternalError(ctx, "QuickJS execution deadline exceeded during disassembly"); }
                std::array<std::uint8_t, 15> bytes{}; std::string error; if (!readProcessMemoryBlock(pid, cursor, bytes, error)) { JS_FreeValue(ctx, array); return jsError(ctx, error); }
                std::string text; std::size_t length = 0; if (!runtimeDecodeProcessInstructionText(mode, bytes, cursor, text, length) || length == 0 || length > bytes.size()) { char fallback[32]; std::snprintf(fallback, sizeof(fallback), "db 0x%02X", bytes[0]); text = fallback; length = 1; }
                JSValue instruction = JS_NewObject(ctx); JS_SetPropertyStr(ctx, instruction, "address", addressValue(ctx, cursor)); JS_SetPropertyStr(ctx, instruction, "size", JS_NewUint32(ctx, static_cast<std::uint32_t>(length))); JS_SetPropertyStr(ctx, instruction, "text", JS_NewString(ctx, text.c_str())); const std::string formatted = runtimeFormatHexBytes(std::span<const std::uint8_t>(bytes).first(length)); JS_SetPropertyStr(ctx, instruction, "bytes", JS_NewString(ctx, formatted.c_str())); JS_SetPropertyUint32(ctx, array, i, instruction); cursor += length;
            }
            return array;
        }

        JSValue jsLoop(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            std::uint32_t count = 0; if (argc < 2 || !valueToCount(ctx, argv[0], count, MaximumLoopIterations) || !JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "q.loop(count, callback): count must be <= 100000 and callback must be a function"); auto* state = scriptContext(ctx); std::uint32_t executed = 0;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (state && runtimeQuickJSDeadlineExpired(*state)) return JS_ThrowInternalError(ctx, "QuickJS execution deadline exceeded in q.loop"); JSValue index = JS_NewUint32(ctx, i); JSValue result = JS_Call(ctx, argv[1], JS_UNDEFINED, 1, &index); JS_FreeValue(ctx, index); if (JS_IsException(result)) return result; ++executed; const bool stop = JS_IsBool(result) && JS_ToBool(ctx, result) == 0; JS_FreeValue(ctx, result); if (stop) break;
            }
            return JS_NewUint32(ctx, executed);
        }

        JSValue jsAddressLoop(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            std::uintptr_t start = 0; std::uint32_t count = 0; std::int64_t stride = 0; if (argc < 4 || !valueToAddress(ctx, argv[0], start) || !valueToCount(ctx, argv[1], count, MaximumLoopIterations) || !valueToInt64(ctx, argv[2], stride) || !JS_IsFunction(ctx, argv[3])) return JS_ThrowTypeError(ctx, "q.re.loop(start, count, stride, callback): invalid arguments"); auto* state = scriptContext(ctx); std::uint32_t executed = 0;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (state && runtimeQuickJSDeadlineExpired(*state)) return JS_ThrowInternalError(ctx, "QuickJS execution deadline exceeded in q.re.loop"); const __int128 computed = static_cast<__int128>(start) + static_cast<__int128>(stride) * i; if (computed < 0 || computed > static_cast<__int128>(std::numeric_limits<std::uintptr_t>::max())) return JS_ThrowRangeError(ctx, "q.re.loop address overflow"); JSValue args[2]{addressValue(ctx, static_cast<std::uintptr_t>(computed)), JS_NewUint32(ctx, i)}; JSValue result = JS_Call(ctx, argv[3], JS_UNDEFINED, 2, args); JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]); if (JS_IsException(result)) return result; ++executed; const bool stop = JS_IsBool(result) && JS_ToBool(ctx, result) == 0; JS_FreeValue(ctx, result); if (stop) break;
            }
            return JS_NewUint32(ctx, executed);
        }
    }

    void runtimeInstallQuickJSLowLevelApi(JSContext* ctx, JSValueConst api)
    {
        JS_SetPropertyStr(ctx, api, "loop", JS_NewCFunction(ctx, jsLoop, "loop", 2)); JSValue re = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, re, "processes", JS_NewCFunction(ctx, jsProcesses, "processes", 0)); JS_SetPropertyStr(ctx, re, "modules", JS_NewCFunction(ctx, jsModules, "modules", 1)); JS_SetPropertyStr(ctx, re, "regions", JS_NewCFunction(ctx, jsRegions, "regions", 1)); JS_SetPropertyStr(ctx, re, "processAlive", JS_NewCFunction(ctx, jsProcessAlive, "processAlive", 1));
        JS_SetPropertyStr(ctx, re, "read", JS_NewCFunction(ctx, jsRead, "read", 3)); JS_SetPropertyStr(ctx, re, "write", JS_NewCFunction(ctx, jsWrite, "write", 4)); JS_SetPropertyStr(ctx, re, "readBytes", JS_NewCFunction(ctx, jsReadBytes, "readBytes", 3)); JS_SetPropertyStr(ctx, re, "writeBytes", JS_NewCFunction(ctx, jsWriteBytes, "writeBytes", 3)); JS_SetPropertyStr(ctx, re, "signature", JS_NewCFunction(ctx, jsSignature, "signature", 3)); JS_SetPropertyStr(ctx, re, "disassemble", JS_NewCFunction(ctx, jsDisassemble, "disassemble", 3)); JS_SetPropertyStr(ctx, re, "loop", JS_NewCFunction(ctx, jsAddressLoop, "loop", 4)); JS_SetPropertyStr(ctx, api, "re", re);
    }

    std::string_view runtimeQuickJSTypeDeclarations() noexcept
    {
        static constexpr std::string_view Declarations = R"TS(// Quartz QuickJS binding API
// Generated by Quartz Client. Scripts are function bodies with `q` available.

type QuartzId = number | bigint | string;
type QuartzAddress = bigint;
type QuartzScalarType = "u8" | "i8" | "u16" | "i16" | "u32" | "i32" | "u64" | "i64" | "f32" | "float" | "f64" | "double" | "bool" | "boolean" | "ptr" | "pointer" | "address";

interface QuartzProcessInfo { pid: number; name: string; exe: string; title: string; commandLine: string; }
interface QuartzModuleInfo { base: QuartzAddress; end: QuartzAddress; size: bigint; name: string; path: string; }
interface QuartzRegionInfo { base: QuartzAddress; end: QuartzAddress; size: bigint; readable: boolean; writable: boolean; executable: boolean; path: string; }
interface QuartzInstruction { address: QuartzAddress; size: number; text: string; bytes: string; }

interface QuartzReverseEngineeringAPI {
    processes(): QuartzProcessInfo[];
    modules(pid: number): QuartzModuleInfo[];
    regions(pid: number): QuartzRegionInfo[];
    processAlive(pid: number): boolean;
    read(pid: number, address: QuartzAddress | number, type?: QuartzScalarType): number | bigint | boolean;
    write(pid: number, address: QuartzAddress | number, type: QuartzScalarType, value: number | bigint | boolean): true;
    readBytes(pid: number, address: QuartzAddress | number, length: number): number[];
    writeBytes(pid: number, address: QuartzAddress | number, bytes: number[] | string): true;
    /** Hex/wildcard pattern. Returns the first match or undefined. */
    signature(pid: number, pattern: string, executableOnly?: boolean): QuartzAddress | undefined;
    disassemble(pid: number, address: QuartzAddress | number, count?: number): QuartzInstruction[];
    /** Iterates addresses start + index * stride. Return false from callback to stop. */
    loop(start: QuartzAddress | number, count: number, stride: number | bigint, callback: (address: QuartzAddress, index: number) => void | boolean): number;
}

interface QuartzBindingAPI {
    binding(idOrName: QuartzId): number | undefined;
    raw(idOrName: QuartzId): number | undefined;
    text(idOrName: QuartzId): string | undefined;
    address(idOrName: QuartzId): QuartzAddress | undefined;
    bank(idOrName: QuartzId): number | bigint | boolean | string | undefined;
    control(idOrName: QuartzId): boolean | undefined;
    triggered(idOrName: QuartzId): boolean | undefined;
    log(...values: unknown[]): void;
    /** Bounded native loop helper. Return false from callback to stop. */
    loop(count: number, callback: (index: number) => void | boolean): number;
    readonly re: QuartzReverseEngineeringAPI;
    readonly time: number;
    readonly deltaTime: number;
    readonly previous: number;
    readonly previousRaw: number;
    readonly id: bigint;
    readonly name: string;
    state: Record<string, any>;
}

declare const q: QuartzBindingAPI;
)TS";
        return Declarations;
    }

    std::filesystem::path runtimeQuickJSTypeDeclarationsPath() { return settingsPath().parent_path() / "quartz-bindings.d.ts"; }

    bool runtimeSaveQuickJSTypeDeclarations(std::string& error)
    {
        const auto path = runtimeQuickJSTypeDeclarationsPath(); std::error_code ec; std::filesystem::create_directories(path.parent_path(), ec); std::ofstream file(path, std::ios::binary | std::ios::trunc); if (!file) { error = "could not open " + path.string(); return false; } const auto declarations = runtimeQuickJSTypeDeclarations(); file.write(declarations.data(), static_cast<std::streamsize>(declarations.size())); if (!file) { error = "could not write " + path.string(); return false; } error.clear(); return true;
    }
}
''')

# Public declarations.
path = "include/quartz/client/runtime/QuickJS.hpp"
text = read(path)
text = replace_once(text, '#include <cstdint>\n', '#include <cstdint>\n#include <filesystem>\n#include <string>\n#include <string_view>\n', 'QuickJS.hpp includes')
text = replace_once(text, '    void runtimeResetScriptBinding(std::uint64_t bindingId) noexcept;\n', '    void runtimeResetScriptBinding(std::uint64_t bindingId) noexcept;\n    std::string_view runtimeQuickJSTypeDeclarations() noexcept;\n    std::filesystem::path runtimeQuickJSTypeDeclarationsPath();\n    bool runtimeSaveQuickJSTypeDeclarations(std::string& error);\n', 'QuickJS.hpp declarations')
write(path, text)

# Core QuickJS: fix the broken wrapper escaping and share context/deadline state with low-level API.
path = "src/runtime/QuickJS.cpp"
text = read(path)
text = replace_once(text, '#include "quartz/client/runtime/QuickJS.hpp"\n', '#include "quartz/client/runtime/QuickJS.hpp"\n#include "QuickJSInternal.hpp"\n', 'QuickJS core include')
text = replace_once(text, '        struct ScriptInstance\n        {\n            QuickJSRuntime* Owner = nullptr;\n            RuntimeBindingEngine* Engine = nullptr;\n            RuntimeBinding* Binding = nullptr;\n            const RuntimeSignalContext* SignalContext = nullptr;\n', '        struct ScriptInstance : RuntimeQuickJSContext\n        {\n', 'ScriptInstance base')
text = text.replace('static_cast<ScriptInstance*>(JS_GetContextOpaque(ctx))', 'static_cast<RuntimeQuickJSContext*>(JS_GetContextOpaque(ctx))')
text = replace_once(text, '            binding.HasAddress = false; binding.AddressValue = 0; binding.AddressProvenance.clear();\n', '            binding.HasString = false; binding.StringValue.clear(); binding.HasAddress = false; binding.AddressValue = 0; binding.AddressProvenance.clear();\n', 'clear stale script side channels')
text = replace_once(text, '            std::chrono::steady_clock::time_point Deadline{};\n            bool DeadlineActive = false;\n            bool Interrupted = false;\n', '            RuntimeQuickJSDeadline Execution;\n', 'QuickJS deadline fields')
text = replace_once(text, '                if (!self || !self->DeadlineActive || std::chrono::steady_clock::now() < self->Deadline) return 0;\n                self->Interrupted = true; return 1;\n', '                if (!self || !self->Execution.Active || std::chrono::steady_clock::now() < self->Execution.Deadline) return 0;\n                self->Execution.Interrupted = true; return 1;\n', 'QuickJS interrupt')
text = replace_once(text, '                Interrupted = false; DeadlineActive = true; Deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(std::clamp(milliseconds, 0.1f, 100.0f)));\n            }\n            void endDeadline() noexcept { DeadlineActive = false; }\n', '                Execution.Interrupted = false; Execution.Active = true; Execution.Deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(std::clamp(milliseconds, 0.1f, 100.0f)));\n            }\n            void endDeadline() noexcept { Execution.Active = false; }\n', 'QuickJS deadline methods')
text = replace_once(text, '                auto created = std::make_unique<ScriptInstance>(); created->Owner = this; created->Context = JS_NewContext(Runtime);\n', '                auto created = std::make_unique<ScriptInstance>(); created->Execution = &Execution; created->Context = JS_NewContext(Runtime);\n', 'QuickJS instance state')
text = replace_once(text, '                JS_SetContextOpaque(created->Context, created.get()); created->Api = JS_NewObject(created->Context);\n', '                JS_SetContextOpaque(created->Context, static_cast<RuntimeQuickJSContext*>(created.get())); created->Api = JS_NewObject(created->Context);\n', 'QuickJS opaque context')
text = replace_once(text, '                JS_SetPropertyStr(created->Context, created->Api, "log", JS_NewCFunction(created->Context, jsLog, "log", 1));\n                JS_DefinePropertyValueStr(created->Context, created->Api, "state", JS_NewObject(created->Context), JS_PROP_ENUMERABLE);\n', '                JS_SetPropertyStr(created->Context, created->Api, "log", JS_NewCFunction(created->Context, jsLog, "log", 1));\n                runtimeInstallQuickJSLowLevelApi(created->Context, created->Api);\n                JS_DefinePropertyValueStr(created->Context, created->Api, "state", JS_NewObject(created->Context), JS_PROP_ENUMERABLE);\n', 'install low-level API')
text = replace_once(text, '                std::string source = "(function(q){\\\\n\\\\\\\"use strict\\\\\\\";\\\\n"; source.append(body); source += "\\\\n})";\n', '                std::string source = "(function(q){\\n\\\"use strict\\\";\\n"; source.append(body); source += "\\n})";\n', 'fix QuickJS wrapper escaping')
text = text.replace('if (Interrupted)', 'if (Execution.Interrupted)')
write(path, text)

# evdev exposes a thread-safe global shortcut query.
path = "include/quartz/client/input/Input.hpp"
text = read(path)
text = replace_once(text, '        bool consumeRestoreRequest() noexcept { return _restoreRequested.exchange(false); }\n', '''        bool consumeRestoreRequest() noexcept { return _restoreRequested.exchange(false); }\n\n        bool shortcutDown(const std::uint16_t key, const bool ctrl, const bool alt, const bool shift) const\n        {\n            if (key > KEY_MAX) return false;\n            std::lock_guard lock(_mutex);\n            const bool ctrlDown = _keyDown[KEY_LEFTCTRL] || _keyDown[KEY_RIGHTCTRL];\n            const bool altDown = _keyDown[KEY_LEFTALT] || _keyDown[KEY_RIGHTALT];\n            const bool shiftDown = _keyDown[KEY_LEFTSHIFT] || _keyDown[KEY_RIGHTSHIFT];\n            return (!ctrl || ctrlDown) && (!alt || altDown) && (!shift || shiftDown) && _keyDown[key];\n        }\n''', 'evdev shortcut query')
text = replace_once(text, '''        void resetKeyState()\n        {\n            _keyDown.fill(false);\n            std::lock_guard lock(_mutex);\n            _state.Down.fill(0.0f);\n        }\n''', '''        void resetKeyState()\n        {\n            std::lock_guard lock(_mutex);\n            _keyDown.fill(false);\n            _state.Down.fill(0.0f);\n        }\n''', 'evdev reset locking')
start = text.index('        void handleKey(const input_event& event)\n')
end = text.index('        void handleLed(const input_event& event)\n', start)
text = text[:start] + '''        void handleKey(const input_event& event)\n        {\n            if (event.code > KEY_MAX) return;\n            const auto* binding = findReactiveKeyBinding(event.code);\n            std::lock_guard lock(_mutex);\n            const bool wasDown = _keyDown[event.code];\n            _keyDown[event.code] = event.value != 0;\n            if (event.value == 1 && !wasDown && restoreShortcutDown()) _restoreRequested.store(true);\n            if (!binding) return;\n            const std::size_t index = static_cast<std::size_t>(binding->Row) * Columns + binding->Column;\n            _state.Down[index] = event.value == 0 ? 0.0f : 1.0f;\n            if (event.value == 1 && !wasDown)\n            {\n                _state.Events[_state.NextEvent] = {static_cast<float>(binding->Column), static_cast<float>(binding->Row), now(), 1.0f};\n                _state.NextEvent = (_state.NextEvent + 1) % ReactiveKeyState::EventCount;\n                if (event.code == KEY_CAPSLOCK) _state.CapsLockActive = !_state.CapsLockActive;\n                if (event.code == KEY_SCROLLLOCK) _state.ScrollLockActive = !_state.ScrollLockActive;\n            }\n        }\n\n''' + text[end:]
write(path, text)

# Profiles use the global Quartz evdev stream when available, preserving the existing stored GLFW key codes.
path = "include/quartz/client/runtime/RuntimeBindingEngine.hpp"
text = read(path)
old_start = text.index('        void pollProfileHotkeys(GLFWwindow* window)\n')
old_end = text.index('        RuntimeObjectPointer& addPointer()\n', old_start)
new_method = r'''        void pollProfileHotkeys(GLFWwindow* window, const EvdevKeyboard& keyboard)
        {
            const bool useEvdev = keyboard.connected();
            if (!useEvdev && !window) return;
            const auto evdevKey = [](const int key) -> std::uint16_t
            {
                switch (key)
                {
                case GLFW_KEY_F1: return KEY_F1; case GLFW_KEY_F2: return KEY_F2; case GLFW_KEY_F3: return KEY_F3; case GLFW_KEY_F4: return KEY_F4; case GLFW_KEY_F5: return KEY_F5; case GLFW_KEY_F6: return KEY_F6;
                case GLFW_KEY_F7: return KEY_F7; case GLFW_KEY_F8: return KEY_F8; case GLFW_KEY_F9: return KEY_F9; case GLFW_KEY_F10: return KEY_F10; case GLFW_KEY_F11: return KEY_F11; case GLFW_KEY_F12: return KEY_F12;
                case GLFW_KEY_A: return KEY_A; case GLFW_KEY_B: return KEY_B; case GLFW_KEY_C: return KEY_C; case GLFW_KEY_D: return KEY_D; case GLFW_KEY_E: return KEY_E; case GLFW_KEY_F: return KEY_F; case GLFW_KEY_G: return KEY_G;
                case GLFW_KEY_H: return KEY_H; case GLFW_KEY_I: return KEY_I; case GLFW_KEY_J: return KEY_J; case GLFW_KEY_K: return KEY_K; case GLFW_KEY_L: return KEY_L; case GLFW_KEY_M: return KEY_M; case GLFW_KEY_N: return KEY_N;
                case GLFW_KEY_O: return KEY_O; case GLFW_KEY_P: return KEY_P; case GLFW_KEY_Q: return KEY_Q; case GLFW_KEY_R: return KEY_R; case GLFW_KEY_S: return KEY_S; case GLFW_KEY_T: return KEY_T; case GLFW_KEY_U: return KEY_U;
                case GLFW_KEY_V: return KEY_V; case GLFW_KEY_W: return KEY_W; case GLFW_KEY_X: return KEY_X; case GLFW_KEY_Y: return KEY_Y; case GLFW_KEY_Z: return KEY_Z;
                default: return 0;
                }
            };
            const bool ctrl = !useEvdev && (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS);
            const bool alt = !useEvdev && (glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS);
            const bool shift = !useEvdev && (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
            for (auto& profile : _profiles)
            {
                if (!profile.Enabled || profile.HotkeyKey <= 0) { profile.HotkeyDown = false; continue; }
                bool down = false;
                if (useEvdev)
                {
                    const std::uint16_t key = evdevKey(profile.HotkeyKey); down = key != 0 && keyboard.shortcutDown(key, profile.HotkeyCtrl, profile.HotkeyAlt, profile.HotkeyShift);
                }
                else
                {
                    const bool modifiers = (!profile.HotkeyCtrl || ctrl) && (!profile.HotkeyAlt || alt) && (!profile.HotkeyShift || shift); down = modifiers && glfwGetKey(window, profile.HotkeyKey) == GLFW_PRESS;
                }
                if (down && !profile.HotkeyDown) applyProfile(profile);
                profile.HotkeyDown = down;
            }
        }

'''
text = text[:old_start] + new_method + text[old_end:]
write(path, text)

path = "src/Application.cpp"
text = read(path)
text = replace_once(text, '        runtimeBindings.pollProfileHotkeys(window.handle());\n', '        runtimeBindings.pollProfileHotkeys(window.handle(), keyboardInput);\n', 'Application profile hotkeys')
write(path, text)

# Fix binding indicator centering.
path = "src/ui/pages/BindingsPage.cpp"
text = read(path)
text = replace_once(text, '''        void drawBindingStateOverlay(const ImVec2 legacyOrigin, const ImVec4 color)\n        {\n            constexpr float LegacySize = 11.0f;\n            const float size = ImGui::GetFrameHeight();\n            const float growth = std::max(size - LegacySize, 0.0f);\n            const ImVec2 min{legacyOrigin.x - growth * 0.5f, legacyOrigin.y - growth * 0.5f};\n            const ImVec2 max{min.x + size, min.y + size};\n            ImGui::GetWindowDrawList()->AddRectFilled(min, max, ImGui::GetColorU32(color), ImGui::GetStyle().FrameRounding);\n        }\n''', '''        void drawBindingStateOverlay(const ImVec2 legacyOrigin, const ImVec4 color)\n        {\n            constexpr float LegacySize = 11.0f;\n            const float lineHeight = ImGui::GetFrameHeight();\n            const float size = lineHeight;\n            const float growth = std::max(size - LegacySize, 0.0f);\n            const ImVec2 min{legacyOrigin.x - growth * 0.5f, legacyOrigin.y + (lineHeight - size) * 0.5f};\n            const ImVec2 max{min.x + size, min.y + size};\n            ImGui::GetWindowDrawList()->AddRectFilled(min, max, ImGui::GetColorU32(color), ImGui::GetStyle().FrameRounding);\n        }\n''', 'binding state centering')
write(path, text)

# Script editor: actual syntax-colored TextEditor + typings button and docs.
path = "src/ui/RuntimeUI.cpp"
text = read(path)
text = replace_once(text, '#include "quartz/client/runtime/QuickJS.hpp"\n', '#include "quartz/client/runtime/QuickJS.hpp"\n#include <memory>\n#include <unordered_map>\n', 'RuntimeUI scripting includes')
insert = r'''    namespace
    {
        const TextEditor::Language* runtimeQuickJSLanguage()
        {
            static const TextEditor::Language language = []
            {
                TextEditor::Language value; value.name = "JavaScript"; value.singleLineComment = "//"; value.commentStart = "/*"; value.commentEnd = "*/"; value.hasSingleQuotedStrings = true; value.hasDoubleQuotedStrings = true; value.otherStringStart = "`"; value.otherStringEnd = "`"; value.stringEscape = '\\';
                value.keywords = {"async","await","break","case","catch","class","const","continue","debugger","default","delete","do","else","export","extends","finally","for","from","function","get","if","import","in","instanceof","let","new","of","return","set","static","super","switch","this","throw","try","typeof","var","void","while","with","yield"};
                value.declarations = {"true","false","null","undefined"}; value.identifiers = {"q","Math","JSON","BigInt","Number","String","Boolean","Array","Object","Map","Set","Date","RegExp","NaN","Infinity"};
                value.isPunctuation = [](const ImWchar c) { return std::string_view("[]{}().,;:+-*/%<>=!&|^~?").find(static_cast<char>(c)) != std::string_view::npos; };
                value.getIdentifier = [](TextEditor::Iterator start, const TextEditor::Iterator end)
                {
                    if (start == end || !(TextEditor::CodePoint::isXidStart(*start) || *start == '_' || *start == '$')) return start; auto current = start; ++current; while (current != end && (TextEditor::CodePoint::isXidContinue(*current) || *current == '$')) ++current; return current;
                };
                value.getNumber = [](TextEditor::Iterator start, const TextEditor::Iterator end)
                {
                    if (start == end) return start; auto current = start; auto next = current; ++next;
                    const auto digit = [](const ImWchar c) { return c >= '0' && c <= '9'; }; const auto hex = [&](const ImWchar c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
                    if (*current == '.' && (next == end || !digit(*next))) return start;
                    if (*current == '0' && next != end && (*next == 'x' || *next == 'X' || *next == 'b' || *next == 'B' || *next == 'o' || *next == 'O'))
                    {
                        const ImWchar prefix = *next; current = next; ++current; while (current != end && (*current == '_' || (prefix == 'x' || prefix == 'X' ? hex(*current) : prefix == 'b' || prefix == 'B' ? (*current == '0' || *current == '1') : (*current >= '0' && *current <= '7')))) ++current; if (current != end && *current == 'n') ++current; return current;
                    }
                    bool dot = false; if (*current == '.') { dot = true; ++current; } while (current != end && (digit(*current) || *current == '_')) ++current; if (!dot && current != end && *current == '.') { ++current; while (current != end && (digit(*current) || *current == '_')) ++current; }
                    if (current != end && (*current == 'e' || *current == 'E')) { auto exponent = current; ++exponent; if (exponent != end && (*exponent == '+' || *exponent == '-')) ++exponent; bool any = false; while (exponent != end && (digit(*exponent) || *exponent == '_')) { any |= digit(*exponent); ++exponent; } if (any) current = exponent; }
                    if (current != end && *current == 'n') ++current; return current;
                };
                return value;
            }();
            return &language;
        }

        struct RuntimeScriptEditorState { TextEditor Editor; std::string Synced; bool Initialized = false; };
        RuntimeScriptEditorState& runtimeScriptEditor(const RuntimeBinding& binding)
        {
            static std::unordered_map<std::uint64_t, std::unique_ptr<RuntimeScriptEditorState>> editors; auto [it, inserted] = editors.try_emplace(binding.Id); if (inserted) it->second = std::make_unique<RuntimeScriptEditorState>(); auto& state = *it->second;
            if (!state.Initialized) { state.Editor.SetLanguage(runtimeQuickJSLanguage()); state.Editor.SetPalette(shaderEditorPalette()); state.Editor.SetTabSize(4); state.Editor.SetInsertSpacesOnTabs(true); state.Editor.SetAutoIndentEnabled(true); state.Editor.SetShowLineNumbersEnabled(true); state.Editor.SetShowMatchingBrackets(true); state.Editor.SetText(binding.Script); state.Synced = binding.Script; state.Initialized = true; }
            else if (state.Synced != binding.Script) { state.Editor.SetText(binding.Script); state.Synced = binding.Script; }
            return state;
        }
    }

'''
text = replace_once(text, 'namespace quartz::client\n{\n', 'namespace quartz::client\n{\n' + insert, 'RuntimeUI JS editor helpers')
start = text.index('        else if (binding.Source == RuntimeSourceKind::Script)\n        {')
end = text.index('        else if (binding.Source == RuntimeSourceKind::ProfileState)\n', start)
script_block = r'''        else if (binding.Source == RuntimeSourceKind::Script)
        {
            ImGui::SeparatorText("QuickJS script");
            ImGui::TextWrapped("The editor is a JavaScript function body. Return a number/bool/string/BigInt address, or { value, string, address }. Scripts are synchronous, compiled once, and run at this binding's update rate; low-level q.re calls share the same execution deadline.");
            auto& editor = runtimeScriptEditor(binding);
            if (editor.Editor.Render("##QuickJSScriptEditor", ImVec2(-1.0f, 240.0f)))
            {
                const std::string source = editor.Editor.GetText();
                if (source.size() < sizeof(binding.Script)) { std::memcpy(binding.Script, source.c_str(), source.size() + 1); editor.Synced = source; binding.NextUpdate = 0.0; changed = true; }
                else binding.Error = "QuickJS editor text exceeds the 8191-byte binding script limit";
            }
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::DragFloat("Execution timeout", &binding.ScriptTimeoutMs, 0.1f, 0.1f, 100.0f, "%.1f ms")) { binding.ScriptTimeoutMs = std::clamp(binding.ScriptTimeoutMs, 0.1f, 100.0f); changed = true; }
            if (ImGui::Button("Run now")) binding.NextUpdate = 0.0;
            ImGui::SameLine(); if (ImGui::Button("Reset script state")) { runtimeResetScriptBinding(binding.Id); binding.NextUpdate = 0.0; binding.ScriptLastLog.clear(); }
            ImGui::SameLine();
            static std::string typeStatus;
            if (ImGui::Button("Save .d.ts")) { std::string error; typeStatus = runtimeSaveQuickJSTypeDeclarations(error) ? "saved " + runtimeQuickJSTypeDeclarationsPath().string() : error; }
            ImGui::SameLine(); ImGui::TextDisabled("runs %llu | compiles %llu | timeouts %llu | last %.3f ms", static_cast<unsigned long long>(binding.ScriptRunCount), static_cast<unsigned long long>(binding.ScriptCompileCount), static_cast<unsigned long long>(binding.ScriptTimeoutCount), binding.ScriptLastMilliseconds);
            if (!typeStatus.empty()) ImGui::TextDisabled("%s", typeStatus.c_str());
            if (!binding.ScriptLastLog.empty()) ImGui::TextDisabled("log: %s", binding.ScriptLastLog.c_str());
            if (ImGui::TreeNode("QuickJS API"))
            {
                ImGui::BulletText("q.binding/raw/text/address(idOrName), q.bank(idOrName), q.control/triggered(idOrName)");
                ImGui::BulletText("q.loop(count, callback) - bounded helper; return false from callback to stop early");
                ImGui::BulletText("q.re.processes/modules/regions/processAlive - native process discovery");
                ImGui::BulletText("q.re.read/write + readBytes/writeBytes - typed/raw process memory access");
                ImGui::BulletText("q.re.signature - synchronous first-match libhat hex-pattern search under the script deadline");
                ImGui::BulletText("q.re.disassemble - architecture-aware x86/x64 Zydis disassembly");
                ImGui::BulletText("q.re.loop(start, count, stride, callback) - bounded address iteration with BigInt addresses");
                ImGui::BulletText("q.time / q.deltaTime / q.previous / q.previousRaw / q.state / q.log(...)");
                ImGui::TextDisabled("Save .d.ts writes quartz-bindings.d.ts beside the bindings config for external editor completion. RE writes are immediate process writes; there is intentionally no UI confirmation inside a script.");
                ImGui::TreePop();
            }
        }
'''
text = text[:start] + script_block + text[end:]
# Profile UI note without changing persisted GLFW key representation.
profile_start = text.index('    bool drawProfileHotkey(RuntimeBindingProfile& profile)\n')
profile_end = text.index('    void drawRuntimeProfiles', profile_start)
profile_text = text[profile_start:profile_end]
profile_text = replace_once(profile_text, '        return changed;\n', '        ImGui::TextDisabled("Uses the Quartz evdev stream globally when available, so the window does not need focus. GLFW is the fallback.");\n        return changed;\n', 'profile hotkey note')
text = text[:profile_start] + profile_text + text[profile_end:]
write(path, text)

# README feature bullets.
path = "README.md"
text = read(path)
text = replace_once(text, '- Runtime status/error feedback and rescan/rebind operations\n', '- Runtime status/error feedback and rescan/rebind operations\n- Embedded QuickJS scripted bindings with persistent per-binding state, execution deadlines and generated TypeScript declarations\n- Script-side `q.re` process/memory/signature/disassembly APIs and bounded loop helpers\n', 'README scripting bullets')
text = replace_once(text, '- Profiles and profile hotkeys\n', '- Profiles and global profile hotkeys through the Quartz evdev stream (GLFW fallback)\n', 'README profile hotkeys')
write(path, text)

print("QuickJS RE API migration applied")
