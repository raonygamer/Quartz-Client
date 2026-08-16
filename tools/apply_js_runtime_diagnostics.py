from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def replace_once(path, old, new, label):
    p = ROOT / path
    text = p.read_text()
    if old not in text:
        raise RuntimeError(f"{label}: source fragment not found in {path}")
    p.write_text(text.replace(old, new, 1))

# Runtime-only script diagnostics and async signature jobs.
replace_once("include/quartz/client/runtime/RuntimeTypes.hpp", """    struct RuntimeScript
    {
""", """    enum class RuntimeScriptLogLevel : std::uint8_t { Debug, Info, Warning, Error };

    struct RuntimeScriptLogEntry
    {
        double Time = 0.0;
        RuntimeScriptLogLevel Level = RuntimeScriptLogLevel::Info;
        std::string Text;
    };

    struct RuntimeScriptSignatureScan
    {
        std::uint64_t Id = 0;
        pid_t Pid = 0;
        bool ExecutableOnly = true;
        std::string Pattern;
        std::shared_ptr<SignatureScanState> State;
        bool CompletionDelivered = false;
        bool Finished = false;
        bool Found = false;
        bool Cancelled = false;
        std::uintptr_t MatchAddress = 0;
        std::uint64_t ScannedBytes = 0;
        std::uint64_t TotalBytes = 0;
        float Progress = 0.0f;
        double AverageMiBs = 0.0;
        double DurationSeconds = 0.0;
        std::string Error;
        std::string Status = \"running\";
    };

    struct RuntimeScript
    {
""", "runtime diagnostic types")

replace_once("include/quartz/client/runtime/RuntimeTypes.hpp", """        std::string LastLog;
        std::string Status;
        std::vector<std::string> Dependencies;
""", """        std::string LastLog;
        std::string Status;
        std::vector<RuntimeScriptLogEntry> Console;
        std::string StateSnapshot = \"{}\";
        std::uint64_t NextSignatureScanId = 1;
        std::vector<RuntimeScriptSignatureScan> SignatureScans;
        std::vector<std::string> Dependencies;
""", "runtime diagnostic fields")

replace_once("src/runtime/QuickJSInternal.hpp", """    void runtimeInstallQuickJSLowLevelApi(JSContext* ctx, JSValueConst api);
    void runtimeInstallQuickJSGraphApi(JSContext* ctx, JSValueConst api);
""", """    void runtimeInstallQuickJSLowLevelApi(JSContext* ctx, JSValueConst api);
    void runtimeInstallQuickJSGraphApi(JSContext* ctx, JSValueConst api);
    void runtimeInstallQuickJSAsyncSignatureApi(JSContext* ctx, JSValueConst api);
    void runtimeRefreshQuickJSSignatureScans(RuntimeScript& script) noexcept;
    void runtimeCancelQuickJSSignatureScans(RuntimeScript& script) noexcept;
""", "async signature declarations")

(ROOT / "src/runtime/QuickJSAsyncSignature.cpp").write_text(r'''#include "QuickJSInternal.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/native/SignatureScanner.hpp"
#include "quartz/client/runtime/RuntimeTypes.hpp"
#include <algorithm>
#include <charconv>
#include <limits>

namespace quartz::client
{
    namespace
    {
        RuntimeScript* scriptFor(JSContext* ctx) noexcept
        {
            auto* context = static_cast<RuntimeQuickJSContext*>(JS_GetContextOpaque(ctx));
            return context ? context->Script : nullptr;
        }

        bool valueToUInt64(JSContext* ctx, JSValueConst value, std::uint64_t& output)
        {
            if (JS_IsBigInt(ctx, value))
            {
                const char* text = JS_ToCString(ctx, value); if (!text) return false;
                const char* end = text + std::strlen(text); const auto [ptr, ec] = std::from_chars(text, end, output, 10); JS_FreeCString(ctx, text);
                return ec == std::errc{} && ptr == end;
            }
            if (!JS_IsNumber(value)) return false;
            std::int64_t raw = 0; if (JS_ToInt64Ext(ctx, &raw, value) < 0 || raw < 0) return false; output = static_cast<std::uint64_t>(raw); return true;
        }

        bool valueToPid(JSContext* ctx, JSValueConst value, pid_t& output)
        {
            std::uint64_t raw = 0; if (!valueToUInt64(ctx, value, raw) || raw == 0 || raw > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) return false;
            output = static_cast<pid_t>(raw); return true;
        }

        void refresh(RuntimeScriptSignatureScan& job) noexcept
        {
            if (!job.State) { job.Finished = true; job.Status = "error"; job.Error = "signature scan state is unavailable"; return; }
            job.ScannedBytes = job.State->ScannedBytes.load(std::memory_order_relaxed);
            job.TotalBytes = job.State->TotalBytes;
            job.Progress = job.TotalBytes ? std::clamp(static_cast<float>(static_cast<double>(job.ScannedBytes) / static_cast<double>(job.TotalBytes)), 0.0f, 1.0f) : 0.0f;
            job.AverageMiBs = signatureScanAverageMiBs(job.State);
            SignatureScanResult result;
            if (!tryGetSignatureScanResult(job.State, result)) { job.Status = "running"; return; }
            job.Finished = true; job.Found = result.Found; job.Cancelled = result.Cancelled; job.MatchAddress = result.MatchAddress; job.ScannedBytes = result.ScannedBytes; job.DurationSeconds = result.DurationSeconds; job.Error = result.Error;
            job.Progress = job.TotalBytes ? std::clamp(static_cast<float>(static_cast<double>(job.ScannedBytes) / static_cast<double>(job.TotalBytes)), 0.0f, 1.0f) : 1.0f;
            if (job.Found) job.Status = "found";
            else if (job.Cancelled) job.Status = "cancelled";
            else if (!job.Error.empty()) job.Status = "error";
            else job.Status = "not-found";
        }

        RuntimeScriptSignatureScan* jobFor(RuntimeScript& script, const std::uint64_t id)
        {
            const auto it = std::ranges::find(script.SignatureScans, id, &RuntimeScriptSignatureScan::Id);
            return it == script.SignatureScans.end() ? nullptr : &*it;
        }

        JSValue statusObject(JSContext* ctx, RuntimeScriptSignatureScan& job)
        {
            refresh(job);
            JSValue object = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, object, "id", JS_NewBigUint64(ctx, job.Id));
            JS_SetPropertyStr(ctx, object, "pid", JS_NewInt32(ctx, job.Pid));
            JS_SetPropertyStr(ctx, object, "pattern", JS_NewString(ctx, job.Pattern.c_str()));
            JS_SetPropertyStr(ctx, object, "executableOnly", JS_NewBool(ctx, job.ExecutableOnly));
            JS_SetPropertyStr(ctx, object, "state", JS_NewString(ctx, job.Status.c_str()));
            JS_SetPropertyStr(ctx, object, "finished", JS_NewBool(ctx, job.Finished));
            JS_SetPropertyStr(ctx, object, "found", JS_NewBool(ctx, job.Found));
            JS_SetPropertyStr(ctx, object, "cancelled", JS_NewBool(ctx, job.Cancelled));
            JS_SetPropertyStr(ctx, object, "progress", JS_NewFloat64(ctx, job.Progress));
            JS_SetPropertyStr(ctx, object, "scannedBytes", JS_NewBigUint64(ctx, job.ScannedBytes));
            JS_SetPropertyStr(ctx, object, "totalBytes", JS_NewBigUint64(ctx, job.TotalBytes));
            JS_SetPropertyStr(ctx, object, "averageMiBs", JS_NewFloat64(ctx, job.AverageMiBs));
            JS_SetPropertyStr(ctx, object, "durationSeconds", JS_NewFloat64(ctx, job.DurationSeconds));
            if (job.Found) JS_SetPropertyStr(ctx, object, "address", JS_NewBigUint64(ctx, static_cast<std::uint64_t>(job.MatchAddress)));
            if (!job.Error.empty()) JS_SetPropertyStr(ctx, object, "error", JS_NewString(ctx, job.Error.c_str()));
            return object;
        }

        JSValue jsScan(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            RuntimeScript* script = scriptFor(ctx); pid_t pid = 0;
            if (!script || argc < 2 || !valueToPid(ctx, argv[0], pid)) return JS_ThrowTypeError(ctx, "q.signature.scan(pid, pattern, executableOnly?): invalid pid");
            const char* raw = JS_ToCString(ctx, argv[1]); if (!raw) return JS_EXCEPTION; std::string pattern(raw); JS_FreeCString(ctx, raw);
            std::vector<std::uint8_t> bytes, masks; std::string error;
            if (!parseRuntimeHexPattern(pattern, bytes, masks, error) || bytes.empty()) return JS_ThrowTypeError(ctx, "%s", error.empty() ? "empty signature" : error.c_str());
            bool executableOnly = true; if (argc > 2 && !JS_IsUndefined(argv[2])) { const int value = JS_ToBool(ctx, argv[2]); if (value < 0) return JS_EXCEPTION; executableOnly = value != 0; }
            auto regions = enumerateRuntimeRegions(pid);
            std::erase_if(regions, [executableOnly](const RuntimeProcessRegion& region) { return !region.Readable || (executableOnly && !region.Executable); });
            if (regions.empty()) return JS_ThrowInternalError(ctx, "no readable%s regions available for signature scan", executableOnly ? " executable" : "");
            std::erase_if(script->SignatureScans, [](RuntimeScriptSignatureScan& job) { refresh(job); return job.Finished && job.CompletionDelivered; });
            if (script->SignatureScans.size() >= 64) return JS_ThrowRangeError(ctx, "too many retained signature scans; consume/cancel old scans first");
            const std::uint64_t id = script->NextSignatureScanId++;
            RuntimeScriptSignatureScan job; job.Id = id; job.Pid = pid; job.ExecutableOnly = executableOnly; job.Pattern = std::move(pattern); job.State = startSignatureScan(pid, std::move(regions), std::move(bytes), std::move(masks), executableOnly, id); job.TotalBytes = job.State ? job.State->TotalBytes : 0;
            script->SignatureScans.push_back(std::move(job));
            return JS_NewBigUint64(ctx, id);
        }

        JSValue jsStatus(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            RuntimeScript* script = scriptFor(ctx); std::uint64_t id = 0; if (!script || argc < 1 || !valueToUInt64(ctx, argv[0], id)) return JS_UNDEFINED;
            auto* job = jobFor(*script, id); return job ? statusObject(ctx, *job) : JS_UNDEFINED;
        }

        JSValue jsCancel(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            RuntimeScript* script = scriptFor(ctx); std::uint64_t id = 0; if (!script || argc < 1 || !valueToUInt64(ctx, argv[0], id)) return JS_FALSE;
            auto* job = jobFor(*script, id); if (!job) return JS_FALSE; refresh(*job); if (job->Finished) return JS_FALSE; cancelSignatureScan(job->State); job->Status = "cancelling"; return JS_TRUE;
        }

        JSValue jsList(JSContext* ctx, JSValueConst, int, JSValueConst*)
        {
            RuntimeScript* script = scriptFor(ctx); JSValue array = JS_NewArray(ctx); if (!script) return array;
            for (std::uint32_t i = 0; i < script->SignatureScans.size(); ++i) JS_SetPropertyUint32(ctx, array, i, statusObject(ctx, script->SignatureScans[i]));
            return array;
        }
    }

    void runtimeInstallQuickJSAsyncSignatureApi(JSContext* ctx, JSValueConst api)
    {
        JSValue signature = JS_GetPropertyStr(ctx, api, "signature"); if (!JS_IsObject(signature)) { JS_FreeValue(ctx, signature); return; }
        JS_SetPropertyStr(ctx, signature, "scan", JS_NewCFunction(ctx, jsScan, "scan", 3));
        JS_SetPropertyStr(ctx, signature, "status", JS_NewCFunction(ctx, jsStatus, "status", 1));
        JS_SetPropertyStr(ctx, signature, "cancel", JS_NewCFunction(ctx, jsCancel, "cancel", 1));
        JS_SetPropertyStr(ctx, signature, "list", JS_NewCFunction(ctx, jsList, "list", 0));
        JS_FreeValue(ctx, signature);
    }

    void runtimeRefreshQuickJSSignatureScans(RuntimeScript& script) noexcept { for (auto& job : script.SignatureScans) refresh(job); }

    void runtimeCancelQuickJSSignatureScans(RuntimeScript& script) noexcept
    {
        for (auto& job : script.SignatureScans) { refresh(job); if (!job.Finished) cancelSignatureScan(job.State); }
        script.SignatureScans.clear();
    }
}
''')

# Workspace: safe files, console ring, state snapshot, async scan events/lifecycle.
replace_once("src/runtime/QuickJSWorkspace.cpp", """            JSValue Function = JS_UNDEFINED;
            JSValue Api = JS_UNDEFINED;
""", """            JSValue Function = JS_UNDEFINED;
            JSValue Api = JS_UNDEFINED;
            JSValue SnapshotFunction = JS_UNDEFINED;
""", "snapshot function field")

replace_once("src/runtime/QuickJSWorkspace.cpp", """            ~Instance()
            {
                if (!Context) return;
                for (auto& [_, value] : Modules) JS_FreeValue(Context, value);
                for (auto& subscription : Subscriptions) JS_FreeValue(Context, subscription.Callback);
                JS_FreeValue(Context, Function);
                JS_FreeValue(Context, Api);
                JS_FreeContext(Context);
            }
""", """            ~Instance()
            {
                if (Script) runtimeCancelQuickJSSignatureScans(*Script);
                if (!Context) return;
                for (auto& [_, value] : Modules) JS_FreeValue(Context, value);
                for (auto& subscription : Subscriptions) JS_FreeValue(Context, subscription.Callback);
                JS_FreeValue(Context, SnapshotFunction);
                JS_FreeValue(Context, Function);
                JS_FreeValue(Context, Api);
                JS_FreeContext(Context);
            }
""", "cancel async work on teardown")

old_log = '''        JSValue jsLog(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            auto* instance = static_cast<RuntimeQuickJSContext*>(JS_GetContextOpaque(ctx));
            if (!instance || !instance->Script) return JS_UNDEFINED;
            std::string text;
            for (int i = 0; i < argc; ++i)
            {
                const char* value = JS_ToCString(ctx, argv[i]);
                if (!value) return JS_EXCEPTION;
                if (!text.empty()) text.push_back(' ');
                if (text.size() < 2048) text.append(value, std::min<std::size_t>(std::strlen(value), 2048 - text.size()));
                JS_FreeCString(ctx, value);
            }
            instance->Script->LastLog = std::move(text);
            return JS_UNDEFINED;
        }

        bool loadFile(const std::filesystem::path& path, std::string& text)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file) return false;
            text.assign(std::istreambuf_iterator<char>(file), {});
            return static_cast<bool>(file) || file.eof();
        }
'''
new_log = '''        void appendLog(RuntimeScript& script, const double time, const RuntimeScriptLogLevel level, std::string text)
        {
            script.LastLog = text;
            script.Console.push_back({time, level, std::move(text)});
            constexpr std::size_t MaximumConsoleEntries = 512;
            if (script.Console.size() > MaximumConsoleEntries) script.Console.erase(script.Console.begin(), script.Console.begin() + static_cast<std::ptrdiff_t>(script.Console.size() - MaximumConsoleEntries));
        }

        std::string logText(JSContext* ctx, const int argc, JSValueConst* argv)
        {
            std::string text;
            for (int i = 0; i < argc; ++i)
            {
                const char* value = JS_ToCString(ctx, argv[i]); if (!value) return {};
                if (!text.empty()) text.push_back(' ');
                if (text.size() < 4096) text.append(value, std::min<std::size_t>(std::strlen(value), 4096 - text.size()));
                JS_FreeCString(ctx, value);
            }
            return text;
        }

        JSValue jsLog(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            auto* instance = static_cast<RuntimeQuickJSContext*>(JS_GetContextOpaque(ctx)); if (!instance || !instance->Script) return JS_UNDEFINED;
            appendLog(*instance->Script, instance->SignalContext ? instance->SignalContext->Time : 0.0, RuntimeScriptLogLevel::Info, logText(ctx, argc, argv)); return JS_UNDEFINED;
        }

        JSValue jsConsoleLog(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv, const int magic)
        {
            auto* instance = static_cast<RuntimeQuickJSContext*>(JS_GetContextOpaque(ctx)); if (!instance || !instance->Script) return JS_UNDEFINED;
            const auto level = magic == 0 ? RuntimeScriptLogLevel::Debug : magic == 2 ? RuntimeScriptLogLevel::Warning : magic == 3 ? RuntimeScriptLogLevel::Error : RuntimeScriptLogLevel::Info;
            appendLog(*instance->Script, instance->SignalContext ? instance->SignalContext->Time : 0.0, level, logText(ctx, argc, argv)); return JS_UNDEFINED;
        }

        bool loadFile(const std::filesystem::path& path, std::string& text, std::string& error) noexcept
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec))
            {
                error = ec ? "could not inspect " + path.string() + ": " + ec.message() : std::filesystem::is_directory(path, ec) ? "script path is a directory: " + path.string() : "script path is not a regular file: " + path.string();
                return false;
            }
            try
            {
                std::ifstream file(path, std::ios::binary); if (!file) { error = "could not open " + path.string(); return false; }
                text.assign(std::istreambuf_iterator<char>(file), {});
                if (!file && !file.eof()) { error = "could not read " + path.string(); return false; }
                error.clear(); return true;
            }
            catch (const std::exception& exception) { error = "could not read " + path.string() + ": " + exception.what(); return false; }
            catch (...) { error = "could not read " + path.string() + ": unknown filesystem error"; return false; }
        }
'''
replace_once("src/runtime/QuickJSWorkspace.cpp", old_log, new_log, "console and safe file loader")

replace_once("src/runtime/QuickJSWorkspace.cpp", '''            std::string source;
            if (!loadFile(path, source)) return JS_ThrowReferenceError(ctx, "could not import %s", key.c_str());
''', '''            std::string source, readError;
            if (!loadFile(path, source, readError)) return JS_ThrowReferenceError(ctx, "%s", readError.c_str());
''', "safe import load")

replace_once("src/runtime/QuickJSWorkspace.cpp", '''            JS_SetPropertyStr(instance->Context, instance->Api, "log", JS_NewCFunction(instance->Context, jsLog, "log", 1));
            JS_SetPropertyStr(instance->Context, instance->Api, "import", JS_NewCFunction(instance->Context, jsImport, "import", 1));
            runtimeInstallQuickJSLowLevelApi(instance->Context, instance->Api);
            runtimeInstallQuickJSGraphApi(instance->Context, instance->Api);
''', '''            JS_SetPropertyStr(instance->Context, instance->Api, "log", JS_NewCFunction(instance->Context, jsLog, "log", 1));
            JSValue console = JS_NewObject(instance->Context);
            JS_SetPropertyStr(instance->Context, console, "debug", JS_NewCFunctionMagic(instance->Context, jsConsoleLog, "debug", 1, JS_CFUNC_generic_magic, 0));
            JS_SetPropertyStr(instance->Context, console, "log", JS_NewCFunctionMagic(instance->Context, jsConsoleLog, "log", 1, JS_CFUNC_generic_magic, 1));
            JS_SetPropertyStr(instance->Context, console, "info", JS_NewCFunctionMagic(instance->Context, jsConsoleLog, "info", 1, JS_CFUNC_generic_magic, 1));
            JS_SetPropertyStr(instance->Context, console, "warn", JS_NewCFunctionMagic(instance->Context, jsConsoleLog, "warn", 1, JS_CFUNC_generic_magic, 2));
            JS_SetPropertyStr(instance->Context, console, "error", JS_NewCFunctionMagic(instance->Context, jsConsoleLog, "error", 1, JS_CFUNC_generic_magic, 3));
            JS_SetPropertyStr(instance->Context, instance->Api, "console", console);
            JS_SetPropertyStr(instance->Context, instance->Api, "import", JS_NewCFunction(instance->Context, jsImport, "import", 1));
            runtimeInstallQuickJSLowLevelApi(instance->Context, instance->Api);
            runtimeInstallQuickJSAsyncSignatureApi(instance->Context, instance->Api);
            runtimeInstallQuickJSGraphApi(instance->Context, instance->Api);
''', "console and async api install")

replace_once("src/runtime/QuickJSWorkspace.cpp", '''            JS_DefinePropertyValueStr(instance->Context, instance->Api, "storage", storage, JS_PROP_ENUMERABLE);
            auto* result = instance.get();
''', '''            JS_DefinePropertyValueStr(instance->Context, instance->Api, "storage", storage, JS_PROP_ENUMERABLE);
            static constexpr std::string_view SnapshotSource = "(value=>{const seen=new WeakSet();return JSON.stringify(value,(key,item)=>{if(typeof item==='bigint')return '0x'+item.toString(16)+'n';if(typeof item==='function')return '[Function]';if(typeof item==='object'&&item!==null){if(seen.has(item))return '[Circular]';seen.add(item);}return item;},2);})";
            instance->SnapshotFunction = JS_Eval(instance->Context, SnapshotSource.data(), SnapshotSource.size(), "quartz-state-snapshot.js", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
            if (JS_IsException(instance->SnapshotFunction)) { JSValue exception = JS_GetException(instance->Context); JS_FreeValue(instance->Context, exception); JS_FreeValue(instance->Context, instance->SnapshotFunction); instance->SnapshotFunction = JS_UNDEFINED; }
            auto* result = instance.get();
''', "state snapshot function")

# Add signature completion events before dispatchBuiltInEvents returns.
needle = '''            if (wantsEvent(instance, "breakpoint.hit"))
            {
                if (const auto hit = executionProbe().hit(); hit && hit->Time != instance.LastBreakpointHitTime)
                {
                    instance.LastBreakpointHitTime = hit->Time;
                    JSValue event = makeEvent(instance.Context, "breakpoint.hit", signal.Time);
                    JS_SetPropertyStr(instance.Context, event, "pid", JS_NewInt32(instance.Context, hit->Pid));
                    JS_SetPropertyStr(instance.Context, event, "tid", JS_NewInt32(instance.Context, hit->Tid));
                    JS_SetPropertyStr(instance.Context, event, "address", JS_NewBigUint64(instance.Context, static_cast<std::uint64_t>(hit->Address)));
                    if (hit->HasRegisters) JS_SetPropertyStr(instance.Context, event, "registers", registerObject(instance.Context, hit->Registers));
                    if (!dispatchEvent(instance, "breakpoint.hit", event, error)) { JS_FreeValue(instance.Context, event); return false; }
                    JS_FreeValue(instance.Context, event);
                }
            }
            return true;
'''
replacement = needle.replace('            return true;\n', '''            if (instance.Script)
            {
                runtimeRefreshQuickJSSignatureScans(*instance.Script);
                for (auto& scan : instance.Script->SignatureScans)
                {
                    if (!scan.Finished || scan.CompletionDelivered) continue;
                    const char* type = scan.Found ? "signature.found" : scan.Cancelled ? "signature.cancelled" : !scan.Error.empty() ? "signature.error" : "signature.not-found";
                    JSValue event = makeEvent(instance.Context, type, signal.Time);
                    JS_SetPropertyStr(instance.Context, event, "id", JS_NewBigUint64(instance.Context, scan.Id)); JS_SetPropertyStr(instance.Context, event, "pid", JS_NewInt32(instance.Context, scan.Pid)); JS_SetPropertyStr(instance.Context, event, "pattern", JS_NewString(instance.Context, scan.Pattern.c_str()));
                    JS_SetPropertyStr(instance.Context, event, "scannedBytes", JS_NewBigUint64(instance.Context, scan.ScannedBytes)); JS_SetPropertyStr(instance.Context, event, "totalBytes", JS_NewBigUint64(instance.Context, scan.TotalBytes)); JS_SetPropertyStr(instance.Context, event, "durationSeconds", JS_NewFloat64(instance.Context, scan.DurationSeconds)); JS_SetPropertyStr(instance.Context, event, "averageMiBs", JS_NewFloat64(instance.Context, scan.AverageMiBs));
                    if (scan.Found) JS_SetPropertyStr(instance.Context, event, "address", JS_NewBigUint64(instance.Context, static_cast<std::uint64_t>(scan.MatchAddress)));
                    if (!scan.Error.empty()) JS_SetPropertyStr(instance.Context, event, "error", JS_NewString(instance.Context, scan.Error.c_str()));
                    if (wantsEvent(instance, type) && !dispatchEvent(instance, type, event, error)) { JS_FreeValue(instance.Context, event); return false; }
                    if (wantsEvent(instance, "signature.finished") && !dispatchEvent(instance, "signature.finished", event, error)) { JS_FreeValue(instance.Context, event); return false; }
                    JS_FreeValue(instance.Context, event); scan.CompletionDelivered = true;
                }
            }
            return true;
''')
replace_once("src/runtime/QuickJSWorkspace.cpp", needle, replacement, "signature events")

replace_once("src/runtime/QuickJSWorkspace.cpp", '''        bool persistStorage(Instance& instance, RuntimeScript& script, JavaScriptRuntime& javascript, std::string& error)
''', '''        void snapshotState(Instance& instance, RuntimeScript& script) noexcept
        {
            if (!JS_IsFunction(instance.Context, instance.SnapshotFunction)) return;
            JSValue state = JS_GetPropertyStr(instance.Context, instance.Api, "state"); JSValue argument = JS_DupValue(instance.Context, state); JSValue result = JS_Call(instance.Context, instance.SnapshotFunction, JS_UNDEFINED, 1, &argument); JS_FreeValue(instance.Context, argument); JS_FreeValue(instance.Context, state);
            if (JS_IsException(result)) { JSValue exception = JS_GetException(instance.Context); JS_FreeValue(instance.Context, exception); JS_FreeValue(instance.Context, result); return; }
            if (const char* text = JS_ToCString(instance.Context, result)) { script.StateSnapshot = text; JS_FreeCString(instance.Context, text); }
            JS_FreeValue(instance.Context, result);
        }

        bool persistStorage(Instance& instance, RuntimeScript& script, JavaScriptRuntime& javascript, std::string& error)
''', "state snapshot helper")

replace_once("src/runtime/QuickJSWorkspace.cpp", '''                reuseExternal = existing != runtime.Instances.end() && JS_IsFunction(existing->second->Context, existing->second->Function);
                if (!reuseExternal && !loadFile(filename, source)) { script.Status = "could not read external script: " + filename.string(); return false; }
''', '''                reuseExternal = existing != runtime.Instances.end() && JS_IsFunction(existing->second->Context, existing->second->Function);
                if (!reuseExternal)
                {
                    std::string readError;
                    if (!loadFile(filename, source, readError)) { script.Status = readError; appendLog(script, signal.Time, RuntimeScriptLogLevel::Error, readError); return false; }
                }
''', "safe external load")

replace_once("src/runtime/QuickJSWorkspace.cpp", '''            if (!persistStorage(*instance, script, javascript, error)) { runtime.end(); script.Status = std::move(error); return false; }
''', '''            snapshotState(*instance, script);
            if (!persistStorage(*instance, script, javascript, error)) { runtime.end(); script.Status = std::move(error); return false; }
''', "snapshot state after run")

# Types + API docs.
replace_once("src/runtime/QuickJSApi.cpp", '''type QuartzEventName = "tick" | "shader.changed" | "key.down" | "key.up" | "key.changed" | "lock.changed" | "process.started" | "process.stopped" | "breakpoint.hit" | "script.loaded" | "script.reload" | string;
''', '''type QuartzEventName = "tick" | "shader.changed" | "key.down" | "key.up" | "key.changed" | "lock.changed" | "process.started" | "process.stopped" | "breakpoint.hit" | "signature.found" | "signature.not-found" | "signature.cancelled" | "signature.error" | "signature.finished" | "script.loaded" | "script.reload" | string;
''', "signature event typings")

replace_once("src/runtime/QuickJSApi.cpp", '''interface QuartzSignatureAPI { find(pid: number, pattern: string, executableOnly?: boolean): QuartzAddress | undefined; }
''', '''type QuartzSignatureScanHandle = bigint;
type QuartzSignatureScanState = "running" | "cancelling" | "found" | "not-found" | "cancelled" | "error";
interface QuartzSignatureScanStatus { id: QuartzSignatureScanHandle; pid: number; pattern: string; executableOnly: boolean; state: QuartzSignatureScanState; finished: boolean; found: boolean; cancelled: boolean; progress: number; scannedBytes: bigint; totalBytes: bigint; averageMiBs: number; durationSeconds: number; address?: QuartzAddress; error?: string; }
interface QuartzSignatureAPI {
    /** Synchronous compatibility scan. Prefer scan() for runtime scripts. */
    find(pid: number, pattern: string, executableOnly?: boolean): QuartzAddress | undefined;
    /** Starts a libhat scan on Quartz's worker pool and returns immediately. */
    scan(pid: number, pattern: string, executableOnly?: boolean): QuartzSignatureScanHandle;
    status(handle: QuartzSignatureScanHandle | number): QuartzSignatureScanStatus | undefined;
    cancel(handle: QuartzSignatureScanHandle | number): boolean;
    list(): QuartzSignatureScanStatus[];
}
''', "async signature typings")

replace_once("src/runtime/QuickJSApi.cpp", '''interface QuartzEventsAPI {
    subscribe(type: QuartzEventName | "*", callback: (event: any) => void): QuartzEventHandle;
    unsubscribe(handle: QuartzEventHandle | number): boolean;
    emit(type: string, data?: any): true;
}
''', '''interface QuartzEventsAPI {
    subscribe(type: QuartzEventName | "*", callback: (event: any) => void): QuartzEventHandle;
    unsubscribe(handle: QuartzEventHandle | number): boolean;
    emit(type: string, data?: any): true;
}
interface QuartzConsoleAPI { debug(...values: unknown[]): void; log(...values: unknown[]): void; info(...values: unknown[]): void; warn(...values: unknown[]): void; error(...values: unknown[]): void; }
''', "console typings")

replace_once("src/runtime/QuickJSApi.cpp", '''    readonly events: QuartzEventsAPI;
    readonly runtime: QuartzRuntimeOutputAPI;
''', '''    readonly events: QuartzEventsAPI;
    readonly console: QuartzConsoleAPI;
    readonly runtime: QuartzRuntimeOutputAPI;
''', "console api typing")

# UI: richer identifiers, safe inline -> external conversion, Editor/State/Console tabs.
replace_once("src/ui/pages/JavaScriptPage.cpp", '''#include <TextEditor.h>
#include <memory>
#include <unordered_map>
''', '''#include <TextEditor.h>
#include <cmath>
#include <fstream>
#include <memory>
#include <unordered_map>
''', "javascript ui includes")

replace_once("src/ui/pages/JavaScriptPage.cpp", '"subscribe","unsubscribe","emit","shader"', '"subscribe","unsubscribe","emit","scan","status","console","debug","info","warn","error","shader"', "javascript editor identifiers")

replace_once("src/ui/pages/JavaScriptPage.cpp", '''        struct KeyOption { const char* Name; int Key; };
''', '''        bool materializeExternal(RuntimeScript& script, std::string& error)
        {
            std::filesystem::path path = script.Path;
            std::error_code ec;
            if (path.empty() || std::filesystem::is_directory(path, ec)) path = runtimeQuickJSScriptDirectory() / ("script-" + std::to_string(script.Id) + ".js");
            if (path.is_relative()) path = runtimeQuickJSScriptDirectory() / path;
            std::filesystem::create_directories(path.parent_path(), ec); ec.clear();
            if (!std::filesystem::exists(path, ec))
            {
                try { std::ofstream file(path, std::ios::binary | std::ios::trunc); if (!file) { error = "could not create " + path.string(); return false; } file.write(script.Source.data(), static_cast<std::streamsize>(script.Source.size())); if (!file) { error = "could not write " + path.string(); return false; } }
                catch (const std::exception& exception) { error = "could not create external script: " + std::string(exception.what()); return false; }
            }
            if (!std::filesystem::is_regular_file(path, ec)) { error = "external script path is not a regular file: " + path.string(); return false; }
            script.Path = path.string(); error.clear(); return true;
        }

        void drawRuntimeIndicator(const RuntimeScript& script)
        {
            const float pulse = 0.62f + 0.38f * static_cast<float>((std::sin(ImGui::GetTime() * 3.6) + 1.0) * 0.5);
            const bool running = script.Enabled && script.Status.starts_with("running"); const bool failed = script.Enabled && !script.Status.empty() && !running && script.Status != "disabled";
            const ImVec4 color = running ? ImVec4(0.18f, 0.86f, 0.95f, pulse) : failed ? ImVec4(0.95f, 0.30f, 0.28f, 0.92f) : ImVec4(0.42f, 0.45f, 0.50f, 0.75f);
            const ImVec2 position = ImGui::GetCursorScreenPos(); const float side = ImGui::GetTextLineHeight() * 0.72f; ImGui::Dummy(ImVec2(side, side)); ImGui::GetWindowDrawList()->AddRectFilled(position, ImVec2(position.x + side, position.y + side), ImGui::ColorConvertFloat4ToU32(color), 2.0f);
            ImGui::SameLine(); ImGui::TextUnformatted(running ? "running" : failed ? "error / waiting" : script.Enabled ? "waiting" : "disabled");
        }

        const ImVec4& consoleColor(const RuntimeScriptLogLevel level)
        {
            static const ImVec4 Debug{0.55f,0.58f,0.64f,1.0f}, Info{0.82f,0.84f,0.88f,1.0f}, Warning{0.95f,0.70f,0.28f,1.0f}, Error{0.96f,0.35f,0.32f,1.0f};
            switch (level) { case RuntimeScriptLogLevel::Debug: return Debug; case RuntimeScriptLogLevel::Warning: return Warning; case RuntimeScriptLogLevel::Error: return Error; default: return Info; }
        }

        struct KeyOption { const char* Name; int Key; };
''', "javascript ui helpers")

replace_once("src/ui/pages/JavaScriptPage.cpp", '''            ImGui::BulletText("q.signature.find  |  q.disassembly.decode");
''', '''            ImGui::BulletText("q.signature.scan/status/cancel/list (async)  |  q.signature.find (sync)  |  q.disassembly.decode");
''', "runtime api async bullet")
replace_once("src/ui/pages/JavaScriptPage.cpp", '''            ImGui::BulletText("q.events.subscribe/unsubscribe/emit");
''', '''            ImGui::BulletText("q.events.subscribe/unsubscribe/emit  |  q.console.log/info/warn/error/debug");
''', "runtime api console bullet")
replace_once("src/ui/pages/JavaScriptPage.cpp", '''            ImGui::TextDisabled("Built-in events: tick, shader.changed, key.down, key.up, key.changed, lock.changed, process.started, process.stopped, breakpoint.hit, script.loaded, script.reload.");
''', '''            ImGui::TextDisabled("Built-in events include tick/input/process/shader/breakpoint plus signature.found, signature.not-found, signature.cancelled, signature.error and signature.finished.");
''', "runtime api event bullet")

replace_once("src/ui/pages/JavaScriptPage.cpp", '''                bool localChanged = false; localChanged |= ImGui::Checkbox("Enabled", &script.Enabled); ImGui::SameLine(); ImGui::SetNextItemWidth(240.0f); localChanged |= ImGui::InputText("Name", script.Name, sizeof(script.Name)); ImGui::SameLine(); localChanged |= ImGui::Checkbox("External", &script.External); ImGui::SameLine();
''', '''                bool localChanged = false; localChanged |= ImGui::Checkbox("Enabled", &script.Enabled); ImGui::SameLine(); ImGui::SetNextItemWidth(240.0f); localChanged |= ImGui::InputText("Name", script.Name, sizeof(script.Name)); ImGui::SameLine();
                const bool wasExternal = script.External;
                if (ImGui::Checkbox("External", &script.External))
                {
                    localChanged = true;
                    if (!wasExternal && script.External)
                    {
                        std::string conversionError;
                        if (!materializeExternal(script, conversionError)) { script.External = false; status = conversionError; }
                        else { runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); ++script.ReloadCount; status = "external script: " + script.Path; }
                    }
                    else if (wasExternal && !script.External) { runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); ++script.ReloadCount; }
                }
                ImGui::SameLine();
''', "safe external toggle")

old_body = '''                if (script.External)
                {
                    char path[1024]{}; std::snprintf(path, sizeof(path), "%s", script.Path.c_str()); ImGui::SetNextItemWidth(-1.0f); if (ImGui::InputText("Path", path, sizeof(path))) { script.Path = path; localChanged = true; }
                    ImGui::TextDisabled("Relative paths resolve under %s", runtimeQuickJSScriptDirectory().string().c_str());
                }
                else
                {
                    auto& editorState = editor(script); editorState.Editor.Render("##JavaScriptEditor", ImVec2(-1.0f, 360.0f)); const std::string edited = editorState.Editor.GetText();
                    if (edited != editorState.Synced) { script.Source = edited; editorState.Synced = script.Source; runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); localChanged = true; }
                }
                ImGui::Text("Runs %llu  compiles %llu  reloads %llu  timeouts %llu  last %.3f ms", static_cast<unsigned long long>(script.RunCount), static_cast<unsigned long long>(script.CompileCount), static_cast<unsigned long long>(script.ReloadCount), static_cast<unsigned long long>(script.TimeoutCount), script.LastMilliseconds);
                if (!script.Status.empty()) ImGui::TextWrapped("Status: %s", script.Status.c_str()); if (!script.LastLog.empty()) ImGui::TextWrapped("Log: %s", script.LastLog.c_str());
                if (!script.Dependencies.empty() && ImGui::TreeNode("Imported dependencies")) { for (const auto& dependency : script.Dependencies) ImGui::BulletText("%s", dependency.c_str()); ImGui::TreePop(); }
'''
new_body = '''                if (ImGui::BeginTabBar("##JavaScriptRuntimeTabs"))
                {
                    if (ImGui::BeginTabItem("Editor"))
                    {
                        if (script.External)
                        {
                            char path[1024]{}; std::snprintf(path, sizeof(path), "%s", script.Path.c_str()); ImGui::SetNextItemWidth(-1.0f); if (ImGui::InputText("Path", path, sizeof(path))) { script.Path = path; runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); localChanged = true; }
                            std::filesystem::path resolved = script.Path; if (resolved.is_relative()) resolved = runtimeQuickJSScriptDirectory() / resolved; std::error_code ec; const bool regular = std::filesystem::is_regular_file(resolved, ec);
                            if (!regular) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f,0.35f,0.32f,1.0f)); ImGui::TextWrapped("Invalid external script: %s", ec ? ec.message().c_str() : resolved.string().c_str()); ImGui::PopStyleColor(); }
                            ImGui::TextDisabled("Relative paths resolve under %s", runtimeQuickJSScriptDirectory().string().c_str());
                        }
                        else
                        {
                            auto& editorState = editor(script); editorState.Editor.Render("##JavaScriptEditor", ImVec2(-1.0f, 360.0f)); const std::string edited = editorState.Editor.GetText();
                            if (edited != editorState.Synced) { script.Source = edited; editorState.Synced = script.Source; runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); localChanged = true; }
                        }
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("State"))
                    {
                        drawRuntimeIndicator(script);
                        ImGui::Text("Runs %llu  compiles %llu  reloads %llu  timeouts %llu  last %.3f ms", static_cast<unsigned long long>(script.RunCount), static_cast<unsigned long long>(script.CompileCount), static_cast<unsigned long long>(script.ReloadCount), static_cast<unsigned long long>(script.TimeoutCount), script.LastMilliseconds);
                        if (!script.Status.empty()) ImGui::TextWrapped("Status: %s", script.Status.c_str());
                        if (!script.SignatureScans.empty())
                        {
                            ImGui::SeparatorText("Async signature scans");
                            for (const auto& scan : script.SignatureScans)
                            {
                                const bool running = !scan.Finished; const float pulse = running ? 0.55f + 0.45f * static_cast<float>((std::sin(ImGui::GetTime() * 4.0) + 1.0) * 0.5) : 1.0f;
                                ImGui::PushStyleColor(ImGuiCol_Text, running ? ImVec4(0.18f,0.86f,0.95f,pulse) : scan.Found ? ImVec4(0.45f,0.90f,0.62f,1.0f) : scan.Status == "error" ? ImVec4(0.96f,0.35f,0.32f,1.0f) : ImVec4(0.72f,0.74f,0.78f,1.0f));
                                ImGui::Text("#%llu  PID %d  %s", static_cast<unsigned long long>(scan.Id), scan.Pid, scan.Status.c_str()); ImGui::PopStyleColor();
                                if (running) ImGui::ProgressBar(scan.Progress, ImVec2(-1.0f, 0.0f));
                                ImGui::TextDisabled("%.1f MiB/s | %llu / %llu bytes%s", scan.AverageMiBs, static_cast<unsigned long long>(scan.ScannedBytes), static_cast<unsigned long long>(scan.TotalBytes), scan.Found ? (" | 0x" + [] (std::uintptr_t value) { char buffer[32]; std::snprintf(buffer, sizeof(buffer), "%llX", static_cast<unsigned long long>(value)); return std::string(buffer); }(scan.MatchAddress)).c_str() : "");
                                if (!scan.Error.empty()) ImGui::TextWrapped("%s", scan.Error.c_str());
                            }
                        }
                        ImGui::SeparatorText("q.state"); ImGui::BeginChild("##jsState", ImVec2(0.0f, 150.0f), true, ImGuiWindowFlags_HorizontalScrollbar); ImGui::TextUnformatted(script.StateSnapshot.empty() ? "{}" : script.StateSnapshot.c_str()); ImGui::EndChild();
                        ImGui::SeparatorText("q.storage"); ImGui::BeginChild("##jsStorage", ImVec2(0.0f, 120.0f), true, ImGuiWindowFlags_HorizontalScrollbar); ImGui::TextUnformatted(script.PersistentStateJson.empty() ? "{}" : script.PersistentStateJson.c_str()); ImGui::EndChild();
                        if (!script.Dependencies.empty() && ImGui::TreeNode("Imported dependencies")) { for (const auto& dependency : script.Dependencies) ImGui::BulletText("%s", dependency.c_str()); ImGui::TreePop(); }
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Console"))
                    {
                        static std::unordered_map<std::uint64_t, bool> autoScroll; bool& follow = autoScroll[script.Id]; if (!autoScroll.contains(script.Id)) follow = true;
                        if (ImGui::SmallButton("Clear")) script.Console.clear(); ImGui::SameLine(); ImGui::Checkbox("Auto-scroll", &follow); ImGui::SameLine(); ImGui::TextDisabled("%zu / 512", script.Console.size());
                        ImGui::BeginChild("##jsConsole", ImVec2(0.0f, 260.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
                        for (const auto& entry : script.Console) { ImGui::TextColored(consoleColor(entry.Level), "[%8.3f]", entry.Time); ImGui::SameLine(); ImGui::TextUnformatted(entry.Text.c_str()); }
                        if (follow && ImGui::GetScrollMaxY() > 0.0f) ImGui::SetScrollY(ImGui::GetScrollMaxY());
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
'''
replace_once("src/ui/pages/JavaScriptPage.cpp", old_body, new_body, "javascript editor state console tabs")

print("JavaScript async signature/diagnostics migration applied")
