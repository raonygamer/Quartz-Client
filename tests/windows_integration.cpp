#include <windows.h>
#include <mmsystem.h>
#include "quartz/client/Model.hpp"
#include "quartz/client/native/ExecutionProbe.hpp"
#include "quartz/client/native/MemoryWatch.hpp"
#include "quartz/client/native/MemoryScanner.hpp"
#include "quartz/client/native/SignatureScanner.hpp"
#include "quartz/client/platform/WindowsMedia.hpp"
#include <quickjs.h>
#include <iostream>

using namespace quartz::client;
namespace
{
    void require(bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
        std::cout << "PASS: " << message << std::endl;
    }
    template<class Predicate> bool until(Predicate predicate, double seconds = 10)
    {
        const auto deadline = runtimeSteadySeconds() + seconds;
        while (runtimeSteadySeconds() < deadline) { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        return predicate();
    }
    struct Fixture
    {
        win::Handle Process;
        pid_t Pid = 0;
        std::uintptr_t Data = 0, Code = 0;
        std::filesystem::path File;
        explicit Fixture(const std::filesystem::path& executable)
        {
            File = std::filesystem::temp_directory_path() / ("quartz-fixture-" + std::to_string(GetCurrentProcessId()) + ".txt");
            std::error_code ec; std::filesystem::remove(File,ec);
            auto command = L"\"" + executable.wstring() + L"\" \"" + File.wstring() + L"\"";
            STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION info{};
            if (!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&info)) throw std::runtime_error(win::error("launching fixture"));
            Process.reset(info.hProcess); CloseHandle(info.hThread);
            if (!until([&] { std::ifstream file(File); return static_cast<bool>(file >> Pid >> Data >> Code); })) throw std::runtime_error("fixture handshake timed out");
        }
        void set(std::size_t offset, std::uint32_t value)
        {
            std::string error;
            if (!runtimeWriteProcessMemory(Pid,Data+offset,{reinterpret_cast<const std::uint8_t*>(&value),sizeof(value)},error)) throw std::runtime_error(error);
        }
        ~Fixture()
        {
            if (Process)
            {
                if (Data) { std::string error; const std::uint32_t quit = 1; runtimeWriteProcessMemory(Pid,Data+8,{reinterpret_cast<const std::uint8_t*>(&quit),4},error); }
                if (WaitForSingleObject(Process.get(),2000) == WAIT_TIMEOUT) TerminateProcess(Process.get(),1);
            }
            std::error_code ec; std::filesystem::remove(File,ec);
        }
    };

    void testProcess(const std::filesystem::path& executable, RuntimeX86Mode mode)
    {
        Fixture target(executable);
        std::string error;
        require(runtimeProcessIsAlive(target.Pid),"child process alive");
        require(runtimeProcessX86Mode(target.Pid) == mode,"target architecture detection");
        const auto processes = enumerateRuntimeProcesses();
        const auto found = std::ranges::find(processes,target.Pid,&RuntimeProcessInfo::Pid);
        require(found != processes.end() && !found->Exe.empty() && found->CommandLine.find("quartz-fixture-") != std::string::npos,"process metadata and command line");
        const auto modules = enumerateRuntimeModules(target.Pid);
        require(std::ranges::any_of(modules,[&](const auto& m) { return m.contains(target.Code); }),"module discovery");
        const auto regions = enumerateRuntimeRegions(target.Pid);
        require(std::ranges::any_of(regions,[&](const auto& r) { return r.Base <= target.Data && target.Data < r.End && r.Readable && r.Writable; }),"memory region permissions");
        std::uint32_t value = 0;
        require(readProcessMemoryValue(target.Pid,target.Data,value,error) && value == 0x13579bdf,"cross-process memory read");
        RuntimeBinding pointerBinding;
        std::snprintf(pointerBinding.Address,sizeof(pointerBinding.Address),"%s -> 12",runtimeHexAddress(target.Data+32).c_str());
        const auto pointerAddress = resolveRuntimeAddress(pointerBinding,target.Pid,error,std::nullopt);
        require(pointerAddress && *pointerAddress == target.Data+12,"pointer chain uses target architecture width");
        pointerBinding.SignatureResolve = SignatureResultMode::PointerAtOffset;
        const auto signaturePointer = resolveRuntimeSignatureMatch(pointerBinding,target.Pid,target.Data+32,error);
        require(signaturePointer && *signaturePointer == target.Data,"signature pointer uses target architecture width");
        target.set(0,0x2468ace0);
        require(readProcessMemoryValue(target.Pid,target.Data,value,error) && value == 0x2468ace0,"cross-process memory write");
        std::vector<std::uint8_t> pattern,masks;
        require(parseRuntimeHexPattern("A0 A1 A? ?? A4 A5 A6 A7 A8 A9 AA AB AC AD AE AF",pattern,masks,error),"signature wildcard parsing");
        auto signature = startSignatureScan(target.Pid,{{target.Data,target.Data+4096,true,true,false,{}}},pattern,masks,false,1);
        SignatureScanResult result;
        require(until([&] { return tryGetSignatureScanResult(signature,result); }) && result.Found && result.MatchAddress == target.Data+12,"asynchronous signature scan");
        MemoryScanner scanner;
        MemoryScanRequest request; request.Pid=target.Pid; request.Type=MemoryScanValueType::U32; request.ValueA="0x2468ACE0";
        require(scanner.newScan(request,error),"memory scanner starts: " + error);
        require(until([&] { scanner.poll(); return !scanner.running(); },30),"initial memory scan completes");
        auto rows = scanner.results(4096);
        require(std::ranges::any_of(rows,[&](const auto& row) { return row.Address == target.Data; }),"memory scanner finds fixture value");
        target.set(0,0x2468ace1); request.Comparison=MemoryScanComparison::Changed;
        require(scanner.nextScan(request,error) && until([&] { scanner.poll(); return !scanner.running(); },30),"changed-value next scan completes");
        rows = scanner.results(4096);
        require(std::ranges::any_of(rows,[&](const auto& row) { return row.Address == target.Data; }),"next scan preserves changed value");
        ExecutionProbe probe;
        require(probe.start(target.Pid,target.Code,error),"execution probe starts");
        require(until([&] { return probe.status().find("armed") != std::string::npos || !probe.running(); }),"execution probe finishes arming");
        require(probe.running(),"execution probe armed: " + probe.status());
        target.set(4,1);
        require(until([&] { return !probe.running(); }),"execution probe completes");
        const auto hit = probe.hit();
        require(hit && hit->HasRegisters && hit->Registers.rip == target.Code,"execution probe captures instruction and registers: " + probe.status());
        probe.stop(); target.set(4,0);
        MemoryWatch watch;
        require(watch.start(target.Pid,target.Data,4,MemoryWatchAccess::Write,3,error),"hardware write monitor starts");
        require(until([&] { return watch.status().find("armed") != std::string::npos || !watch.running(); }) && watch.running(),"hardware write monitor armed: " + watch.status());
        target.set(4,1);
        require(until([&] { return !watch.running(); }),"hardware write monitor reaches hit limit");
        const auto hits = watch.hits();
        std::uint64_t count=0; for (const auto& entry:hits) count+=entry.Count;
        require(count==3 && hits.front().HasRegisters && !hits.front().Instruction.empty(),"hardware access hits group with registers and disassembly: " + watch.status());
        watch.stop(); target.set(4,0);
        require(probe.start(target.Pid,target.Code,error),"probe can reattach after watch cleanup");
        require(until([&] { return probe.status().find("armed") != std::string::npos || !probe.running(); }) && probe.running(),"cancellation test armed");
        probe.stop();
        target.set(4,1);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        require(runtimeProcessIsAlive(target.Pid),"target survives breakpoint cancellation and detachment");
        target.set(4,0);
    }

    void testAssembler()
    {
        for (const auto mode : {RuntimeX86Mode::X86,RuntimeX86Mode::X64})
        {
            const std::uintptr_t base = mode==RuntimeX86Mode::X64 ? 0x7ff012340000ULL : 0x12340000;
            std::ostringstream source; source << "nop\njmp 0x" << std::hex << (base+0x30);
            std::vector<std::uint8_t> bytes; std::string error;
            const bool assembled = runtimeAssembleInstructionText(mode,base,source.str(),bytes,error);
            require(assembled,"assemble relocated patch: " + error);
            require(bytes.size()==6 && bytes.front()==0x90,"assembler trims COFF section padding");
            RuntimeDecodedInstruction decoded;
            require(runtimeDecodeProcessInstruction(mode,std::span<const std::uint8_t>(bytes).subspan(1),base+1,decoded) && decoded.Target==base+0x30,"assembled branch resolves to requested address");
        }
    }

    void testProtection()
    {
        auto* page=static_cast<std::uint8_t*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        require(page!=nullptr,"allocate patch test page");
        DWORD old; VirtualProtect(page,4096,PAGE_EXECUTE_READ,&old);
        const std::array<std::uint8_t,2> patch{0x90,0xC3}; std::string error;
        const bool wrote=runtimeWriteProcessMemory(GetCurrentProcessId(),reinterpret_cast<std::uintptr_t>(page),patch,error);
        MEMORY_BASIC_INFORMATION info{}; VirtualQuery(page,&info,sizeof(info));
        const bool restored=info.Protect==PAGE_EXECUTE_READ && page[0]==0x90 && page[1]==0xC3;
        VirtualFree(page,0,MEM_RELEASE);
        require(wrote && restored,"executable patch restores page protection: " + error);
    }

    void testScripts()
    {
        JSRuntime* runtime=JS_NewRuntime(); JSContext* context=JS_NewContext(runtime);
        const char* source="const xs = [1,2,3]; xs.map(x => x * 2).reduce((a,b) => a+b, 0)";
        JSValue result=JS_Eval(context,source,std::strlen(source),"windows-test.js",JS_EVAL_TYPE_GLOBAL);
        int value=0; const bool success=!JS_IsException(result) && JS_ToInt32(context,&value,result)==0 && value==12;
        JS_FreeValue(context,result); JS_FreeContext(context); JS_FreeRuntime(runtime);
        require(success,"embedded QuickJS executes modern JavaScript");
    }

    void testDesktop(bool graphics = true, bool deviceCommunication = true)
    {
        if (graphics)
        {
        require(glfwInit(),"GLFW initializes");
        glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE); glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
        auto* window=glfwCreateWindow(64,64,"Quartz integration test",nullptr,nullptr);
        require(window!=nullptr,"OpenGL context available"); glfwMakeContextCurrent(window);
        require(gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress)),"OpenGL entry points load");
        {
            ShaderFramebuffer shader; VisualizerSettings settings; std::array<float,Columns> bands{}; std::array<Color32,MatrixSize> frame{};
            const auto presets=buildShaderPresets();
            bool allCompiled = true;
            for (const auto& preset:presets)
            {
                const bool compiled = shader.compile(DefaultVertexShaderSource,preset.FragmentSource);
                if (!compiled) { allCompiled = false; std::cerr << "SHADER FAILURE: " << preset.Name << "\n" << shader.status() << std::endl; continue; }
                require(compiled,"shader compiles: " + preset.Name + " " + shader.status());
                require(shader.render(1.0,bands,settings,std::nullopt,0,{},frame),"shader renders: " + preset.Name);
            }
            require(allCompiled, "all built-in shaders compile and render");
        }
        glfwDestroyWindow(window); glfwTerminate();
        }
        const auto sources=enumerateAudioSources();
        require(sources.size()>2,"WASAPI enumerates real endpoints");
        AudioSpectrum audio;
        const bool started=audio.start("default");
        require(started,"WASAPI default output loopback starts: " + audio.error());
        // A quiet half-second test tone validates real samples and the FFT path.
        std::vector<std::int16_t> tone(48000);
        for (std::size_t i=0;i<tone.size()/2;++i) tone[2*i]=tone[2*i+1]=static_cast<std::int16_t>(655.0*std::sin(2.0*Pi*440.0*double(i)/48000.0));
        WAVEFORMATEX waveFormat{WAVE_FORMAT_PCM,2,48000,192000,4,16,0};
        HWAVEOUT wave = nullptr;
        require(waveOutOpen(&wave,WAVE_MAPPER,&waveFormat,0,0,CALLBACK_NULL)==MMSYSERR_NOERROR,"test playback opens");
        WAVEHDR waveHeader{}; waveHeader.lpData=reinterpret_cast<char*>(tone.data()); waveHeader.dwBufferLength=static_cast<DWORD>(tone.size()*sizeof(std::int16_t));
        waveOutPrepareHeader(wave,&waveHeader,sizeof(waveHeader)); waveOutWrite(wave,&waveHeader,sizeof(waveHeader));
        const bool signal = until([&] { return audio.levelSnapshot().Peak > 0.0001f; },2);
        std::array<float,64> bands{}; audio.getBands(bands,20,20000,-90,0);
        waveOutReset(wave); waveOutUnprepareHeader(wave,&waveHeader,sizeof(waveHeader)); waveOutClose(wave);
        require(signal && std::ranges::any_of(bands,[](float value) { return value>0.05f; }),"WASAPI loopback captures real samples and produces FFT bands");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(audio.isRunning(),"WASAPI capture remains running"); audio.stop();
        std::atomic_bool running{true};
        const auto media=windowsMediaSnapshot(running);
        std::cout << "MEDIA: " << media.Title << " | " << media.Artist << " | artwork bytes=" << media.Artwork.size() << std::endl;
        require(media.Status.find("(requires") == std::string::npos && media.Status.find("initialization (") == std::string::npos && media.Status.find("sessions (") == std::string::npos,"Windows media API responds: " + media.Status);
        EvdevKeyboard keyboard; keyboard.start(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(keyboard.status().find("RegisterRawInputDevices") == std::string::npos,"Windows Raw Input initializes: " + keyboard.status()); keyboard.stop();
        libusb_context* usb=nullptr;
        require(libusb_init(&usb)==0,"libusb initializes Windows backend");
        libusb_device** devices=nullptr; const auto count=libusb_get_device_list(usb,&devices);
        require(count>=0,"USB enumeration succeeds"); libusb_free_device_list(devices,1); libusb_exit(usb);
        if (!deviceCommunication) return;
        RawUSB quartz;
        require(quartz.initialize(), "Quartz USB transport initializes");
        const bool connected = quartz.connect();
        require(connected, std::string("Quartz RPC interface opens: ") + libusb_error_name(quartz.lastError()));
        std::atomic_bool pong{false}, performance{false};
        quartz.setPacketHandler([&](const PacketHeader& packet) { if (packet.Type==PacketType::Pong) pong=true; if (packet.Type==PacketType::PerformanceResponse) performance=true; });
        require(quartz.send(makePacket(PacketType::Ping)), "USB ping request sent");
        require(until([&] { return pong.load(); },3), "Quartz firmware pong received");
        require(quartz.send(makePacket(PacketType::PerformanceRequest)), "USB performance request sent");
        require(until([&] { return performance.load(); },3), "Quartz performance telemetry received");
        quartz.disconnect();
    }
}
int main(int argc,char** argv)
{
    try
    {
        if (argc > 1 && std::string_view(argv[1]) == "--desktop-only") { testDesktop(); return 0; }
        if (argc > 1 && std::string_view(argv[1]) == "--devices-only") { testDesktop(false); return 0; }
        if (argc > 1 && std::string_view(argv[1]) == "--host-only") { testDesktop(true,false); return 0; }
        testScripts(); testProtection(); testAssembler();
        if (argc>1) testProcess(std::filesystem::absolute(argv[1]),RuntimeX86Mode::X64);
        if (argc>2 && std::string_view(argv[2])!="--desktop") testProcess(std::filesystem::absolute(argv[2]),RuntimeX86Mode::X86);
        for (int i=1;i<argc;++i) if (std::string_view(argv[i])=="--desktop") testDesktop();
        std::cout << "All requested Windows integration checks passed.\n"; return 0;
    }
    catch(const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
