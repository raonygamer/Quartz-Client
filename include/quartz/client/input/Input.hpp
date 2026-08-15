#pragma once
#include "quartz/client/Functions.hpp"

namespace quartz::client
{
    struct ReactiveKeyBinding
    {
        std::uint16_t Key;
        std::uint8_t Row;
        std::uint8_t Column;
    };

    struct ReactiveKeyEvent
    {
        float Column = 0.0f;
        float Row = 0.0f;
        float Time = 0.0f;
        float Valid = 0.0f;
    };

    struct ReactiveKeyState
    {
        static constexpr std::size_t EventCount = 16;
        std::array<float, MatrixSize> Down{};
        std::array<ReactiveKeyEvent, EventCount> Events{};
        std::size_t NextEvent = 0;
        bool CapsLockActive = false;
        bool ScrollLockActive = false;
    };

    class EvdevKeyboard
    {
    public:
        ~EvdevKeyboard() { stop(); }

        void start(const double glfwTime)
        {
            if (_running.exchange(true))
                return;
            _glfwBaseTime = glfwTime;
            _steadyBaseTime = std::chrono::steady_clock::now();
            _thread = std::thread(&EvdevKeyboard::run, this);
        }

        void stop() noexcept
        {
            if (!_running.exchange(false))
                return;
            if (_thread.joinable())
                _thread.join();
            closeDevices();
        }

        ReactiveKeyState snapshot() const
        {
            std::lock_guard lock(_mutex);
            return _state;
        }

        bool connected() const
        {
            std::lock_guard lock(_mutex);
            return _connected;
        }

        std::string deviceName() const
        {
            std::lock_guard lock(_mutex);
            return _deviceName;
        }

        std::string status() const
        {
            std::lock_guard lock(_mutex);
            return _status;
        }

        bool consumeRestoreRequest() noexcept { return _restoreRequested.exchange(false); }

        bool shortcutDown(const std::uint16_t key, const bool ctrl, const bool alt, const bool shift) const
        {
            if (key > KEY_MAX) return false;
            std::lock_guard lock(_mutex);
            const bool ctrlDown = _keyDown[KEY_LEFTCTRL] || _keyDown[KEY_RIGHTCTRL];
            const bool altDown = _keyDown[KEY_LEFTALT] || _keyDown[KEY_RIGHTALT];
            const bool shiftDown = _keyDown[KEY_LEFTSHIFT] || _keyDown[KEY_RIGHTSHIFT];
            return (!ctrl || ctrlDown) && (!alt || altDown) && (!shift || shiftDown) && _keyDown[key];
        }

    private:
        struct Device
        {
            int Fd = -1;
            std::string Path;
            std::string Name;
            bool HasKeys = false;
            bool HasCapsLed = false;
            bool HasScrollLed = false;
        };

        static constexpr std::size_t BitsPerLong = sizeof(unsigned long) * 8;

        static bool testBit(const unsigned long* bits, const std::size_t bit) noexcept
        {
            return (bits[bit / BitsPerLong] & (1UL << (bit % BitsPerLong))) != 0;
        }

        float now() const noexcept
        {
            return static_cast<float>(_glfwBaseTime + std::chrono::duration<double>(std::chrono::steady_clock::now() - _steadyBaseTime).count());
        }

        void closeDevices() noexcept
        {
            for (auto& device : _devices)
                if (device.Fd >= 0)
                    ::close(device.Fd);
            _devices.clear();
        }

        void resetKeyState()
        {
            std::lock_guard lock(_mutex);
            _keyDown.fill(false);
            _state.Down.fill(0.0f);
        }

        bool scanDevices()
        {
            closeDevices();
            bool permissionDenied = false;
            std::string firstName;
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator("/dev/input", error))
            {
                if (error)
                    break;
                const std::string filename = entry.path().filename().string();
                if (!filename.starts_with("event"))
                    continue;
                const int fd = ::open(entry.path().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (fd < 0)
                {
                    if (errno == EACCES || errno == EPERM)
                        permissionDenied = true;
                    continue;
                }
                input_id id{};
                if (::ioctl(fd, EVIOCGID, &id) < 0 || id.vendor != VendorId || id.product != ProductId)
                {
                    ::close(fd);
                    continue;
                }
                char name[256]{};
                if (::ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
                    std::snprintf(name, sizeof(name), "Quartz keyboard");

                constexpr std::size_t EventWords = (EV_MAX + BitsPerLong) / BitsPerLong;
                constexpr std::size_t LedWords = (LED_MAX + BitsPerLong) / BitsPerLong;
                std::array<unsigned long, EventWords> eventBits{};
                std::array<unsigned long, LedWords> ledBits{};
                const bool gotEvents = ::ioctl(fd, EVIOCGBIT(0, sizeof(eventBits)), eventBits.data()) >= 0;
                const bool hasKeys = gotEvents && testBit(eventBits.data(), EV_KEY);
                const bool hasLedEvents = gotEvents && testBit(eventBits.data(), EV_LED);
                if (hasLedEvents)
                    ::ioctl(fd, EVIOCGBIT(EV_LED, sizeof(ledBits)), ledBits.data());
                const bool hasCapsLed = hasLedEvents && testBit(ledBits.data(), LED_CAPSL);
                const bool hasScrollLed = hasLedEvents && testBit(ledBits.data(), LED_SCROLLL);

                if (firstName.empty() && hasKeys)
                    firstName = name;
                _devices.push_back({fd, entry.path().string(), name, hasKeys, hasCapsLed, hasScrollLed});
            }

            _hasCapsLedNode = false;
            _hasScrollLedNode = false;
            _scrollLedInitialKnown = false;
            _scrollLedAuthoritative = false;
            std::size_t keyNodes = 0;
            std::size_t ledNodes = 0;
            for (const auto& device : _devices)
            {
                keyNodes += device.HasKeys ? 1u : 0u;
                if (device.HasCapsLed || device.HasScrollLed) ++ledNodes;
                _hasCapsLedNode |= device.HasCapsLed;
                _hasScrollLedNode |= device.HasScrollLed;
                if (firstName.empty()) firstName = device.Name;
            }
            resetKeyState();
            {
                std::lock_guard lock(_mutex);
                _connected = !_devices.empty();
                _deviceName = firstName;
                if (_connected)
                    _status = "evdev connected (" + std::to_string(keyNodes) + " key / " + std::to_string(ledNodes) + " LED nodes; Ctrl+Alt+Shift+Q restores window)";
                else if (permissionDenied)
                    _status = "No readable matching /dev/input/event* device; check evdev permissions/udev rules";
                else
                    _status = "Waiting for matching evdev keyboard";
            }
            if (!_devices.empty())
                refreshLedState();
            return !_devices.empty();
        }

        void refreshLedState()
        {
            constexpr std::size_t LedWords = (LED_MAX + BitsPerLong) / BitsPerLong;
            std::array<unsigned long, LedWords> leds{};
            bool capsKnown = false;
            bool scrollKnown = false;
            bool caps = false;
            bool scroll = false;
            for (const auto& device : _devices)
            {
                if (!device.HasCapsLed && !device.HasScrollLed)
                    continue;
                leds.fill(0);
                if (::ioctl(device.Fd, EVIOCGLED(sizeof(leds)), leds.data()) < 0)
                    continue;
                if (device.HasCapsLed)
                {
                    capsKnown = true;
                    caps |= testBit(leds.data(), LED_CAPSL);
                }
                if (device.HasScrollLed)
                {
                    scrollKnown = true;
                    scroll |= testBit(leds.data(), LED_SCROLLL);
                }
            }
            std::lock_guard lock(_mutex);
            if (capsKnown) _state.CapsLockActive = caps;
            if (scrollKnown)
            {
                _state.ScrollLockActive = scroll;
                _scrollLedInitialKnown = true;
                _initialScrollLedState = scroll;
                _scrollLedAuthoritative = false;
            }
        }

        bool restoreShortcutDown() const noexcept
        {
            const bool ctrl = _keyDown[KEY_LEFTCTRL] || _keyDown[KEY_RIGHTCTRL];
            const bool alt = _keyDown[KEY_LEFTALT] || _keyDown[KEY_RIGHTALT];
            const bool shift = _keyDown[KEY_LEFTSHIFT] || _keyDown[KEY_RIGHTSHIFT];
            return ctrl && alt && shift && _keyDown[KEY_Q];
        }

        void handleKey(const input_event& event)
        {
            if (event.code > KEY_MAX) return;
            const auto* binding = findReactiveKeyBinding(event.code);
            std::lock_guard lock(_mutex);
            const bool wasDown = _keyDown[event.code];
            _keyDown[event.code] = event.value != 0;
            if (event.value == 1 && !wasDown && restoreShortcutDown()) _restoreRequested.store(true);
            if (!binding) return;
            const std::size_t index = static_cast<std::size_t>(binding->Row) * Columns + binding->Column;
            _state.Down[index] = event.value == 0 ? 0.0f : 1.0f;
            if (event.value == 1 && !wasDown)
            {
                _state.Events[_state.NextEvent] = {static_cast<float>(binding->Column), static_cast<float>(binding->Row), now(), 1.0f};
                _state.NextEvent = (_state.NextEvent + 1) % ReactiveKeyState::EventCount;
                if (event.code == KEY_CAPSLOCK) _state.CapsLockActive = !_state.CapsLockActive;
                if (event.code == KEY_SCROLLLOCK) _state.ScrollLockActive = !_state.ScrollLockActive;
            }
        }

        void handleLed(const input_event& event)
        {
            if (event.code != LED_CAPSL && event.code != LED_SCROLLL)
                return;
            std::lock_guard lock(_mutex);
            if (event.code == LED_CAPSL)
            {
                _state.CapsLockActive = event.value != 0;
                return;
            }

            // Scroll Lock is frequently advertised through EV_LED even when the desktop/XKB
            // never actually toggles it. Keep the physical-key fallback unless we observe the
            // LED leave its startup state; once that happens, the OS LED stream is authoritative.
            const bool value = event.value != 0;
            if (!_scrollLedInitialKnown || _scrollLedAuthoritative || value != _initialScrollLedState)
            {
                _scrollLedAuthoritative = true;
                _state.ScrollLockActive = value;
            }
        }

        bool drainDevices()
        {
            for (const auto& device : _devices)
            {
                for (;;)
                {
                    input_event event{};
                    const ssize_t count = ::read(device.Fd, &event, sizeof(event));
                    if (count == static_cast<ssize_t>(sizeof(event)))
                    {
                        if (event.type == EV_KEY)
                            handleKey(event);
                        else if (event.type == EV_LED)
                            handleLed(event);
                        continue;
                    }
                    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                        break;
                    if (count == 0 || (count < 0 && errno == ENODEV))
                        return false;
                    if (count < 0)
                        break;
                }
            }
            return true;
        }

        void run()
        {
            auto nextScan = std::chrono::steady_clock::now();
            while (_running.load())
            {
                const auto current = std::chrono::steady_clock::now();
                if (_devices.empty())
                {
                    if (current >= nextScan)
                    {
                        scanDevices();
                        nextScan = current + std::chrono::seconds(1);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    continue;
                }

                if (!drainDevices())
                {
                    closeDevices();
                    resetKeyState();
                    std::lock_guard lock(_mutex);
                    _connected = false;
                    _status = "evdev device disconnected; rescanning";
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        mutable std::mutex _mutex;
        std::atomic<bool> _running{false};
        std::atomic<bool> _restoreRequested{false};
        std::thread _thread;
        std::vector<Device> _devices;
        std::array<bool, KEY_MAX + 1> _keyDown{};
        ReactiveKeyState _state{};
        bool _connected = false;
        std::string _deviceName;
        std::string _status = "evdev not started";
        bool _hasCapsLedNode = false;
        bool _hasScrollLedNode = false;
        bool _scrollLedInitialKnown = false;
        bool _initialScrollLedState = false;
        bool _scrollLedAuthoritative = false;
        double _glfwBaseTime = 0.0;
        std::chrono::steady_clock::time_point _steadyBaseTime{};
    };

}
