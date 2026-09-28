// Bind only Quartz's vendor RPC interface to Microsoft's signed, inbox driver.
// Default mode is inspection; --install requires administrator elevation.
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

static int fail(const wchar_t* operation)
{
    std::wcerr << operation << L" failed (Windows error " << GetLastError() << L").\n";
    return 1;
}

int wmain(int argc, wchar_t** argv)
{
    const bool install = argc == 2 && std::wstring(argv[1]) == L"--install";
    if (argc > 1 && !install) { std::wcerr << L"Usage: quartz-winusb-setup [--install]\n"; return 1; }
    std::wofstream log;
    struct RestoreStreams { std::wstreambuf* out; std::wstreambuf* err;
        ~RestoreStreams() { std::wcout.rdbuf(out); std::wcerr.rdbuf(err); }
    } streams{std::wcout.rdbuf(), std::wcerr.rdbuf()};
    if (install)
    {
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
        log.open(std::filesystem::path(executable).parent_path() / "driver-install.log");
        if (log) { std::wcout.rdbuf(log.rdbuf()); std::wcerr.rdbuf(log.rdbuf()); }
    }
    HDEVINFO devices = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devices == INVALID_HANDLE_VALUE) return fail(L"Device enumeration");
    struct Cleanup { HDEVINFO value; ~Cleanup() { SetupDiDestroyDeviceInfoList(value); } } cleanup{devices};
    bool found = false;
    for (DWORD index = 0;; ++index)
    {
        SP_DEVINFO_DATA device{sizeof(device)};
        if (!SetupDiEnumDeviceInfo(devices, index, &device)) break;
        wchar_t id[512]{};
        if (!SetupDiGetDeviceInstanceIdW(devices, &device, id, 512, nullptr)) continue;
        const std::wstring prefix = L"USB\\VID_B147&PID_4131&MI_01\\";
        if (_wcsnicmp(id, prefix.c_str(), prefix.size()) != 0) continue;
        found = true;
        std::wcout << L"Quartz RPC interface: " << id << L"\n";
        wchar_t service[256]{};
        SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_SERVICE, nullptr,
            reinterpret_cast<BYTE*>(service), sizeof(service), nullptr);
        std::wcout << L"Current driver: " << (service[0] ? service : L"(none)") << L"\n";
        SP_DEVINSTALL_PARAMS_W parameters{sizeof(parameters)};
        if (!SetupDiGetDeviceInstallParamsW(devices, &device, &parameters)) return fail(L"Read install parameters");
        parameters.Flags |= DI_ENUMSINGLEINF;
        parameters.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
        const UINT length = GetWindowsDirectoryW(parameters.DriverPath, MAX_PATH);
        if (!length || length + 12 >= MAX_PATH) return fail(L"Windows directory");
        wcscat_s(parameters.DriverPath, L"\\INF\\winusb.inf");
        if (!SetupDiSetDeviceInstallParamsW(devices, &device, &parameters)) return fail(L"Set driver search");
        if (!SetupDiBuildDriverInfoList(devices, &device, SPDIT_CLASSDRIVER)) return fail(L"Enumerate inbox WinUSB driver");
        bool selected = false;
        for (DWORD driverIndex = 0;; ++driverIndex)
        {
            SP_DRVINFO_DATA_W driver{sizeof(driver)};
            if (!SetupDiEnumDriverInfoW(devices, &device, SPDIT_CLASSDRIVER, driverIndex, &driver)) break;
            DWORD required = 0;
            SetupDiGetDriverInfoDetailW(devices, &device, &driver, nullptr, 0, &required);
            if (!required) continue;
            std::vector<BYTE> storage(required);
            auto* detail = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(storage.data());
            detail->cbSize = sizeof(*detail);
            if (!SetupDiGetDriverInfoDetailW(devices, &device, &driver, detail, required, nullptr)) continue;
            if (_wcsicmp(detail->SectionName, L"WINUSB") != 0) continue;
            std::wcout << L"Selected: " << driver.Description << L" / " << driver.ProviderName
                       << L" / " << detail->InfFileName << L"\n";
            selected = true;
            if (install)
            {
                BOOL reboot = FALSE;
                using InstallDevice = BOOL(WINAPI*)(HWND, HDEVINFO, SP_DEVINFO_DATA*, SP_DRVINFO_DATA_W*, DWORD, BOOL*);
                HMODULE newdev = LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                const auto installDevice = reinterpret_cast<InstallDevice>(newdev ? GetProcAddress(newdev, "DiInstallDevice") : nullptr);
                if (!installDevice || !installDevice(nullptr, devices, &device, &driver, 0, &reboot)) return fail(L"Install WinUSB");
                FreeLibrary(newdev);
                // The generic inbox INF has no product-specific interface GUID.
                // libusb needs one to discover and open this vendor interface.
                HKEY key = SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_SET_VALUE);
                if (key == INVALID_HANDLE_VALUE) return fail(L"Open RPC interface parameters");
                constexpr wchar_t guid[] = L"{BC447D83-CE42-4BD7-87A6-1BC5B7495519}\0";
                const LSTATUS result = RegSetValueExW(key, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ,
                    reinterpret_cast<const BYTE*>(guid), sizeof(guid));
                RegCloseKey(key);
                if (result != ERROR_SUCCESS) { SetLastError(result); return fail(L"Set RPC interface GUID"); }
                SP_PROPCHANGE_PARAMS change{};
                change.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
                change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
                change.StateChange = DICS_PROPCHANGE; change.Scope = DICS_FLAG_GLOBAL;
                if (!SetupDiSetClassInstallParamsW(devices, &device, &change.ClassInstallHeader, sizeof(change)) ||
                    !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devices, &device))
                    std::wcout << L"Reconnect the keyboard to finish activating WinUSB.\n";
                std::wcout << L"WinUSB installed on the RPC interface. HID keyboard interface unchanged.\n";
                if (reboot) std::wcout << L"Windows reports a restart is required.\n";
            }
            break;
        }
        SetupDiDestroyDriverInfoList(devices, &device, SPDIT_CLASSDRIVER);
        if (!selected) { std::wcerr << L"Microsoft inbox WinUSB driver was not found.\n"; return 1; }
    }
    if (!found) { std::wcerr << L"Connect a Quartz B147:4131 keyboard first.\n"; return 1; }
    if (!install) std::wcout << L"Inspection only. Run with --install as administrator to apply.\n";
    return 0;
}
