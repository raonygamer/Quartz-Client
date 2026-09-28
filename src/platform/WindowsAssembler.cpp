#ifdef _WIN32
#include "quartz/client/Model.hpp"
#include "quartz/client/native/NativeDisassembly.hpp"

namespace quartz::client
{
    namespace
    {
        std::wstring quoteArgument(std::wstring_view argument)
        {
            std::wstring result = L"\"";
            std::size_t slashes = 0;
            for (const wchar_t c : argument)
            {
                if (c == L'\\') { ++slashes; continue; }
                if (c == L'\"') result.append(slashes * 2 + 1,L'\\');
                else result.append(slashes,L'\\');
                slashes = 0; result += c;
            }
            result.append(slashes * 2,L'\\'); result += L'\"'; return result;
        }

        std::wstring findTool(std::string_view name)
        {
            std::wstring module(32768,L'\0');
            module.resize(GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size())));
            const auto file = win::wide(name) + L".exe";
            const auto bundled = std::filesystem::path(module).parent_path() / "tools" / file;
            if (std::filesystem::is_regular_file(bundled)) return bundled.wstring();
            std::wstring found(32768,L'\0');
            const DWORD length = SearchPathW(nullptr,file.c_str(),nullptr,static_cast<DWORD>(found.size()),found.data(),nullptr);
            if (!length || length >= found.size()) return {};
            found.resize(length); return found;
        }

        bool runTool(std::wstring_view executable, const std::vector<std::wstring>& arguments, std::string& output)
        {
            output.clear();
            if (executable.empty()) { output = "GNU binutils are required: place as.exe, ld.exe, nm.exe and objcopy.exe in tools beside quartz.exe, or on PATH"; return false; }
            SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
            HANDLE readHandle = nullptr,writeHandle = nullptr;
            if (!CreatePipe(&readHandle,&writeHandle,&security,0)) { output = win::error("CreatePipe"); return false; }
            win::Handle reader(readHandle),writer(writeHandle);
            SetHandleInformation(reader.get(),HANDLE_FLAG_INHERIT,0);
            win::Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ | FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr));
            STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = input.get(); startup.hStdOutput = startup.hStdError = writer.get();
            std::wstring command = quoteArgument(executable);
            for (const auto& argument : arguments) command += L" " + quoteArgument(argument);
            PROCESS_INFORMATION processInfo{};
            if (!CreateProcessW(std::wstring(executable).c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&processInfo))
            { output = win::error("starting assembler tool"); return false; }
            win::Handle process(processInfo.hProcess),thread(processInfo.hThread); writer.reset();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            for (;;)
            {
                DWORD available = 0;
                if (PeekNamedPipe(reader.get(),nullptr,0,nullptr,&available,nullptr) && available)
                {
                    char buffer[4096]; DWORD read = 0;
                    if (ReadFile(reader.get(),buffer,std::min<DWORD>(available,sizeof(buffer)),&read,nullptr) && output.size() < 64*1024) output.append(buffer,std::min<std::size_t>(read,64*1024-output.size()));
                    continue;
                }
                if (WaitForSingleObject(process.get(),5) == WAIT_OBJECT_0)
                {
                    // A short-lived tool can write and exit between PeekNamedPipe
                    // and the wait. Drain those final bytes before returning.
                    if (PeekNamedPipe(reader.get(),nullptr,0,nullptr,&available,nullptr) && available) continue;
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                { TerminateProcess(process.get(),ERROR_TIMEOUT); WaitForSingleObject(process.get(),1000); output = "assembler tool timed out"; return false; }
            }
            DWORD code = 1; GetExitCodeProcess(process.get(),&code); return code == 0;
        }
    }

    bool commandExists(std::string_view name) { return !findTool(name).empty(); }

    bool runtimeAssembleInstructionText(RuntimeX86Mode mode,std::uintptr_t address,std::string_view source,std::vector<std::uint8_t>& bytes,std::string& error)
    {
        bytes.clear(); error.clear();
        if (source.empty()) { error = "assembly source is empty"; return false; }
        if (mode == RuntimeX86Mode::X86 && address > UINT32_MAX) { error = "address exceeds 32-bit target range"; return false; }
        const auto assembler = findTool("as"),linker = findTool("ld"),symbols = findTool("nm"),extract = findTool("objcopy");
        if (assembler.empty() || linker.empty() || symbols.empty() || extract.empty())
        { error = "GNU binutils (as, ld, nm, objcopy) are required in tools beside quartz.exe or on PATH"; return false; }
        std::error_code ec;
        const auto directory = std::filesystem::temp_directory_path() / ("quartz-asm-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(directory,ec)) { error = "could not create assembler temporary directory: " + ec.message(); return false; }
        struct Cleanup { std::filesystem::path Directory; ~Cleanup() { std::error_code error; std::filesystem::remove_all(Directory,error); } } cleanup{directory};
        const auto assembly = directory / "patch.s", object = directory / "patch.o", script = directory / "patch.ld", executable = directory / "patch.exe", binary = directory / "patch.bin";
        std::string assignments,processed;
        std::istringstream lines{std::string(source)}; std::string line; unsigned target = 0;
        // COFF cannot encode a high absolute address directly as a signed rel32
        // addend. Give the linker a symbol so it computes target - instruction.
        const std::regex branch(R"(^([ \t]*(?:j[a-z]+|call|loop[a-z]*)[ \t]+)(0x[0-9a-fA-F]+|[0-9]+)([ \t]*(?:[#;].*)?)$)",std::regex::icase);
        while (std::getline(lines,line))
        {
            std::smatch match;
            if (std::regex_match(line,match,branch))
            {
                const auto name = "quartz_absolute_" + std::to_string(target++);
                assignments += name + " = " + match[2].str() + ";\n";
                line = match[1].str() + name + match[3].str();
            }
            processed += line + '\n';
        }
        { std::ofstream file(assembly); file << ".intel_syntax noprefix\n.text\n.global quartz_patch_start\nquartz_patch_start:\n" << processed << "\nquartz_patch_end:\n"; if (!file) { error = "could not write assembler source"; return false; } }
        if (!runTool(assembler,{mode == RuntimeX86Mode::X86 ? L"--32" : L"--64",L"-o",object.wstring(),assembly.wstring()},error)) return false;
        std::string listing;
        if (!runTool(symbols,{L"--format=posix",object.wstring()},listing)) { error = listing; return false; }
        std::smatch end;
        if (!std::regex_search(listing,end,std::regex(R"(quartz_patch_end [tT] ([0-9a-fA-F]+))"))) { error = "assembly must end in the .text section"; return false; }
        std::size_t size = 0;
        const auto sizeText = end[1].str(); const auto parsed = std::from_chars(sizeText.data(),sizeText.data()+sizeText.size(),size,16);
        if (parsed.ec != std::errc{} || !size || size > 4096) { error = "assembled patch must contain 1 to 4096 bytes"; return false; }
        std::ostringstream base; base << "0x" << std::hex << address;
        std::ostringstream imageBase; imageBase << "0x" << std::hex << (address & ~std::uintptr_t{0xFFFF});
        { std::ofstream file(script); file << assignments << "SECTIONS { . = " << base.str() << "; .text : { *(.text) } /DISCARD/ : { *(*) } }\n"; if (!file) { error = "could not write linker script"; return false; } }
        if (!runTool(linker,{L"-m",mode == RuntimeX86Mode::X86 ? L"i386pe" : L"i386pep",L"--image-base",win::wide(imageBase.str()),L"-T",script.wstring(),L"-e",L"quartz_patch_start",L"-o",executable.wstring(),object.wstring()},error)) return false;
        if (!runTool(extract,{L"-O",L"binary",L"-j",L".text",executable.wstring(),binary.wstring()},error)) return false;
        std::ifstream file(binary,std::ios::binary);
        bytes.resize(size);
        if (!file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size))) { bytes.clear(); error = "could not read assembled patch"; return false; }
        error.clear(); return true;
    }
}
#endif
