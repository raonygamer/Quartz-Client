#include "quartz/client/native/FunctionAnalysis.hpp"
#include "quartz/client/async/ThreadPool.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/native/NativeTypes.hpp"
#include "quartz/client/settings/RuntimeConfiguration.hpp"
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace quartz::client
{
    namespace
    {
        constexpr std::size_t AnalysisReadChunk = 32ULL * 1024ULL;
        constexpr std::uintptr_t PageMask = 4095;

        struct AnalysisEntry
        {
            pid_t Pid = 0;
            std::uintptr_t Start = 0;
            std::uintptr_t End = 0;
            std::atomic_bool Cancelled = false;
            std::mutex Mutex;
            bool Running = true;
            float Progress = 0.0f;
            std::uint64_t Revision = 1;
            std::vector<RuntimeFunctionCandidate> Candidates;
            std::string Status = "queued";
        };

        std::mutex EntriesMutex;
        std::vector<std::shared_ptr<AnalysisEntry>> Entries;
        std::map<std::pair<pid_t,std::uintptr_t>,RuntimeFunctionCandidate> ObservedTargets;

        bool contains(const AnalysisEntry& entry, const pid_t pid, const std::uintptr_t address) noexcept { return entry.Pid == pid && address >= entry.Start && address < entry.End; }

        bool likelyPaddingBefore(const std::span<const std::uint8_t> bytes, const std::size_t offset) noexcept
        {
            if (offset == 0) return true; const std::uint8_t value = bytes[offset - 1]; return value == 0x90 || value == 0xCC || value == 0x00;
        }

        bool addCandidate(std::vector<RuntimeFunctionCandidate>& candidates, const std::uintptr_t address, const RuntimeFunctionCandidateSource source, const float confidence)
        {
            const auto it = std::ranges::find(candidates,address,&RuntimeFunctionCandidate::Address); if (it == candidates.end()) { candidates.push_back({address,source,confidence}); return true; } if (confidence > it->Confidence) { it->Source = source; it->Confidence = confidence; return true; } return false;
        }

        std::vector<RuntimeFunctionCandidate> scanChunk(const std::span<const std::uint8_t> bytes, const std::uintptr_t base)
        {
            std::vector<RuntimeFunctionCandidate> result;
            for (std::size_t i=0;i<bytes.size();++i)
            {
                const auto match = [&](const std::initializer_list<std::uint8_t> pattern) { if (i + pattern.size() > bytes.size()) return false; std::size_t offset=0; for (const std::uint8_t value:pattern) if (bytes[i+offset++] != value) return false; return true; };
                float confidence=0.0f; if (match({0xF3,0x0F,0x1E,0xFA})||match({0xF3,0x0F,0x1E,0xFB})) confidence=0.99f; else if (match({0x55,0x48,0x89,0xE5})||match({0x55,0x48,0x8B,0xEC})||match({0x55,0x89,0xE5})) confidence=0.94f; else if (i+4<=bytes.size()&&match({0x48,0x83,0xEC})&&(likelyPaddingBefore(bytes,i)||((base+i)&0xF)==0)) confidence=0.76f; else if (i+5<=bytes.size()&&match({0x40,0x53,0x48,0x83,0xEC})&&(likelyPaddingBefore(bytes,i)||((base+i)&0xF)==0)) confidence=0.74f; else if (i+5<=bytes.size()&&match({0x48,0x89,0x5C,0x24})&&likelyPaddingBefore(bytes,i)) confidence=0.68f; if (confidence>0.0f) addCandidate(result,base+i,RuntimeFunctionCandidateSource::Prologue,confidence);
            }
            return result;
        }

        void publish(const std::shared_ptr<AnalysisEntry>& entry, std::vector<RuntimeFunctionCandidate>&& candidates, const float progress, const char* status)
        {
            std::lock_guard lock(entry->Mutex); for (const auto& candidate:candidates) addCandidate(entry->Candidates,candidate.Address,candidate.Source,candidate.Confidence); std::ranges::sort(entry->Candidates,{},&RuntimeFunctionCandidate::Address); entry->Progress=progress; entry->Status=status; ++entry->Revision;
        }

        std::pair<std::uintptr_t,std::uintptr_t> analysisRange(const pid_t pid, const std::uintptr_t around)
        {
            const auto modules=enumerateRuntimeModules(pid); const auto module=std::ranges::find_if(modules,[&](const RuntimeProcessModule& value){return around>=value.Base&&around<value.End;}); const std::size_t requested=std::max<std::size_t>(runtimeConfiguration().FunctionAnalysisWindowBytes,16ULL*1024ULL); const std::uintptr_t half=static_cast<std::uintptr_t>(requested/2); std::uintptr_t minimum=around>half?around-half:0, maximum=around+half; if (module!=modules.end()) { minimum=std::max(minimum,module->Base); maximum=std::min(maximum,module->End); } minimum&=~PageMask; maximum=(maximum+PageMask)&~PageMask; if (maximum<=minimum) maximum=minimum+4096; return {minimum,maximum};
        }

        bool cachedWindowCovers(const pid_t pid, const std::uintptr_t around, const bool includeEdge) noexcept
        {
            for (const auto& existing:Entries)
            {
                if (!contains(*existing,pid,around)) continue; if (existing->Running) return true; if (includeEdge) return true; const std::uintptr_t span=existing->End-existing->Start, margin=span/5; if (around-existing->Start>=margin&&existing->End-around>margin) return true;
            }
            return false;
        }
    }

    void requestRuntimeFunctionAnalysis(const pid_t pid, const std::uintptr_t around, const bool force)
    {
        if (pid<=0||around==0||!runtimeConfiguration().FunctionHeuristics) return;
        if (!force) { std::lock_guard lock(EntriesMutex); if (cachedWindowCovers(pid,around,false)) return; }
        const auto [start,end]=analysisRange(pid,around); std::shared_ptr<AnalysisEntry> entry;
        {
            std::lock_guard lock(EntriesMutex);
            if (!force&&cachedWindowCovers(pid,around,false)) return;
            if (force) for (const auto& existing:Entries) if (contains(*existing,pid,around)) existing->Cancelled=true;
            entry=std::make_shared<AnalysisEntry>(); entry->Pid=pid; entry->Start=start; entry->End=end; Entries.emplace_back(entry); if (Entries.size()>12) for (auto it=Entries.begin();it!=Entries.end()&&Entries.size()>12;) { if (!(*it)->Running) it=Entries.erase(it); else ++it; }
        }
        async::globalThreadPool().submit([entry](const std::stop_token stop)
        {
            const std::uintptr_t total=entry->End-entry->Start; std::uintptr_t done=0; publish(entry,{},0.0f,"analyzing function boundaries");
            for (std::uintptr_t address=entry->Start;address<entry->End&&!stop.stop_requested()&&!entry->Cancelled.load();)
            {
                const std::size_t size=static_cast<std::size_t>(std::min<std::uintptr_t>(AnalysisReadChunk,entry->End-address)); std::vector<std::uint8_t> bytes(size); std::string error; std::vector<RuntimeFunctionCandidate> candidates; if (readProcessMemoryBlock(entry->Pid,address,bytes,error)) candidates=scanChunk(bytes,address); address+=size; done+=size; publish(entry,std::move(candidates),total?static_cast<float>(static_cast<double>(done)/static_cast<double>(total)):1.0f,error.empty()?"analyzing function boundaries":"analysis skipped an unreadable chunk");
            }
            std::lock_guard lock(entry->Mutex); entry->Running=false; entry->Progress=1.0f; entry->Status=entry->Cancelled.load()||stop.stop_requested()?"function analysis cancelled":"function analysis ready"; ++entry->Revision;
        },async::TaskPriority::Background);
    }

    RuntimeFunctionAnalysisSnapshot runtimeFunctionAnalysisSnapshot(const pid_t pid, const std::uintptr_t around)
    {
        RuntimeFunctionAnalysisSnapshot snapshot; snapshot.Pid=pid; std::shared_ptr<AnalysisEntry> entry;
        {
            std::lock_guard lock(EntriesMutex); for (auto it=Entries.rbegin();it!=Entries.rend();++it) if (contains(**it,pid,around)) { entry=*it; break; } if (entry) { std::lock_guard entryLock(entry->Mutex); snapshot.RangeStart=entry->Start; snapshot.RangeEnd=entry->End; snapshot.Running=entry->Running; snapshot.Progress=entry->Progress; snapshot.Revision=entry->Revision; snapshot.Candidates=entry->Candidates; snapshot.Status=entry->Status; } for (const auto& [key,candidate]:ObservedTargets) { if (key.first!=pid) continue; if (entry&&(candidate.Address<entry->Start||candidate.Address>=entry->End)) continue; addCandidate(snapshot.Candidates,candidate.Address,candidate.Source,candidate.Confidence); }
        }
        std::ranges::sort(snapshot.Candidates,{},&RuntimeFunctionCandidate::Address); return snapshot;
    }

    void runtimeObserveFunctionTarget(const pid_t pid, const std::uintptr_t address, const RuntimeFunctionCandidateSource source)
    {
        if (pid<=0||address==0) return; const float confidence=source==RuntimeFunctionCandidateSource::CallTarget?1.0f:0.90f; std::lock_guard lock(EntriesMutex); auto [observed,inserted]=ObservedTargets.try_emplace({pid,address},RuntimeFunctionCandidate{address,source,confidence}); if (!inserted&&confidence>observed->second.Confidence) { observed->second.Source=source; observed->second.Confidence=confidence; }
        for (const auto& entry:Entries) { if (!contains(*entry,pid,address)) continue; std::lock_guard entryLock(entry->Mutex); if (addCandidate(entry->Candidates,address,source,confidence)) { std::ranges::sort(entry->Candidates,{},&RuntimeFunctionCandidate::Address); ++entry->Revision; } }
    }

    void invalidateRuntimeFunctionAnalysis(const pid_t pid)
    {
        std::lock_guard lock(EntriesMutex); for (const auto& entry:Entries) if (pid==0||entry->Pid==pid) entry->Cancelled=true; std::erase_if(Entries,[&](const auto& entry){return pid==0||entry->Pid==pid;}); if (pid==0) ObservedTargets.clear(); else std::erase_if(ObservedTargets,[&](const auto& item){return item.first.first==pid;});
    }

    const char* runtimeFunctionCandidateSourceName(const RuntimeFunctionCandidateSource source) noexcept
    {
        switch (source) { case RuntimeFunctionCandidateSource::CallTarget:return "call target"; case RuntimeFunctionCandidateSource::EndBranchTarget:return "branch target"; default:return "prologue heuristic"; }
    }
}
