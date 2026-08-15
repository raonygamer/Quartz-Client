
    static std::vector<ShaderPreset> ShaderPresets = buildShaderPresets();

    static_assert(sizeof(Color32) == 3);
    static_assert(sizeof(FramebufferSetPayload<MatrixSize>) == 340);
    static_assert(sizeof(PerformancePayload) == 36);
    static_assert(sizeof(MatrixTimingProbeResult<ActiveProbeRows>) == 52);

    struct PerformanceSnapshot
    {
        std::uint32_t CoreClock = 0;
        std::uint32_t BeginScanTicks = 0;
        std::uint32_t ScanTicks = 0;
        std::uint32_t EndScanTicks = 0;
        std::uint32_t StateUpdateTicks = 0;
        std::uint32_t HIDTicks = 0;
        std::uint32_t RGBTicks = 0;
        std::uint32_t AverageScanPeriodTicks = 0;
        std::uint32_t RGBSlotMaxTicks = 0;
    };

    struct SharedDeviceState
    {
        std::mutex Mutex;
        PerformanceSnapshot Performance{};
        MatrixTimingProbeResult<ActiveProbeRows> TimingProbe{};
        bool HasPerformance = false;
        bool HasTimingProbe = false;
        std::uint64_t ReceivedPackets = 0;
    };


    struct ReactiveKeyBinding
    {
        std::uint16_t Key;
        std::uint8_t Row;
        std::uint8_t Column;
    };

    constexpr auto ReactiveKeyBindings = std::to_array<ReactiveKeyBinding>({
        {KEY_ESC, 0, 0}, {KEY_F1, 0, 1}, {KEY_F2, 0, 2}, {KEY_F3, 0, 3}, {KEY_F4, 0, 4}, {KEY_F5, 0, 5}, {KEY_F6, 0, 6}, {KEY_F7, 0, 7},
        {KEY_F8, 0, 8}, {KEY_F9, 0, 9}, {KEY_F10, 0, 10}, {KEY_F11, 0, 11}, {KEY_F12, 0, 12}, {KEY_SYSRQ, 0, 13}, {KEY_SCROLLLOCK, 0, 14}, {KEY_PAUSE, 0, 15},
        {KEY_GRAVE, 1, 0}, {KEY_1, 1, 1}, {KEY_2, 1, 2}, {KEY_3, 1, 3}, {KEY_4, 1, 4}, {KEY_5, 1, 5}, {KEY_6, 1, 6}, {KEY_7, 1, 7},
        {KEY_8, 1, 8}, {KEY_9, 1, 9}, {KEY_0, 1, 10}, {KEY_MINUS, 1, 11}, {KEY_EQUAL, 1, 12}, {KEY_BACKSPACE, 1, 12}, {KEY_INSERT, 1, 13}, {KEY_HOME, 1, 14}, {KEY_PAGEUP, 1, 15},
        {KEY_TAB, 2, 0}, {KEY_Q, 2, 1}, {KEY_W, 2, 2}, {KEY_E, 2, 3}, {KEY_R, 2, 4}, {KEY_T, 2, 5}, {KEY_Y, 2, 6}, {KEY_U, 2, 7},
        {KEY_I, 2, 8}, {KEY_O, 2, 9}, {KEY_P, 2, 10}, {KEY_LEFTBRACE, 2, 11}, {KEY_RIGHTBRACE, 2, 12}, {KEY_DELETE, 2, 13}, {KEY_END, 2, 14}, {KEY_PAGEDOWN, 2, 15},
        {KEY_CAPSLOCK, 3, 0}, {KEY_A, 3, 1}, {KEY_S, 3, 2}, {KEY_D, 3, 3}, {KEY_F, 3, 4}, {KEY_G, 3, 5}, {KEY_H, 3, 6}, {KEY_J, 3, 7},
        {KEY_K, 3, 8}, {KEY_L, 3, 9}, {KEY_SEMICOLON, 3, 10}, {KEY_APOSTROPHE, 3, 11}, {KEY_BACKSLASH, 3, 12}, {KEY_ENTER, 3, 15},
        {KEY_LEFTSHIFT, 4, 0}, {KEY_102ND, 4, 1}, {KEY_Z, 4, 2}, {KEY_X, 4, 3}, {KEY_C, 4, 4}, {KEY_V, 4, 5}, {KEY_B, 4, 6}, {KEY_N, 4, 7}, {KEY_M, 4, 8},
        {KEY_COMMA, 4, 9}, {KEY_DOT, 4, 10}, {KEY_SLASH, 4, 11}, {KEY_RO, 4, 12}, {KEY_UP, 4, 14}, {KEY_RIGHTSHIFT, 4, 15},
        {KEY_LEFTCTRL, 5, 0}, {KEY_LEFTMETA, 5, 1}, {KEY_LEFTALT, 5, 2}, {KEY_SPACE, 5, 6}, {KEY_RIGHTALT, 5, 9}, {KEY_MENU, 5, 11}, {KEY_RIGHTCTRL, 5, 12},
        {KEY_LEFT, 5, 13}, {KEY_DOWN, 5, 14}, {KEY_RIGHT, 5, 15}
    });

    static const ReactiveKeyBinding* findReactiveKeyBinding(const std::uint16_t key) noexcept
    {
        for (const auto& binding : ReactiveKeyBindings)
            if (binding.Key == key)
                return &binding;
        return nullptr;
    }

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
            _keyDown.fill(false);
            std::lock_guard lock(_mutex);
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
            const bool wasDown = _keyDown[event.code];
            _keyDown[event.code] = event.value != 0;
            if (event.value == 1 && !wasDown && restoreShortcutDown()) _restoreRequested.store(true);

            const auto* binding = findReactiveKeyBinding(event.code);
            if (!binding) return;
            const std::size_t index = static_cast<std::size_t>(binding->Row) * Columns + binding->Column;
            std::lock_guard lock(_mutex);
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

    class AppCpuMeter
    {
    public:
        float update(const double wallTime) noexcept
        {
            const std::clock_t cpuNow = std::clock();
            if (_lastWallTime < 0.0)
            {
                _lastWallTime = wallTime;
                _lastCpuTime = cpuNow;
                return _usage;
            }

            const double wallDelta = wallTime - _lastWallTime;
            if (wallDelta < 0.25)
                return _usage;

            const double cpuDelta = static_cast<double>(cpuNow - _lastCpuTime) / CLOCKS_PER_SEC;
            _usage = static_cast<float>(std::max(0.0, cpuDelta / wallDelta * 100.0));
            _lastWallTime = wallTime;
            _lastCpuTime = cpuNow;
            return _usage;
        }

    private:
        double _lastWallTime = -1.0;
        std::clock_t _lastCpuTime = 0;
        float _usage = 0.0f;
    };

    struct VisualizerSettings
    {
        bool Enabled = true;
        bool SendFramebuffer = true;
        bool MediaArtworkColor = true;
        bool ForceFullRow = true;
        bool ShowFramebuffer = true;
        bool ShowAnalysisSpectrum = true;
        bool ShowMappedSpectrum = true;
        bool LimitMainLoop = true;
        bool AutoReconnect = true;
        bool ShaderRecompileOnChange = false;
        bool ShaderKeyStateUniforms = true;
        bool ShaderCapsLockColorEnabled = true;
        bool ShaderScrollLockColorEnabled = true;
        int ShaderFramebufferWidth = DefaultShaderWidth;
        int ShaderFramebufferHeight = DefaultShaderHeight;
        int FrameRate = 240;
        int AnalysisBandCount = 512;
        int BassColumns = 3;
        int BassEndBand = 16;
        int FullRow = 5;
        float OverallGain = 1.62f;
        bool AutomaticOverallGain = false;
        float AutoGainBaseline = 1.62f;
        float AutoGainTargetRms = 0.10f;
        float AutoGainAdaptation = 0.35f;
        float AutoGainMinCorrection = 0.45f;
        float AutoGainMaxCorrection = 3.00f;
        float AutoGainSilenceGate = 0.0025f;
        float GlobalBrightness = 1.0f;
        float LiveOutputInterpolation = 0.35f;
        float WaveSpeed = 0.40f;
        float FeatherRows = 2.5f;
        float Saturation = 2.0f;
        float AttackSpeed = 3.5f;
        float ReleaseSpeed = 40.5f;
        float BassActivationThreshold = 0.65f;
        float BassMaxBoost = 1.68f;
        float ColorTransitionSpeed = 1.5f;
        float MediaColorBlend = 1.0f;
        float MinFrequency = 50.0f;
        float MaxFrequency = 16000.0f;
        float MinDb = -72.0f;
        float MaxDb = -15.0f;
        float StatisticsInterval = 0.20f;
        float MediaPollInterval = 0.50f;
        float ShaderEditorZoom = 1.0f;
        float ShaderTransitionSeconds = 0.35f;
        std::array<float, 3> SolidColor{1.0f, 0.0f, 0.0f};
        std::array<float, 3> ShaderCapsLockColor{0.10f, 0.80f, 1.00f};
        std::array<float, 3> ShaderScrollLockColor{1.00f, 0.20f, 0.55f};
        std::array<float, Columns> ColumnGain{0.55f, 0.58f, 0.56f, 0.72f, 0.78f, 0.72f, 0.81f, 0.74f, 0.77f, 0.84f, 0.84f, 0.86f, 0.99f, 0.99f, 0.99f, 0.92f};
        char AudioSource[128] = "easyeffects_sink.monitor";
        int BaseColorMode = 0;
        int ShaderDownsampleMode = 0;
        int ShaderPresetIndex = 1;
        std::string ShaderId = "builtin.rainbow_equalizer";
    };

    enum class ViewPage : std::uint8_t
    {
        Main,
        ShaderEditor
    };

    struct ShaderEditorState
    {
        TextEditor Vertex;
        TextEditor Fragment;
        int ActiveStage = 0;
        bool Initialized = false;
        bool ZoomInWasDown = false;
        bool ZoomOutWasDown = false;
        bool ZoomResetWasDown = false;
    };

    static TextEditor::Palette shaderEditorPalette()
    {
        auto palette = TextEditor::GetDarkPalette();
        palette[static_cast<std::size_t>(TextEditor::Color::background)] = IM_COL32(13, 13, 13, 255);
        palette[static_cast<std::size_t>(TextEditor::Color::selection)] = IM_COL32(90, 90, 90, 110);
        palette[static_cast<std::size_t>(TextEditor::Color::lineNumber)] = IM_COL32(105, 105, 105, 255);
        palette[static_cast<std::size_t>(TextEditor::Color::currentLineNumber)] = IM_COL32(230, 230, 230, 255);
        palette[static_cast<std::size_t>(TextEditor::Color::matchingBracketBackground)] = IM_COL32(80, 80, 80, 100);
        return palette;
    }

    static void configureShaderEditor(TextEditor& editor, const std::string_view source)
    {
        editor.SetPalette(shaderEditorPalette());
        editor.SetLanguage(TextEditor::Language::Glsl());
        editor.SetTabSize(4);
        editor.SetInsertSpacesOnTabs(true);
        editor.SetAutoIndentEnabled(true);
        editor.SetShowLineNumbersEnabled(true);
        editor.SetShowMiniMapEnabled(true);
        editor.SetMiniMapColumns(28);
        editor.SetShowMatchingBrackets(true);
        editor.SetCompletePairedGlyphs(true);
        editor.SetLineFoldingEnabled(true);
        editor.SetWordWrapEnabled(false);
        editor.SetText(source);
    }

    static void initializeShaderEditors(ShaderEditorState& state, const std::string_view vertexSource, const std::string_view fragmentSource)
    {
        if (state.Initialized)
            return;
        configureShaderEditor(state.Vertex, vertexSource);
        configureShaderEditor(state.Fragment, fragmentSource);
        state.Initialized = true;
    }



    static std::string g_SettingsStatus = "Settings not loaded yet";

    static std::filesystem::path settingsPath()
    {
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            return std::filesystem::path(xdg) / "quartz" / "visualizer.ini";
        if (const char* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / ".config" / "quartz" / "visualizer.ini";
        return "quartz_visualizer.ini";
    }

    static std::filesystem::path vertexShaderPath()
    {
        return settingsPath().parent_path() / "visualizer.vert";
    }

    static std::filesystem::path fragmentShaderPath()
    {
        return settingsPath().parent_path() / "visualizer.frag";
    }

    static std::filesystem::path shaderMaterialPath()
    {
        return settingsPath().parent_path() / "visualizer.material.ini";
    }

    static void setShaderSource(std::array<char, ShaderSourceCapacity>& destination, const std::string_view source)
    {
        const std::size_t size = std::min(source.size(), destination.size() - 1);
        std::memcpy(destination.data(), source.data(), size);
        destination[size] = '\0';
    }

    static bool loadTextFile(const std::filesystem::path& path, std::array<char, ShaderSourceCapacity>& source)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return false;
        const auto size = file.tellg();
        if (size <= 0 || static_cast<std::size_t>(size) >= source.size())
            return false;
        file.seekg(0);
        file.read(source.data(), size);
        if (!file)
            return false;
        source[static_cast<std::size_t>(size)] = '\0';
        return true;
    }

    static bool saveTextFile(const std::filesystem::path& path, const std::array<char, ShaderSourceCapacity>& source)
    {
        std::error_code error;
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file.write(source.data(), static_cast<std::streamsize>(std::strlen(source.data())));
        return static_cast<bool>(file);
    }

    static bool replaceAll(std::string& value, const std::string_view from, const std::string_view to)
    {
        if (from.empty())
            return false;
        bool changed = false;
        std::size_t offset = 0;
        while ((offset = value.find(from, offset)) != std::string::npos)
        {
            value.replace(offset, from.size(), to);
            offset += to.size();
            changed = true;
        }
        return changed;
    }

    static bool migrateObsoleteShaderSource(std::array<char, ShaderSourceCapacity>& source)
    {
        std::string value(source.data());
        const std::string oldState = std::string("u") + "C" + "trl";
        const std::string oldColor = oldState + "Color";
        const std::string oldEnabled = oldColor + "Enabled";
        bool changed = false;
        changed |= replaceAll(value, oldEnabled, "uCapsLockColorEnabled");
        changed |= replaceAll(value, oldColor, "uCapsLockColor");
        changed |= replaceAll(value, oldState, "uCapsLock");
        changed |= replaceAll(value, "row == 5 && (column == 0 || column == 12)", "row == 3 && column == 0");
        changed |= replaceAll(value, "keyRow == 5 && (keyColumn == 0 || keyColumn == 12)", "keyRow == 3 && keyColumn == 0");
        if (changed)
            setShaderSource(source, value);
        return changed;
    }

    static std::filesystem::path shaderLibraryPath()
    {
        return settingsPath().parent_path() / "shaders";
    }

    static bool loadTextFileString(const std::filesystem::path& path, std::string& source)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return false;
        const auto size = file.tellg();
        if (size <= 0 || static_cast<std::size_t>(size) >= ShaderSourceCapacity) return false;
        source.resize(static_cast<std::size_t>(size));
        file.seekg(0);
        file.read(source.data(), size);
        return static_cast<bool>(file);
    }

    static std::optional<std::pair<std::string, std::string>> parseShaderAnnotation(const std::string_view source)
    {
        const std::size_t marker = source.find("@shader");
        if (marker == std::string_view::npos) return std::nullopt;
        const std::size_t end = source.find('\n', marker);
        const std::string line(source.substr(marker, end == std::string_view::npos ? source.size() - marker : end - marker));
        std::smatch match;
        static const std::regex IdQuoted(R"(id\s*=\s*[\"']([^\"']+)[\"'])", std::regex::icase);
        static const std::regex IdBare(R"(id\s*=\s*([A-Za-z0-9_.:/-]+))", std::regex::icase);
        static const std::regex LabelQuoted(R"(label\s*=\s*[\"']([^\"']+)[\"'])", std::regex::icase);
        std::string id, label;
        if (std::regex_search(line, match, IdQuoted)) id = match[1].str();
        else if (std::regex_search(line, match, IdBare)) id = match[1].str();
        if (std::regex_search(line, match, LabelQuoted)) label = match[1].str();
        if (id.empty()) return std::nullopt;
        if (label.empty()) label = id;
        return std::pair{id, label};
    }

    static void refreshShaderLibrary()
    {
        std::erase_if(ShaderPresets, [](const ShaderPreset& preset) { return !preset.BuiltIn; });
        std::error_code ec;
        const auto root = shaderLibraryPath();
        std::filesystem::create_directories(root, ec);
        if (ec) return;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec))
        {
            if (ec || !entry.is_regular_file()) continue;
            const auto extension = entry.path().extension().string();
            if (extension != ".glsl" && extension != ".frag" && extension != ".fs") continue;
            std::string source;
            if (!loadTextFileString(entry.path(), source)) continue;
            const auto annotation = parseShaderAnnotation(source);
            if (!annotation) continue;
            const auto& [id, label] = *annotation;
            if (std::ranges::any_of(ShaderPresets, [&](const ShaderPreset& preset) { return preset.Id == id; })) continue;
            ShaderPreset preset;
            preset.Name = label;
            preset.FragmentSource = std::move(source);
            preset.Id = id;
            preset.SourcePath = entry.path();
            preset.BuiltIn = false;
            ShaderPresets.emplace_back(std::move(preset));
        }
    }

    static const ShaderPreset* findShaderPresetById(const std::string_view id) noexcept
    {
        const auto it = std::ranges::find_if(ShaderPresets, [&](const ShaderPreset& preset) { return preset.Id == id; });
        return it == ShaderPresets.end() ? nullptr : &*it;
    }

    static int shaderPresetIndexById(const std::string_view id) noexcept
    {
        for (std::size_t i = 0; i < ShaderPresets.size(); ++i) if (ShaderPresets[i].Id == id) return static_cast<int>(i + 1);
        return 0;
    }

    static bool importShaderToLibrary(const std::filesystem::path& sourcePath, std::string& importedId, std::string& error)
    {
        std::string source;
        if (!loadTextFileString(sourcePath, source)) { error = "could not read shader file"; return false; }
        const auto annotation = parseShaderAnnotation(source);
        if (!annotation) { error = "shader has no @shader id=... annotation"; return false; }
        importedId = annotation->first;
        std::error_code ec;
        std::filesystem::create_directories(shaderLibraryPath(), ec);
        if (ec) { error = "could not create shader library: " + ec.message(); return false; }
        const std::filesystem::path destination = shaderLibraryPath() / sourcePath.filename();
        if (sourcePath.lexically_normal() != destination.lexically_normal())
        {
            std::filesystem::copy_file(sourcePath, destination, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) { error = "could not import shader: " + ec.message(); return false; }
        }
        refreshShaderLibrary();
        error.clear();
        return findShaderPresetById(importedId) != nullptr;
    }


    static std::string shaderPresetIdByIndex(const int index)
    {
        return index > 0 && index <= static_cast<int>(ShaderPresets.size()) ? ShaderPresets[static_cast<std::size_t>(index - 1)].Id : std::string{};
    }

    static void loadShaderSources(std::array<char, ShaderSourceCapacity>& vertexSource, std::array<char, ShaderSourceCapacity>& fragmentSource)
    {
        setShaderSource(vertexSource, DefaultVertexShaderSource);
        setShaderSource(fragmentSource, ShaderPresets.front().FragmentSource);
        loadTextFile(vertexShaderPath(), vertexSource);
        loadTextFile(fragmentShaderPath(), fragmentSource);
    }

    static int detectShaderPreset(const std::string_view source) noexcept
    {
        for (std::size_t i = 0; i < ShaderPresets.size(); ++i)
            if (source == std::string_view(ShaderPresets[i].FragmentSource)) return static_cast<int>(i + 1);
        return 0;
    }

    static void saveShaderSources(const std::array<char, ShaderSourceCapacity>& vertexSource, const std::array<char, ShaderSourceCapacity>& fragmentSource)
    {
        saveTextFile(vertexShaderPath(), vertexSource);
        saveTextFile(fragmentShaderPath(), fragmentSource);
    }

    static std::string serializeSettings(const VisualizerSettings& settings)
    {
        std::ostringstream stream;
        stream << std::boolalpha << std::setprecision(9);
        stream << "Enabled=" << settings.Enabled << '\n';
        stream << "SendFramebuffer=" << settings.SendFramebuffer << '\n';
        stream << "MediaArtworkColor=" << settings.MediaArtworkColor << '\n';
        stream << "ForceFullRow=" << settings.ForceFullRow << '\n';
        stream << "ShowFramebuffer=" << settings.ShowFramebuffer << '\n';
        stream << "ShowAnalysisSpectrum=" << settings.ShowAnalysisSpectrum << '\n';
        stream << "ShowMappedSpectrum=" << settings.ShowMappedSpectrum << '\n';
        stream << "LimitMainLoop=" << settings.LimitMainLoop << '\n';
        stream << "AutoReconnect=" << settings.AutoReconnect << '\n';
        stream << "ShaderRecompileOnChange=" << settings.ShaderRecompileOnChange << '\n';
        stream << "ShaderKeyStateUniforms=" << settings.ShaderKeyStateUniforms << '\n';
        stream << "ShaderCapsLockColorEnabled=" << settings.ShaderCapsLockColorEnabled << '\n';
        stream << "ShaderScrollLockColorEnabled=" << settings.ShaderScrollLockColorEnabled << '\n';
        stream << "ShaderFramebufferWidth=" << settings.ShaderFramebufferWidth << '\n';
        stream << "ShaderFramebufferHeight=" << settings.ShaderFramebufferHeight << '\n';
        stream << "FrameRate=" << settings.FrameRate << '\n';
        stream << "AnalysisBandCount=" << settings.AnalysisBandCount << '\n';
        stream << "BassColumns=" << settings.BassColumns << '\n';
        stream << "BassEndBand=" << settings.BassEndBand << '\n';
        stream << "FullRow=" << settings.FullRow << '\n';
        stream << "OverallGain=" << settings.OverallGain << '\n';
        stream << "AutomaticOverallGain=" << settings.AutomaticOverallGain << '\n';
        stream << "AutoGainBaseline=" << settings.AutoGainBaseline << '\n';
        stream << "AutoGainTargetRms=" << settings.AutoGainTargetRms << '\n';
        stream << "AutoGainAdaptation=" << settings.AutoGainAdaptation << '\n';
        stream << "AutoGainMinCorrection=" << settings.AutoGainMinCorrection << '\n';
        stream << "AutoGainMaxCorrection=" << settings.AutoGainMaxCorrection << '\n';
        stream << "AutoGainSilenceGate=" << settings.AutoGainSilenceGate << '\n';
        stream << "GlobalBrightness=" << settings.GlobalBrightness << '\n';
        stream << "LiveOutputInterpolation=" << settings.LiveOutputInterpolation << '\n';
        stream << "WaveSpeed=" << settings.WaveSpeed << '\n';
        stream << "FeatherRows=" << settings.FeatherRows << '\n';
        stream << "Saturation=" << settings.Saturation << '\n';
        stream << "AttackSpeed=" << settings.AttackSpeed << '\n';
        stream << "ReleaseSpeed=" << settings.ReleaseSpeed << '\n';
        stream << "BassActivationThreshold=" << settings.BassActivationThreshold << '\n';
        stream << "BassMaxBoost=" << settings.BassMaxBoost << '\n';
        stream << "ColorTransitionSpeed=" << settings.ColorTransitionSpeed << '\n';
        stream << "MediaColorBlend=" << settings.MediaColorBlend << '\n';
        stream << "MinFrequency=" << settings.MinFrequency << '\n';
        stream << "MaxFrequency=" << settings.MaxFrequency << '\n';
        stream << "MinDb=" << settings.MinDb << '\n';
        stream << "MaxDb=" << settings.MaxDb << '\n';
        stream << "StatisticsInterval=" << settings.StatisticsInterval << '\n';
        stream << "MediaPollInterval=" << settings.MediaPollInterval << '\n';
        stream << "ShaderEditorZoom=" << settings.ShaderEditorZoom << '\n';
        stream << "ShaderTransitionSeconds=" << settings.ShaderTransitionSeconds << '\n';
        stream << "BaseColorMode=" << settings.BaseColorMode << '\n';
        stream << "ShaderDownsampleMode=" << settings.ShaderDownsampleMode << '\n';
        stream << "ShaderPresetIndex=" << settings.ShaderPresetIndex << '\n';
        stream << "ShaderId=" << settings.ShaderId << '\n';
        stream << "AudioSource=" << settings.AudioSource << '\n';
        stream << "SolidColor=" << settings.SolidColor[0] << ',' << settings.SolidColor[1] << ',' << settings.SolidColor[2] << '\n';
        stream << "ShaderCapsLockColor=" << settings.ShaderCapsLockColor[0] << ',' << settings.ShaderCapsLockColor[1] << ',' << settings.ShaderCapsLockColor[2] << '\n';
        stream << "ShaderScrollLockColor=" << settings.ShaderScrollLockColor[0] << ',' << settings.ShaderScrollLockColor[1] << ',' << settings.ShaderScrollLockColor[2] << '\n';
        stream << "ColumnGain=";
        for (std::size_t i = 0; i < settings.ColumnGain.size(); ++i)
        {
            if (i != 0)
                stream << ',';
            stream << settings.ColumnGain[i];
        }
        stream << '\n';
        return stream.str();
    }

    static std::string_view trimSettingValue(std::string_view value) noexcept
    {
        const auto begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string_view::npos) return {};
        const auto end = value.find_last_not_of(" \t\r\n");
        return value.substr(begin, end - begin + 1);
    }

    static bool parseBool(const std::string_view value, bool& result)
    {
        if (value == "true" || value == "1") { result = true; return true; }
        if (value == "false" || value == "0") { result = false; return true; }
        return false;
    }

    template<typename T>
    static bool parseNumber(const std::string_view value, T& result)
    {
        T parsed{};
        const char* begin = value.data();
        const char* end = value.data() + value.size();
        std::from_chars_result parsedResult{};
        if constexpr (std::is_integral_v<T>) parsedResult = std::from_chars(begin, end, parsed);
        else parsedResult = std::from_chars(begin, end, parsed, std::chars_format::general);
        if (parsedResult.ec != std::errc{} || parsedResult.ptr != end) return false;
        if constexpr (std::is_floating_point_v<T>) if (!std::isfinite(parsed)) return false;
        result = parsed;
        return true;
    }

    template<std::size_t N>
    static bool parseFloatArray(const std::string_view value, std::array<float, N>& result)
    {
        std::array<float, N> parsed{};
        std::size_t start = 0;
        for (std::size_t i = 0; i < N; ++i)
        {
            const std::size_t end = value.find(',', start);
            const std::string_view part = trimSettingValue(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start));
            if (!parseNumber(part, parsed[i]))
                return false;
            if (i + 1 < N)
            {
                if (end == std::string_view::npos)
                    return false;
                start = end + 1;
            }
            else if (end != std::string_view::npos)
                return false;
        }
        result = parsed;
        return true;
    }

    static void loadSettings(VisualizerSettings& settings)
    {
        const auto path = settingsPath();
        std::ifstream file(path);
        if (!file)
        {
            g_SettingsStatus = "No settings file; using defaults";
            return;
        }

        std::size_t loaded = 0;
        std::size_t failed = 0;
        std::string line;
        while (std::getline(file, line))
        {
            std::string_view whole = trimSettingValue(line);
            if (whole.empty() || whole.front() == '#' || whole.front() == ';') continue;
            const std::size_t separator = whole.find('=');
            if (separator == std::string_view::npos) { ++failed; continue; }
            std::string_view key = trimSettingValue(whole.substr(0, separator));
            const std::string_view value = trimSettingValue(whole.substr(separator + 1));
            if (key.size() >= 3 && static_cast<unsigned char>(key[0]) == 0xEF && static_cast<unsigned char>(key[1]) == 0xBB && static_cast<unsigned char>(key[2]) == 0xBF) key.remove_prefix(3);

#define LOAD_BOOL(name) if (key == #name) { if (parseBool(value, settings.name)) ++loaded; else ++failed; continue; }
#define LOAD_NUM(name) if (key == #name) { if (parseNumber(value, settings.name)) ++loaded; else ++failed; continue; }
            LOAD_BOOL(Enabled)
            LOAD_BOOL(SendFramebuffer)
            LOAD_BOOL(MediaArtworkColor)
            LOAD_BOOL(ForceFullRow)
            LOAD_BOOL(ShowFramebuffer)
            LOAD_BOOL(ShowAnalysisSpectrum)
            LOAD_BOOL(ShowMappedSpectrum)
            LOAD_BOOL(LimitMainLoop)
            LOAD_BOOL(AutoReconnect)
            LOAD_BOOL(ShaderRecompileOnChange)
            LOAD_BOOL(ShaderKeyStateUniforms)
            LOAD_BOOL(ShaderCapsLockColorEnabled)
            LOAD_BOOL(ShaderScrollLockColorEnabled)
            LOAD_NUM(ShaderFramebufferWidth)
            LOAD_NUM(ShaderFramebufferHeight)
            LOAD_NUM(FrameRate)
            LOAD_NUM(AnalysisBandCount)
            LOAD_NUM(BassColumns)
            LOAD_NUM(BassEndBand)
            LOAD_NUM(FullRow)
            LOAD_NUM(OverallGain)
            LOAD_BOOL(AutomaticOverallGain)
            LOAD_NUM(AutoGainBaseline)
            LOAD_NUM(AutoGainTargetRms)
            LOAD_NUM(AutoGainAdaptation)
            LOAD_NUM(AutoGainMinCorrection)
            LOAD_NUM(AutoGainMaxCorrection)
            LOAD_NUM(AutoGainSilenceGate)
            LOAD_NUM(GlobalBrightness)
            LOAD_NUM(LiveOutputInterpolation)
            LOAD_NUM(WaveSpeed)
            LOAD_NUM(FeatherRows)
            LOAD_NUM(Saturation)
            LOAD_NUM(AttackSpeed)
            LOAD_NUM(ReleaseSpeed)
            LOAD_NUM(BassActivationThreshold)
            LOAD_NUM(BassMaxBoost)
            LOAD_NUM(ColorTransitionSpeed)
            LOAD_NUM(MediaColorBlend)
            LOAD_NUM(MinFrequency)
            LOAD_NUM(MaxFrequency)
            LOAD_NUM(MinDb)
            LOAD_NUM(MaxDb)
            LOAD_NUM(StatisticsInterval)
            LOAD_NUM(MediaPollInterval)
            LOAD_NUM(ShaderEditorZoom)
            LOAD_NUM(ShaderTransitionSeconds)
            LOAD_NUM(BaseColorMode)
            LOAD_NUM(ShaderDownsampleMode)
#undef LOAD_NUM
#undef LOAD_BOOL
            if (key == "StartMinimized" || key == "StartHidden") { ++loaded; continue; }
            // ShaderPresetIndex is deliberately ignored. The fragment file is authoritative and
            // the preset is detected from its contents after loading, so preset reordering cannot
            // silently replace a user's saved shader.
            if (key == "ShaderPresetIndex") { ++loaded; continue; }
            if (key == "ShaderId") { settings.ShaderId = value; ++loaded; continue; }
            if (key == "AudioSource")
            {
                const std::size_t count = std::min(value.size(), sizeof(settings.AudioSource) - 1);
                std::memcpy(settings.AudioSource, value.data(), count);
                settings.AudioSource[count] = '\0';
                ++loaded;
            }
            else if (key == "SolidColor") { parseFloatArray(value, settings.SolidColor) ? ++loaded : ++failed; }
            else if (key == "ShaderCapsLockColor") { parseFloatArray(value, settings.ShaderCapsLockColor) ? ++loaded : ++failed; }
            else if (key == "ShaderScrollLockColor") { parseFloatArray(value, settings.ShaderScrollLockColor) ? ++loaded : ++failed; }
            else if (key == "ColumnGain") { parseFloatArray(value, settings.ColumnGain) ? ++loaded : ++failed; }
        }
        settings.FrameRate = std::clamp(settings.FrameRate, 30, 500);
        settings.AnalysisBandCount = std::clamp(settings.AnalysisBandCount, 32, static_cast<int>(FFTSize));
        settings.BassColumns = std::clamp(settings.BassColumns, 2, 8);
        settings.BassEndBand = std::clamp(settings.BassEndBand, 0, settings.AnalysisBandCount - 1);
        settings.FullRow = std::clamp(settings.FullRow, 0, static_cast<int>(Rows) - 2);
        settings.BaseColorMode = std::clamp(settings.BaseColorMode, 0, 2);
        settings.ShaderDownsampleMode = std::clamp(settings.ShaderDownsampleMode, 0, 2);
        settings.ShaderFramebufferWidth = std::clamp(settings.ShaderFramebufferWidth, static_cast<int>(Columns), MaxShaderDimension);
        settings.ShaderFramebufferHeight = std::clamp(settings.ShaderFramebufferHeight, static_cast<int>(Rows), MaxShaderDimension);
        settings.ShaderEditorZoom = std::clamp(std::round(settings.ShaderEditorZoom * 10.0f) / 10.0f, 0.60f, 2.50f);
        settings.ShaderTransitionSeconds = std::clamp(settings.ShaderTransitionSeconds, 0.0f, 10.0f);
        settings.GlobalBrightness = std::clamp(settings.GlobalBrightness, 0.0f, 1.0f);
        settings.LiveOutputInterpolation = std::clamp(settings.LiveOutputInterpolation, 0.0f, 1.0f);
        g_SettingsStatus = "Loaded " + std::to_string(loaded) + " settings from " + path.string();
        if (failed != 0) g_SettingsStatus += " (" + std::to_string(failed) + " malformed lines ignored)";
    }
