#ifdef _WIN32
#include "quartz/client/Model.hpp"

namespace quartz::client
{
    void EvdevKeyboard::start(double glfwTime)
    {
        if (_running.exchange(true)) return;
        if (_thread.joinable()) _thread.join();
        _glfwBaseTime = glfwTime; _steadyBaseTime = std::chrono::steady_clock::now();
        _thread = std::thread(&EvdevKeyboard::run, this);
    }
    void EvdevKeyboard::stop() noexcept
    {
        _running.store(false);
        if (_thread.joinable()) _thread.join();
    }
    bool EvdevKeyboard::shortcutDown(std::uint16_t key, bool ctrl, bool alt, bool shift) const
    {
        if (key > KEY_MAX) return false;
        std::lock_guard lock(_mutex);
        return _keyDown[key] && (!ctrl || _keyDown[KEY_LEFTCTRL] || _keyDown[KEY_RIGHTCTRL]) &&
               (!alt || _keyDown[KEY_LEFTALT] || _keyDown[KEY_RIGHTALT]) && (!shift || _keyDown[KEY_LEFTSHIFT] || _keyDown[KEY_RIGHTSHIFT]);
    }
    void EvdevKeyboard::scanDevices()
    {
        UINT count = 0;
        if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1)) return;
        std::vector<RAWINPUTDEVICELIST> list(count);
        const UINT read = GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
        if (read == UINT(-1)) return;
        std::set<HANDLE> matching;
        for (UINT index = 0; index < read; ++index)
        {
            const auto& item = list[index];
            if (item.dwType != RIM_TYPEKEYBOARD) continue;
            UINT size = 0;
            if (GetRawInputDeviceInfoW(item.hDevice, RIDI_DEVICENAME, nullptr, &size) == UINT(-1)) continue;
            std::wstring name(size, L'\0');
            if (GetRawInputDeviceInfoW(item.hDevice, RIDI_DEVICENAME, name.data(), &size) == UINT(-1)) continue;
            std::ranges::transform(name, name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
            if (name.find(L"VID_B147") != std::wstring::npos && name.find(L"PID_4131") != std::wstring::npos) matching.insert(item.hDevice);
        }
        std::lock_guard lock(_mutex);
        if (_devices != matching) { _keyDown.fill(false); _state.Down.fill(0); }
        _devices = std::move(matching); _connected = !_devices.empty();
        _deviceName = _connected ? "Quartz keyboard (Windows Raw Input)" : "";
        _status = _connected ? "Raw Input connected; Ctrl+Alt+Shift+Q restores window" : "Waiting for Quartz keyboard; Ctrl+Alt+Shift+Q restores window";
    }
    void EvdevKeyboard::handleKey(const RAWKEYBOARD& raw)
    {
        if (raw.VKey == 255 || raw.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE) return;
        unsigned key = raw.MakeCode;
        if (key == 0x73) key = KEY_RO; // ABNT2 key beside right Shift.
        if (raw.VKey == VK_PAUSE) key = KEY_PAUSE;
        else if (raw.VKey == VK_SNAPSHOT) key = KEY_SYSRQ;
        else if (raw.Flags & RI_KEY_E0)
        {
            switch (raw.MakeCode)
            {
            case 0x1D:key=KEY_RIGHTCTRL;break; case 0x38:key=KEY_RIGHTALT;break;
            case 0x47:key=KEY_HOME;break; case 0x48:key=KEY_UP;break; case 0x49:key=KEY_PAGEUP;break;
            case 0x4B:key=KEY_LEFT;break; case 0x4D:key=KEY_RIGHT;break; case 0x4F:key=KEY_END;break;
            case 0x50:key=KEY_DOWN;break; case 0x51:key=KEY_PAGEDOWN;break; case 0x52:key=KEY_INSERT;break;
            case 0x53:key=KEY_DELETE;break; case 0x5B:key=KEY_LEFTMETA;break; case 0x5C:key=KEY_RIGHTMETA;break;
            case 0x5D:key=KEY_MENU;break; default:return;
            }
        }
        if (!key || key > KEY_MAX) return;
        const bool down = !(raw.Flags & RI_KEY_BREAK);
        std::lock_guard lock(_mutex);
        const bool wasDown = _keyDown[key]; _keyDown[key] = down;
        if (down && !wasDown)
        {
            if (key == KEY_CAPSLOCK) _state.CapsLockActive = !_state.CapsLockActive;
            if (key == KEY_SCROLLLOCK) _state.ScrollLockActive = !_state.ScrollLockActive;
        }
        if (const auto* binding = findReactiveKeyBinding(static_cast<std::uint16_t>(key)))
        {
            _state.Down[binding->Row * Columns + binding->Column] = down ? 1.0f : 0.0f;
            if (down && !wasDown)
            {
                const float time = static_cast<float>(_glfwBaseTime + std::chrono::duration<double>(std::chrono::steady_clock::now() - _steadyBaseTime).count());
                _state.Events[_state.NextEvent] = {float(binding->Column),float(binding->Row),time,1.0f};
                _state.NextEvent = (_state.NextEvent + 1) % ReactiveKeyState::EventCount;
            }
        }
        // Pause has no break scan code on Windows keyboards.
        if (key == KEY_PAUSE) _keyDown[key] = false;
    }
    LRESULT CALLBACK EvdevKeyboard::windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE) SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
        auto* self = reinterpret_cast<EvdevKeyboard*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if (self)
        {
            if (message == WM_HOTKEY) self->_restoreRequested.store(true);
            else if (message == WM_INPUT_DEVICE_CHANGE) self->scanDevices();
            else if (message == WM_INPUT)
            {
                RAWINPUT input{}; UINT size = sizeof(input);
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam),RID_INPUT,&input,&size,sizeof(RAWINPUTHEADER)) != UINT(-1) && input.header.dwType == RIM_TYPEKEYBOARD && self->_devices.contains(input.header.hDevice)) self->handleKey(input.data.keyboard);
            }
        }
        return DefWindowProcW(window,message,wparam,lparam);
    }
    void EvdevKeyboard::run()
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW cls{}; cls.lpfnWndProc = windowProcedure; cls.hInstance = instance; cls.lpszClassName = L"QuartzRawInput";
        RegisterClassW(&cls);
        const HWND window = CreateWindowExW(0,cls.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,instance,this);
        RAWINPUTDEVICE device{1,6,RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,window};
        if (!window || !RegisterRawInputDevices(&device,1,sizeof(device)))
        {
            std::lock_guard lock(_mutex); _status = win::error("RegisterRawInputDevices");
            if (window) DestroyWindow(window); _running.store(false); return;
        }
        RegisterHotKey(window,1,MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT,'Q');
        { std::lock_guard lock(_mutex); _state.CapsLockActive = (GetKeyState(VK_CAPITAL) & 1) != 0; _state.ScrollLockActive = (GetKeyState(VK_SCROLL) & 1) != 0; }
        scanDevices();
        auto nextScan = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (_running.load())
        {
            MSG message;
            while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            if (std::chrono::steady_clock::now() >= nextScan) { scanDevices(); nextScan = std::chrono::steady_clock::now() + std::chrono::seconds(1); }
            MsgWaitForMultipleObjects(0,nullptr,FALSE,10,QS_ALLINPUT);
        }
        UnregisterHotKey(window,1);
        RAWINPUTDEVICE remove{1,6,RIDEV_REMOVE,nullptr}; RegisterRawInputDevices(&remove,1,sizeof(remove));
        DestroyWindow(window);
        std::lock_guard lock(_mutex); _connected = false; _keyDown.fill(false); _state.Down.fill(0); _devices.clear(); _status = "keyboard input stopped";
    }
}
#endif
