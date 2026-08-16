#include "QuickJSInternal.hpp"
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
