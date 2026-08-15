
    static void applyShaderDiagnostics(TextEditor& editor, const std::string_view log)
    {
        editor.ClearMarkers();
        static const std::regex colonPattern(R"((?:ERROR|WARNING)?\s*:?\s*\d+:(\d+)(?::|\())", std::regex::icase);
        static const std::regex parenPattern(R"(\d+\((\d+)\))");
        std::size_t start = 0;
        while (start < log.size())
        {
            const std::size_t end = log.find('\n', start);
            std::string line(log.substr(start, end == std::string_view::npos ? log.size() - start : end - start));
            std::smatch match;
            std::size_t lineNumber = 0;
            if (std::regex_search(line, match, colonPattern) || std::regex_search(line, match, parenPattern))
            {
                try { lineNumber = static_cast<std::size_t>(std::stoul(match[1].str())); } catch (...) { lineNumber = 0; }
            }
            if (lineNumber > 0)
            {
                const bool warning = line.find("warning") != std::string::npos || line.find("WARNING") != std::string::npos;
                const ImU32 numberColor = warning ? IM_COL32(255, 195, 70, 255) : IM_COL32(255, 85, 85, 255);
                const ImU32 textColor = warning ? IM_COL32(115, 80, 10, 55) : IM_COL32(120, 20, 20, 65);
                editor.AddMarker(lineNumber - 1, numberColor, textColor, line, line);
            }
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
    }

    static void updateShaderDiagnostics(ShaderEditorState& editors, const std::string& status)
    {
        if (!editors.Initialized)
            return;
        editors.Vertex.ClearMarkers();
        editors.Fragment.ClearMarkers();
        if (status.starts_with("Vertex compile error:"))
            applyShaderDiagnostics(editors.Vertex, status);
        else if (status.starts_with("Fragment compile error:"))
            applyShaderDiagnostics(editors.Fragment, status);
    }

    static bool compileShaders(ShaderFramebuffer& framebuffer, ShaderEditorState& editors, const std::array<char, ShaderSourceCapacity>& vertexSource, const std::array<char, ShaderSourceCapacity>& fragmentSource)
    {
        const bool result = framebuffer.compile(vertexSource.data(), fragmentSource.data());
        updateShaderDiagnostics(editors, framebuffer.status());
        return result;
    }

    struct ShaderTransitionState
    {
        ShaderFramebuffer Previous;
        std::array<Color32, MatrixSize> Frame{};
        bool Active = false;
        double StartedAt = 0.0;
        float Duration = 0.0f;

        void cancel() noexcept
        {
            Active = false;
            StartedAt = 0.0;
            Duration = 0.0f;
            Previous.shutdown();
        }
    };

    static Color32 lerpColorLinear(const Color32 from, const Color32 to, const float amount) noexcept
    {
        const float t = std::clamp(amount, 0.0f, 1.0f);
        const auto mix = [t](const std::uint8_t a, const std::uint8_t b) { return static_cast<std::uint8_t>(std::lround(static_cast<float>(a) + (static_cast<float>(b) - a) * t)); };
        return {mix(from.R, to.R), mix(from.G, to.G), mix(from.B, to.B)};
    }

    static bool switchShaderPreset(ShaderFramebuffer& framebuffer, ShaderTransitionState& transition, ShaderEditorState& editors, std::array<char, ShaderSourceCapacity>& vertexSource, std::array<char, ShaderSourceCapacity>& fragmentSource, VisualizerSettings& settings, const int presetIndex, const double now, const float transitionSeconds, const bool persist = true)
    {
        if (presetIndex <= 0 || presetIndex > static_cast<int>(ShaderPresets.size())) return false;
        const auto& preset = ShaderPresets[static_cast<std::size_t>(presetIndex - 1)];
        if (std::string_view(fragmentSource.data()) == preset.FragmentSource)
        {
            settings.ShaderPresetIndex = presetIndex;
            settings.ShaderId = preset.Id;
            return true;
        }

        const std::array<char, ShaderSourceCapacity> previousSource = fragmentSource;
        transition.cancel();
        const float duration = std::clamp(transitionSeconds, 0.0f, 10.0f);
        if (duration > 0.0f && framebuffer.ready())
        {
            framebuffer.snapshotMaterialValues();
            if (transition.Previous.initialize(settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight) && transition.Previous.compile(vertexSource.data(), previousSource.data()))
            {
                transition.Active = true;
                transition.StartedAt = now;
                transition.Duration = duration;
            }
            else
                transition.cancel();
        }

        setShaderSource(fragmentSource, preset.FragmentSource);
        if (!compileShaders(framebuffer, editors, vertexSource, fragmentSource))
        {
            fragmentSource = previousSource;
            transition.cancel();
            return false;
        }

        settings.ShaderPresetIndex = presetIndex;
        settings.ShaderId = preset.Id;
        settings.BaseColorMode = 2;
        if (editors.Initialized) editors.Fragment.SetText(fragmentSource.data());
        if (persist) saveShaderSources(vertexSource, fragmentSource);
        return true;
    }

    static bool switchShaderId(ShaderFramebuffer& framebuffer, ShaderTransitionState& transition, ShaderEditorState& editors, std::array<char, ShaderSourceCapacity>& vertexSource, std::array<char, ShaderSourceCapacity>& fragmentSource, VisualizerSettings& settings, const std::string_view shaderId, const double now, const float transitionSeconds, const bool persist = true)
    {
        const int index = shaderPresetIndexById(shaderId);
        return index > 0 && switchShaderPreset(framebuffer, transition, editors, vertexSource, fragmentSource, settings, index, now, transitionSeconds, persist);
    }

    struct PacketBuffer
    {
        alignas(4) std::array<std::byte, 512> Data{};
        std::size_t Size = 0;
    };

    template<typename T>
    static PacketBuffer makePacket(const PacketType type, const T& payload)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        PacketBuffer packet{};
        packet.Size = sizeof(PacketHeader) + sizeof(T);
        auto* header = std::construct_at(reinterpret_cast<PacketHeader*>(packet.Data.data()), 1, type, PacketDirection::HostToDevice, 0, sizeof(T));
        std::memcpy(packet.Data.data() + sizeof(PacketHeader), &payload, sizeof(T));
        return packet;
    }

    static PacketBuffer makePacket(const PacketType type)
    {
        PacketBuffer packet{};
        packet.Size = sizeof(PacketHeader);
        std::construct_at(reinterpret_cast<PacketHeader*>(packet.Data.data()), 1, type, PacketDirection::HostToDevice, 0, 0);
        return packet;
    }

    struct USBStatsSnapshot
    {
        std::uint64_t TxBytes = 0;
        std::uint64_t RxBytes = 0;
        std::uint64_t TxTransfers = 0;
        std::uint64_t RxTransfers = 0;
        std::uint64_t TxErrors = 0;
        std::uint64_t RxErrors = 0;
        std::uint64_t Connects = 0;
        std::uint64_t Disconnects = 0;
        double LastTxMilliseconds = 0.0;
        double LastRxMilliseconds = 0.0;
    };

    class RawUSB
    {
    public:
        using PacketHandler = std::function<void(const PacketHeader&)>;
        using PacketObserver = std::function<void(bool, const PacketHeader&, std::span<const std::byte>)>;

        ~RawUSB()
        {
            shutdown();
        }

        bool initialize() noexcept
        {
            if (_context)
                return true;
            const int result = libusb_init(&_context);
            _lastError.store(result, std::memory_order_release);
            return result == LIBUSB_SUCCESS;
        }

        bool connect() noexcept
        {
            if (_connected.load(std::memory_order_acquire))
                return true;
            if (!_context && !initialize())
                return false;
            if (_handle)
                disconnect();

            _handle = libusb_open_device_with_vid_pid(_context, VendorId, ProductId);
            if (!_handle)
            {
                _lastError.store(LIBUSB_ERROR_NO_DEVICE, std::memory_order_release);
                return false;
            }

            _deviceName.clear();
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(libusb_get_device(_handle), &descriptor) == LIBUSB_SUCCESS && descriptor.iProduct != 0)
            {
                std::array<unsigned char, 256> product{};
                const int length = libusb_get_string_descriptor_ascii(_handle, descriptor.iProduct, product.data(), static_cast<int>(product.size() - 1));
                if (length > 0)
                    _deviceName.assign(reinterpret_cast<const char*>(product.data()), static_cast<std::size_t>(length));
            }
            if (_deviceName.empty())
                _deviceName = "Quartz K552X";

            const int detachResult = libusb_set_auto_detach_kernel_driver(_handle, 1);
            if (detachResult != LIBUSB_SUCCESS && detachResult != LIBUSB_ERROR_NOT_SUPPORTED)
            {
                _lastError.store(detachResult, std::memory_order_release);
                libusb_close(_handle);
                _handle = nullptr;
                return false;
            }

            const int claimResult = libusb_claim_interface(_handle, RPCInterface);
            if (claimResult != LIBUSB_SUCCESS)
            {
                _lastError.store(claimResult, std::memory_order_release);
                libusb_close(_handle);
                _handle = nullptr;
                return false;
            }

            _running.store(true, std::memory_order_release);
            _connected.store(true, std::memory_order_release);
            _lastError.store(LIBUSB_SUCCESS, std::memory_order_release);
            _connects.fetch_add(1, std::memory_order_relaxed);
            try
            {
                _receiveThread = std::thread(&RawUSB::_receiveLoop, this);
            }
            catch (...)
            {
                _running.store(false, std::memory_order_release);
                _connected.store(false, std::memory_order_release);
                libusb_release_interface(_handle, RPCInterface);
                libusb_close(_handle);
                _handle = nullptr;
                _lastError.store(LIBUSB_ERROR_OTHER, std::memory_order_release);
                return false;
            }
            return true;
        }

        void disconnect() noexcept
        {
            const bool hadDevice = _handle != nullptr || _connected.load(std::memory_order_acquire);
            _running.store(false, std::memory_order_release);
            _connected.store(false, std::memory_order_release);
            if (_receiveThread.joinable())
                _receiveThread.join();
            if (_handle)
            {
                libusb_release_interface(_handle, RPCInterface);
                libusb_close(_handle);
                _handle = nullptr;
            }
            _deviceName.clear();
            _rxAssembly.clear();
            if (hadDevice) _disconnects.fetch_add(1, std::memory_order_relaxed);
        }

        void shutdown() noexcept
        {
            disconnect();
            if (_context)
            {
                libusb_exit(_context);
                _context = nullptr;
            }
        }

        bool isConnected() const noexcept
        {
            return _connected.load(std::memory_order_acquire);
        }

        int lastError() const noexcept
        {
            return _lastError.load(std::memory_order_acquire);
        }

        const std::string& deviceName() const noexcept
        {
            return _deviceName;
        }

        void setPacketHandler(PacketHandler handler)
        {
            _packetHandler = std::move(handler);
        }

        void setPacketObserver(PacketObserver observer)
        {
            _packetObserver = std::move(observer);
        }

        USBStatsSnapshot stats() const noexcept
        {
            return {
                _txBytes.load(std::memory_order_relaxed),
                _rxBytes.load(std::memory_order_relaxed),
                _txTransfers.load(std::memory_order_relaxed),
                _rxTransfers.load(std::memory_order_relaxed),
                _txErrors.load(std::memory_order_relaxed),
                _rxErrors.load(std::memory_order_relaxed),
                _connects.load(std::memory_order_relaxed),
                _disconnects.load(std::memory_order_relaxed),
                _lastTxMicroseconds.load(std::memory_order_relaxed) / 1000.0,
                _lastRxMicroseconds.load(std::memory_order_relaxed) / 1000.0
            };
        }

        bool send(const PacketBuffer& packet) noexcept
        {
            if (!_handle || !_connected.load(std::memory_order_acquire) || packet.Size == 0 || packet.Size > packet.Data.size())
                return false;
            int transferred = 0;
            const auto transferStart = std::chrono::steady_clock::now();
            const int result = libusb_bulk_transfer(_handle, RequestEndpoint, reinterpret_cast<unsigned char*>(const_cast<std::byte*>(packet.Data.data())), static_cast<int>(packet.Size), &transferred, 20);
            const auto transferEnd = std::chrono::steady_clock::now();
            _lastTxMicroseconds.store(std::chrono::duration<double, std::micro>(transferEnd - transferStart).count(), std::memory_order_relaxed);
            _txTransfers.fetch_add(1, std::memory_order_relaxed);
            if (transferred > 0) _txBytes.fetch_add(static_cast<std::uint64_t>(transferred), std::memory_order_relaxed);
            if (result != LIBUSB_SUCCESS || transferred != static_cast<int>(packet.Size)) _txErrors.fetch_add(1, std::memory_order_relaxed);
            _lastError.store(result, std::memory_order_release);
            if (result == LIBUSB_ERROR_NO_DEVICE)
            {
                _running.store(false, std::memory_order_release);
                _connected.store(false, std::memory_order_release);
            }
            const bool success = result == LIBUSB_SUCCESS && transferred == static_cast<int>(packet.Size);
            if (success && _packetObserver)
            {
                const std::span<const std::byte> bytes(packet.Data.data(), packet.Size);
                if (const auto* header = PacketHeader::asPtr(bytes))
                    _packetObserver(true, *header, bytes);
            }
            return success;
        }

    private:
        static constexpr int RPCInterface = RPCInterfaceNumber;
        static constexpr unsigned char RequestEndpoint = RPCOutEndpoint;
        static constexpr unsigned char ResponseEndpoint = RPCInEndpoint;

        void _receiveLoop() noexcept
        {
            std::array<unsigned char, 512> buffer{};
            while (_running.load(std::memory_order_acquire))
            {
                int transferred = 0;
                const auto transferStart = std::chrono::steady_clock::now();
                const int result = libusb_bulk_transfer(_handle, ResponseEndpoint, buffer.data(), static_cast<int>(buffer.size()), &transferred, 100);
                const auto transferEnd = std::chrono::steady_clock::now();
                if (result == LIBUSB_ERROR_TIMEOUT)
                    continue;
                _lastRxMicroseconds.store(std::chrono::duration<double, std::micro>(transferEnd - transferStart).count(), std::memory_order_relaxed);
                _rxTransfers.fetch_add(1, std::memory_order_relaxed);
                if (transferred > 0) _rxBytes.fetch_add(static_cast<std::uint64_t>(transferred), std::memory_order_relaxed);
                _lastError.store(result, std::memory_order_release);
                if (result != LIBUSB_SUCCESS)
                {
                    _rxErrors.fetch_add(1, std::memory_order_relaxed);
                    if (result == LIBUSB_ERROR_NO_DEVICE)
                        _connected.store(false, std::memory_order_release);
                    break;
                }
                if (transferred > 0)
                    _consumeReceivedBytes(reinterpret_cast<const std::byte*>(buffer.data()), static_cast<std::size_t>(transferred));
            }
            _running.store(false, std::memory_order_release);
        }

        void _consumeReceivedBytes(const std::byte* data, const std::size_t size)
        {
            _rxAssembly.insert(_rxAssembly.end(), data, data + size);
            while (_rxAssembly.size() >= sizeof(PacketHeader))
            {
                std::uint32_t magic = 0;
                std::memcpy(&magic, _rxAssembly.data(), sizeof(magic));
                if (magic != PacketHeader::MAGIC_NUMBER)
                {
                    _rxAssembly.erase(_rxAssembly.begin());
                    continue;
                }

                PacketBuffer packet{};
                std::memcpy(packet.Data.data(), _rxAssembly.data(), sizeof(PacketHeader));
                const auto* header = reinterpret_cast<const PacketHeader*>(packet.Data.data());
                const std::size_t packetSize = sizeof(PacketHeader) + header->PayloadLength;
                if (packetSize > packet.Data.size())
                {
                    _rxAssembly.clear();
                    return;
                }
                if (_rxAssembly.size() < packetSize)
                    return;

                packet.Size = packetSize;
                std::memcpy(packet.Data.data(), _rxAssembly.data(), packetSize);
                header = PacketHeader::asPtr(std::span<const std::byte>(packet.Data.data(), packet.Size));
                if (header && _packetObserver)
                    _packetObserver(false, *header, std::span<const std::byte>(packet.Data.data(), packet.Size));
                if (header && _packetHandler)
                    _packetHandler(*header);
                _rxAssembly.erase(_rxAssembly.begin(), _rxAssembly.begin() + static_cast<std::ptrdiff_t>(packetSize));
            }
        }

        libusb_context* _context = nullptr;
        libusb_device_handle* _handle = nullptr;
        std::thread _receiveThread;
        std::atomic_bool _running = false;
        std::atomic_bool _connected = false;
        std::atomic_int _lastError = LIBUSB_SUCCESS;
        std::atomic_uint64_t _txBytes{0}, _rxBytes{0}, _txTransfers{0}, _rxTransfers{0}, _txErrors{0}, _rxErrors{0}, _connects{0}, _disconnects{0};
        std::atomic<double> _lastTxMicroseconds{0.0}, _lastRxMicroseconds{0.0};
        PacketHandler _packetHandler;
        PacketObserver _packetObserver;
        std::vector<std::byte> _rxAssembly;
        std::string _deviceName;
    };

    struct AudioLevelSnapshot
    {
        float Rms = 0.0f;
        float Peak = 0.0f;
    };

    class AudioSpectrum
    {
    public:
        static constexpr int SampleRate = 48000;
        static constexpr int Channels = 2;
        static constexpr int BytesPerSample = sizeof(float);
        static constexpr int BytesPerFrame = BytesPerSample * Channels;

        ~AudioSpectrum() { stop(); }

        bool start(const std::string& source)
        {
            stop();
            int pipeFds[2];
            if (pipe(pipeFds) != 0)
            {
                _error = "pipe() failed";
                return false;
            }
            const std::string deviceArg = "--device=" + source;
            const pid_t pid = fork();
            if (pid < 0)
            {
                close(pipeFds[0]);
                close(pipeFds[1]);
                _error = "fork() failed";
                return false;
            }
            if (pid == 0)
            {
                dup2(pipeFds[1], STDOUT_FILENO);
                close(pipeFds[0]);
                close(pipeFds[1]);
                setenv("PULSE_LATENCY_MSEC", "5", 1);
                execlp("parec", "parec", deviceArg.c_str(), "--format=float32le", "--rate=48000", "--channels=2", "--raw", static_cast<char*>(nullptr));
                _exit(127);
            }
            close(pipeFds[1]);
            _readFd = pipeFds[0];
            _pid = pid;
            _source = source;
            _error.clear();
            _running.store(true, std::memory_order_release);
            _thread = std::thread(&AudioSpectrum::readLoop, this);
            return true;
        }

        void stop()
        {
            if (_pid > 0)
                kill(_pid, SIGTERM);
            _running.store(false, std::memory_order_release);
            if (_thread.joinable())
                _thread.join();
            if (_readFd >= 0)
            {
                close(_readFd);
                _readFd = -1;
            }
            if (_pid > 0)
            {
                int status = 0;
                waitpid(_pid, &status, 0);
                _pid = -1;
            }
        }

        bool isRunning() const noexcept { return _running.load(std::memory_order_acquire); }
        const std::string& source() const noexcept { return _source; }
        const std::string& error() const noexcept { return _error; }

        AudioLevelSnapshot levelSnapshot()
        {
            AudioLevelSnapshot result{};
            std::lock_guard lock(_sampleMutex);
            const int available = std::min(_sampleCount, static_cast<int>(FFTSize));
            if (available <= 0) return result;
            const int start = (_writePosition - available + static_cast<int>(FFTSize)) % static_cast<int>(FFTSize);
            double sumSquares = 0.0;
            for (int i = 0; i < available; ++i)
            {
                const float sample = _samples[(start + i) % static_cast<int>(FFTSize)];
                sumSquares += static_cast<double>(sample) * sample;
                result.Peak = std::max(result.Peak, std::abs(sample));
            }
            result.Rms = static_cast<float>(std::sqrt(sumSquares / available));
            return result;
        }

        void getBands(const std::span<float> bands, const float minFrequency, const float maxFrequency, const float minDb, const float maxDb)
        {
            std::array<float, FFTSize> samples{};
            {
                std::lock_guard lock(_sampleMutex);
                const int available = std::min(_sampleCount, static_cast<int>(FFTSize));
                const int padding = static_cast<int>(FFTSize) - available;
                const int start = (_writePosition - available + static_cast<int>(FFTSize)) % static_cast<int>(FFTSize);
                for (int i = 0; i < available; ++i)
                    samples[padding + i] = _samples[(start + i) % static_cast<int>(FFTSize)];
            }

            std::array<std::complex<float>, FFTSize> fft{};
            for (std::size_t i = 0; i < FFTSize; ++i)
            {
                const float window = 0.5f - 0.5f * std::cos(2.0f * Pi * static_cast<float>(i) / static_cast<float>(FFTSize - 1));
                fft[i] = {samples[i] * window, 0.0f};
            }
            transform(fft);

            const float safeMinFrequency = std::clamp(minFrequency, 1.0f, SampleRate * 0.49f);
            const float safeMaxFrequency = std::clamp(maxFrequency, safeMinFrequency + 1.0f, SampleRate * 0.49f);
            const float safeMinDb = std::min(minDb, maxDb - 0.1f);
            for (std::size_t band = 0; band < bands.size(); ++band)
            {
                const float normalizedLow = static_cast<float>(band) / static_cast<float>(bands.size());
                const float normalizedHigh = static_cast<float>(band + 1) / static_cast<float>(bands.size());
                const float ratio = safeMaxFrequency / safeMinFrequency;
                const float lowFrequency = safeMinFrequency * std::pow(ratio, normalizedLow);
                const float highFrequency = safeMinFrequency * std::pow(ratio, normalizedHigh);
                const int lowBin = std::clamp(static_cast<int>(std::floor(lowFrequency * FFTSize / SampleRate)), 1, static_cast<int>(FFTSize / 2 - 1));
                const int highBin = std::clamp(static_cast<int>(std::ceil(highFrequency * FFTSize / SampleRate)), lowBin, static_cast<int>(FFTSize / 2 - 1));
                float maxMagnitude = 0.0f;
                for (int bin = lowBin; bin <= highBin; ++bin)
                    maxMagnitude = std::max(maxMagnitude, std::abs(fft[bin]) * 4.0f / static_cast<float>(FFTSize));
                const float db = 20.0f * std::log10(maxMagnitude + 1e-9f);
                bands[band] = std::clamp((db - safeMinDb) / (maxDb - safeMinDb), 0.0f, 1.0f);
            }
        }

    private:
        static void transform(std::array<std::complex<float>, FFTSize>& values)
        {
            for (std::size_t i = 1, j = 0; i < FFTSize; ++i)
            {
                std::size_t bit = FFTSize >> 1;
                while ((j & bit) != 0)
                {
                    j ^= bit;
                    bit >>= 1;
                }
                j ^= bit;
                if (i < j)
                    std::swap(values[i], values[j]);
            }
            for (std::size_t length = 2; length <= FFTSize; length <<= 1)
            {
                const float angle = -2.0f * Pi / static_cast<float>(length);
                const std::complex<float> step(std::cos(angle), std::sin(angle));
                for (std::size_t offset = 0; offset < FFTSize; offset += length)
                {
                    std::complex<float> factor(1.0f, 0.0f);
                    const std::size_t half = length >> 1;
                    for (std::size_t i = 0; i < half; ++i)
                    {
                        const auto even = values[offset + i];
                        const auto odd = values[offset + i + half] * factor;
                        values[offset + i] = even + odd;
                        values[offset + i + half] = even - odd;
                        factor *= step;
                    }
                }
            }
        }

        void readLoop()
        {
            std::array<std::byte, 8192 + BytesPerFrame> buffer{};
            std::size_t carry = 0;
            while (_running.load(std::memory_order_acquire))
            {
                const ssize_t bytesRead = read(_readFd, buffer.data() + carry, buffer.size() - carry);
                if (bytesRead <= 0)
                    break;
                const std::size_t totalBytes = carry + static_cast<std::size_t>(bytesRead);
                const std::size_t frameCount = totalBytes / BytesPerFrame;
                const std::size_t consumedBytes = frameCount * BytesPerFrame;
                {
                    std::lock_guard lock(_sampleMutex);
                    for (std::size_t frame = 0; frame < frameCount; ++frame)
                    {
                        const std::size_t offset = frame * BytesPerFrame;
                        float left;
                        float right;
                        std::memcpy(&left, buffer.data() + offset, sizeof(float));
                        std::memcpy(&right, buffer.data() + offset + sizeof(float), sizeof(float));
                        _samples[_writePosition] = (left + right) * 0.5f;
                        _writePosition = (_writePosition + 1) % static_cast<int>(FFTSize);
                        if (_sampleCount < static_cast<int>(FFTSize))
                            ++_sampleCount;
                    }
                }
                carry = totalBytes - consumedBytes;
                if (carry != 0)
                    std::memmove(buffer.data(), buffer.data() + consumedBytes, carry);
            }
            _running.store(false, std::memory_order_release);
        }

        std::array<float, FFTSize> _samples{};
        std::mutex _sampleMutex;
        std::thread _thread;
        std::atomic_bool _running = false;
        pid_t _pid = -1;
        int _readFd = -1;
        int _writePosition = 0;
        int _sampleCount = 0;
        std::string _source;
        std::string _error;
    };

    struct AutoGainState
    {
        float LongTermRms = 0.0f;
        float Correction = 1.0f;
        float EffectiveGain = 1.62f;
        float SilenceSeconds = 0.0f;
        bool Initialized = false;

        void reset(const VisualizerSettings& settings) noexcept
        {
            LongTermRms = 0.0f;
            Correction = 1.0f;
            EffectiveGain = settings.AutomaticOverallGain ? settings.AutoGainBaseline : settings.OverallGain;
            SilenceSeconds = 0.0f;
            Initialized = false;
        }

        void update(const AudioLevelSnapshot& level, const VisualizerSettings& settings, const float dt) noexcept
        {
            if (!settings.AutomaticOverallGain)
            {
                Correction = 1.0f;
                EffectiveGain = settings.OverallGain;
                SilenceSeconds = 0.0f;
                return;
            }

            const float safeDt = std::clamp(dt, 0.0001f, 0.25f);
            if (level.Rms > settings.AutoGainSilenceGate)
            {
                SilenceSeconds = 0.0f;
                const float loudnessAlpha = 1.0f - std::exp(-settings.AutoGainAdaptation * safeDt);
                if (!Initialized)
                {
                    LongTermRms = level.Rms;
                    Initialized = true;
                }
                else
                    LongTermRms += (level.Rms - LongTermRms) * loudnessAlpha;

                const float desired = std::clamp(settings.AutoGainTargetRms / std::max(LongTermRms, 0.000001f), settings.AutoGainMinCorrection, settings.AutoGainMaxCorrection);
                const float correctionAlpha = 1.0f - std::exp(-std::max(settings.AutoGainAdaptation * 0.55f, 0.02f) * safeDt);
                Correction += (desired - Correction) * correctionAlpha;
            }
            else
            {
                SilenceSeconds += safeDt;
                if (SilenceSeconds > 3.0f)
                {
                    const float returnAlpha = 1.0f - std::exp(-0.12f * safeDt);
                    Correction += (1.0f - Correction) * returnAlpha;
                }
            }
            Correction = std::clamp(Correction, settings.AutoGainMinCorrection, settings.AutoGainMaxCorrection);
            EffectiveGain = settings.AutoGainBaseline * Correction;
        }
    };

    static std::string shellQuote(const std::string_view value)
    {
        std::string result = "'";
        for (const char c : value)
        {
            if (c == '\'')
                result += "'\\''";
            else
                result += c;
        }
        result += '\'';
        return result;
    }

    static std::vector<std::byte> readCommand(const std::string& command, const std::size_t maxBytes = 16 * 1024 * 1024)
    {
        std::vector<std::byte> result;
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe)
            return result;
        std::array<std::byte, 8192> buffer{};
        while (result.size() < maxBytes)
        {
            const std::size_t readSize = std::fread(buffer.data(), 1, std::min(buffer.size(), maxBytes - result.size()), pipe);
            if (readSize == 0)
                break;
            result.insert(result.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(readSize));
        }
        pclose(pipe);
        return result;
    }

    static std::string bytesToString(const std::vector<std::byte>& bytes)
    {
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }


    static std::string trim(std::string value)
    {
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t'))
            value.pop_back();
        std::size_t start = 0;
        while (start < value.size() && (value[start] == '\n' || value[start] == '\r' || value[start] == ' ' || value[start] == '\t'))
            ++start;
        if (start != 0)
            value.erase(0, start);
        return value;
    }

    static bool commandExists(const std::string_view command)
    {
        return !trim(bytesToString(readCommand("command -v " + shellQuote(command) + " 2>/dev/null", 4096))).empty();
    }

    struct AudioSourceInfo
    {
        std::string Name;
        std::string Description;
    };

    static std::vector<AudioSourceInfo> enumerateAudioSources()
    {
        std::vector<AudioSourceInfo> sources;
        if (!commandExists("pactl"))
            return sources;
        const std::string output = bytesToString(readCommand("pactl list sources 2>/dev/null", 512 * 1024));
        AudioSourceInfo current;
        const auto flush = [&]
        {
            if (current.Name.empty())
                return;
            if (current.Description.empty())
                current.Description = current.Name;
            if (std::ranges::none_of(sources, [&](const auto& source) { return source.Name == current.Name; }))
                sources.emplace_back(std::move(current));
            current = {};
        };
        std::size_t start = 0;
        while (start < output.size())
        {
            const std::size_t end = output.find('\n', start);
            const std::string line = trim(std::string(output.substr(start, end == std::string::npos ? output.size() - start : end - start)));
            if (line.starts_with("Source #"))
                flush();
            else if (line.starts_with("Name:"))
                current.Name = trim(line.substr(5));
            else if (line.starts_with("Description:"))
                current.Description = trim(line.substr(12));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        flush();
        std::ranges::sort(sources, {}, &AudioSourceInfo::Description);
        return sources;
    }

    static std::vector<std::string> split(const std::string_view value, const char delimiter)
    {
        std::vector<std::string> result;
        std::size_t start = 0;
        for (;;)
        {
            const std::size_t end = value.find(delimiter, start);
            result.emplace_back(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start));
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
        return result;
    }

    static std::string percentDecode(const std::string_view value)
    {
        std::string result;
        result.reserve(value.size());
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] == '%' && i + 2 < value.size())
            {
                const auto hex = [](const char c) -> int
                {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                const int high = hex(value[i + 1]);
                const int low = hex(value[i + 2]);
                if (high >= 0 && low >= 0)
                {
                    result += static_cast<char>((high << 4) | low);
                    i += 2;
                    continue;
                }
            }
            result += value[i];
        }
        return result;
    }

    class MediaColorProvider
    {
    public:
        ~MediaColorProvider() { stop(); }

        void start()
        {
            if (_running.exchange(true, std::memory_order_acq_rel))
                return;
            _thread = std::thread(&MediaColorProvider::run, this);
        }

        void stop()
        {
            _running.store(false, std::memory_order_release);
            if (_thread.joinable())
                _thread.join();
        }

        void setPollInterval(const float seconds) noexcept { _pollMilliseconds.store(static_cast<int>(std::clamp(seconds, 0.10f, 5.0f) * 1000.0f), std::memory_order_release); }

        std::optional<Color32> targetColor() const noexcept
        {
            const std::int32_t packed = _targetColor.load(std::memory_order_acquire);
            if (packed < 0)
                return std::nullopt;
            return Color32{
                static_cast<std::uint8_t>(packed >> 16),
                static_cast<std::uint8_t>(packed >> 8),
                static_cast<std::uint8_t>(packed)
            };
        }

        bool playing() const noexcept { return _playing.load(std::memory_order_acquire); }

        std::string mediaTitle() const
        {
            std::lock_guard lock(_stateMutex);
            return _mediaTitle;
        }

        std::string status() const
        {
            std::lock_guard lock(_stateMutex);
            return _status;
        }

        static constexpr bool imageDecoderAvailable() noexcept { return QUARTZ_HAS_STB_IMAGE != 0; }

    private:
        struct MediaInfo
        {
            std::string Player;
            std::string Status;
            std::string Title;
            std::string Artist;
            std::string ArtworkUrl;
        };

        struct MediaDiscovery
        {
            std::optional<MediaInfo> Media;
            std::string Status;
        };

        struct Bucket
        {
            std::uint64_t R = 0;
            std::uint64_t G = 0;
            std::uint64_t B = 0;
            std::uint32_t Count = 0;
            float Score = 0.0f;
        };

        static std::vector<std::string> tokenizeBusctl(const std::string_view value)
        {
            std::vector<std::string> tokens;
            std::size_t i = 0;
            while (i < value.size())
            {
                while (i < value.size() && (value[i] == ' ' || value[i] == '\t' || value[i] == '\r' || value[i] == '\n'))
                    ++i;
                if (i >= value.size())
                    break;
                std::string token;
                if (value[i] == '"')
                {
                    ++i;
                    while (i < value.size() && value[i] != '"')
                    {
                        if (value[i] == '\\' && i + 1 < value.size())
                        {
                            ++i;
                            switch (value[i])
                            {
                            case 'n': token += '\n'; break;
                            case 'r': token += '\r'; break;
                            case 't': token += '\t'; break;
                            default: token += value[i]; break;
                            }
                            ++i;
                            continue;
                        }
                        token += value[i++];
                    }
                    if (i < value.size() && value[i] == '"')
                        ++i;
                }
                else
                {
                    const std::size_t start = i;
                    while (i < value.size() && value[i] != ' ' && value[i] != '\t' && value[i] != '\r' && value[i] != '\n')
                        ++i;
                    token.assign(value.substr(start, i - start));
                }
                tokens.emplace_back(std::move(token));
            }
            return tokens;
        }

        static std::vector<std::string> mprisPlayers()
        {
            std::vector<std::string> players;
            const std::string output = bytesToString(readCommand("busctl --user --no-pager --no-legend --full list 2>/dev/null", 256 * 1024));
            std::size_t start = 0;
            while (start < output.size())
            {
                const std::size_t end = output.find('\n', start);
                const std::string line = trim(std::string(output.substr(start, end == std::string::npos ? output.size() - start : end - start)));
                if (!line.empty())
                {
                    const std::size_t space = line.find_first_of(" \t");
                    const std::string name = line.substr(0, space);
                    if (name.starts_with("org.mpris.MediaPlayer2."))
                        players.emplace_back(name);
                }
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            return players;
        }

        static std::vector<std::string> mprisProperty(const std::string& player, const std::string_view property)
        {
            const std::string command = "busctl --user --no-pager get-property " + shellQuote(player) + " /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player " + shellQuote(property) + " 2>/dev/null";
            return tokenizeBusctl(bytesToString(readCommand(command, 256 * 1024)));
        }

        static std::string scalarProperty(const std::vector<std::string>& tokens)
        {
            return tokens.size() >= 2 ? tokens[1] : std::string{};
        }

        static std::string metadataString(const std::vector<std::string>& tokens, const std::string_view key)
        {
            for (std::size_t i = 0; i + 2 < tokens.size(); ++i)
            {
                if (tokens[i] != key)
                    continue;
                if (tokens[i + 1] == "s" || tokens[i + 1] == "o")
                    return tokens[i + 2];
                if (tokens[i + 1] == "as" && i + 3 < tokens.size())
                    return tokens[i + 3];
                return {};
            }
            return {};
        }

        static std::string youtubeThumbnail(const std::string_view url)
        {
            std::string videoId;
            if (const std::size_t shortPos = url.find("youtu.be/"); shortPos != std::string_view::npos)
            {
                const std::size_t start = shortPos + 9;
                const std::size_t end = url.find_first_of("?#&/", start);
                videoId = std::string(url.substr(start, end == std::string_view::npos ? url.size() - start : end - start));
            }
            else if (url.find("youtube.com") != std::string_view::npos)
            {
                const std::size_t v = url.find("v=");
                if (v != std::string_view::npos)
                {
                    const std::size_t start = v + 2;
                    const std::size_t end = url.find_first_of("&#", start);
                    videoId = std::string(url.substr(start, end == std::string_view::npos ? url.size() - start : end - start));
                }
            }
            return videoId.empty() ? std::string{} : "https://i.ytimg.com/vi/" + videoId + "/hqdefault.jpg";
        }

        static MediaDiscovery currentMedia()
        {
            if (!commandExists("busctl"))
                return {.Status = "busctl not found"};
            const auto players = mprisPlayers();
            if (players.empty())
                return {.Status = "No MPRIS players on user D-Bus"};
            std::optional<MediaInfo> fallback;
            for (const auto& player : players)
            {
                MediaInfo media;
                media.Player = player;
                media.Status = scalarProperty(mprisProperty(player, "PlaybackStatus"));
                const auto metadata = mprisProperty(player, "Metadata");
                media.Title = metadataString(metadata, "xesam:title");
                media.Artist = metadataString(metadata, "xesam:artist");
                media.ArtworkUrl = metadataString(metadata, "mpris:artUrl");
                if (media.ArtworkUrl.empty())
                    media.ArtworkUrl = youtubeThumbnail(metadataString(metadata, "xesam:url"));
                if (!fallback)
                    fallback = media;
                if (media.Status == "Playing")
                    return {.Media = std::move(media), .Status = "MPRIS active"};
            }
            if (fallback)
                return {.Media = std::move(fallback), .Status = "MPRIS active"};
            return {.Status = "MPRIS players found, properties unavailable"};
        }

        static std::vector<std::byte> readArtwork(const std::string& url)
        {
            if (url.starts_with("file://"))
            {
                const std::filesystem::path path(percentDecode(std::string_view(url).substr(7)));
                std::ifstream file(path, std::ios::binary | std::ios::ate);
                if (!file)
                    return {};
                const auto size = file.tellg();
                if (size <= 0 || size > 16 * 1024 * 1024)
                    return {};
                std::vector<std::byte> data(static_cast<std::size_t>(size));
                file.seekg(0);
                file.read(reinterpret_cast<char*>(data.data()), size);
                return file ? data : std::vector<std::byte>{};
            }
            if (url.starts_with("http://") || url.starts_with("https://"))
                return readCommand("curl -fsSL --max-time 5 -- " + shellQuote(url) + " 2>/dev/null");
            return {};
        }

        static std::optional<Color32> dominantColor(const std::span<const std::byte> encoded)
        {
#if QUARTZ_HAS_STB_IMAGE
            if (encoded.empty() || encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                return std::nullopt;
            int width;
            int height;
            int channels;
            stbi_uc* image = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(encoded.data()), static_cast<int>(encoded.size()), &width, &height, &channels, 3);
            if (!image || width <= 0 || height <= 0)
            {
                if (image)
                    stbi_image_free(image);
                return std::nullopt;
            }
            std::unordered_map<int, Bucket> buckets;
            const int stepX = std::max(1, width / 32);
            const int stepY = std::max(1, height / 32);
            for (int y = 0; y < height; y += stepY)
            {
                for (int x = 0; x < width; x += stepX)
                {
                    const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 3;
                    const std::uint8_t r = image[offset + 0];
                    const std::uint8_t g = image[offset + 1];
                    const std::uint8_t b = image[offset + 2];
                    const float maximum = std::max({r, g, b}) / 255.0f;
                    const float minimum = std::min({r, g, b}) / 255.0f;
                    const float saturation = maximum > 0.0f ? (maximum - minimum) / maximum : 0.0f;
                    if (maximum < 0.06f || (maximum > 0.95f && saturation < 0.08f))
                        continue;
                    const int key = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
                    auto& bucket = buckets[key];
                    bucket.R += r;
                    bucket.G += g;
                    bucket.B += b;
                    ++bucket.Count;
                    bucket.Score += (0.15f + saturation * 0.85f) * (0.40f + maximum * 0.60f);
                }
            }
            stbi_image_free(image);
            const Bucket* dominant = nullptr;
            for (const auto& [_, bucket] : buckets)
            {
                if (!dominant || bucket.Score > dominant->Score)
                    dominant = &bucket;
            }
            if (!dominant || dominant->Count == 0)
                return std::nullopt;
            return Color32{
                static_cast<std::uint8_t>(dominant->R / dominant->Count),
                static_cast<std::uint8_t>(dominant->G / dominant->Count),
                static_cast<std::uint8_t>(dominant->B / dominant->Count)
            };
#else
            (void)encoded;
            return std::nullopt;
#endif
        }

        static Color32 enhanceColor(const Color32 color) noexcept
        {
            auto hsv = rgbToHsv(color);
            hsv.S = std::clamp(hsv.S * 1.35f, 0.65f, 1.0f);
            return hsvToRgb(hsv.H, hsv.S, 1.0f);
        }

        void setStatus(std::string status, std::string title = {})
        {
            std::lock_guard lock(_stateMutex);
            _status = std::move(status);
            _mediaTitle = std::move(title);
        }

        void refresh()
        {
            const auto discovery = currentMedia();
            if (!discovery.Media)
            {
                _playing.store(false, std::memory_order_release);
                _lastMediaKey.clear();
                _targetColor.store(-1, std::memory_order_release);
                setStatus(discovery.Status);
                return;
            }
            const auto& media = *discovery.Media;
            if (media.Status != "Playing")
            {
                _playing.store(false, std::memory_order_release);
                _lastMediaKey.clear();
                _targetColor.store(-1, std::memory_order_release);
                setStatus("MPRIS " + (media.Status.empty() ? std::string("not playing") : media.Status) + " (" + media.Player + ")", media.Title);
                return;
            }
            _playing.store(true, std::memory_order_release);
            if (media.ArtworkUrl.empty())
            {
                _lastMediaKey.clear();
                _targetColor.store(-1, std::memory_order_release);
                setStatus("MPRIS active, no artwork (" + media.Player + ")", media.Title);
                return;
            }
            const std::string mediaKey = media.Player + '\n' + media.Title + '\n' + media.Artist + '\n' + media.ArtworkUrl;
            if (mediaKey == _lastMediaKey)
                return;
#if !QUARTZ_HAS_STB_IMAGE
            setStatus("stb_image.h not available", media.Title);
            return;
#else
            const auto artwork = readArtwork(media.ArtworkUrl);
            const auto dominant = dominantColor(artwork);
            if (!dominant)
            {
                setStatus("Artwork decode failed (" + media.Player + ")", media.Title);
                return;
            }
            const auto target = enhanceColor(*dominant);
            _lastMediaKey = mediaKey;
            _targetColor.store((static_cast<std::int32_t>(target.R) << 16) | (static_cast<std::int32_t>(target.G) << 8) | target.B, std::memory_order_release);
            setStatus("Artwork color active (" + media.Player + ")", media.Title);
#endif
        }

        void run()
        {
            while (_running.load(std::memory_order_acquire))
            {
                refresh();
                const int milliseconds = _pollMilliseconds.load(std::memory_order_acquire);
                int slept = 0;
                while (_running.load(std::memory_order_acquire) && slept < milliseconds)
                {
                    constexpr int Slice = 50;
                    std::this_thread::sleep_for(std::chrono::milliseconds(Slice));
                    slept += Slice;
                }
            }
        }

        std::thread _thread;
        std::atomic_bool _running = false;
        std::atomic_int _pollMilliseconds = 500;
        std::atomic<std::int32_t> _targetColor = -1;
        std::atomic_bool _playing = false;
        mutable std::mutex _stateMutex;
        std::string _lastMediaKey;
        std::string _mediaTitle;
        std::string _status = "Starting";
    };
