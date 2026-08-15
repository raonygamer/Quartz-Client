#pragma once
#include "quartz/client/Functions.hpp"
#include "quartz/client/settings/VisualizerSettings.hpp"

namespace quartz::client
{
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

    struct AudioSourceInfo
    {
        std::string Name;
        std::string Description;
    };

}
