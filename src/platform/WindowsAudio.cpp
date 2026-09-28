#ifdef _WIN32
#include "quartz/client/Model.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

namespace quartz::client
{
    namespace
    {
        struct ComApartment
        {
            HRESULT Result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ~ComApartment() { if (SUCCEEDED(Result)) CoUninitialize(); }
        };
    }

    std::vector<AudioSourceInfo> enumerateAudioSources()
    {
        std::vector<AudioSourceInfo> result{{"default", "Default output (loopback)"}, {"default-input", "Default microphone / input"}};
        ComApartment apartment;
        if (FAILED(apartment.Result) && apartment.Result != RPC_E_CHANGED_MODE) return result;
        win::ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.put())))) return result;
        for (auto flow : {eRender, eCapture})
        {
            win::ComPtr<IMMDeviceCollection> devices;
            if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, devices.put()))) continue;
            UINT count = 0; devices->GetCount(&count);
            for (UINT i = 0; i < count; ++i)
            {
                win::ComPtr<IMMDevice> device;
                if (FAILED(devices->Item(i,device.put()))) continue;
                LPWSTR id = nullptr;
                if (FAILED(device->GetId(&id))) continue;
                AudioSourceInfo info; info.Name = (flow == eRender ? "loopback:" : "capture:") + win::utf8(id); CoTaskMemFree(id);
                win::ComPtr<IPropertyStore> properties;
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ,properties.put())))
                {
                    PROPVARIANT value{};
                    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName,&value)) && value.vt == VT_LPWSTR) info.Description = win::utf8(value.pwszVal);
                    PropVariantClear(&value);
                }
                if (info.Description.empty()) info.Description = info.Name;
                info.Description += flow == eRender ? " (loopback)" : " (input)";
                result.push_back(std::move(info));
            }
        }
        return result;
    }

    bool AudioSpectrum::start(const std::string& source)
    {
        stop();
        _source = source;
        { std::lock_guard lock(_sampleMutex); _samples.fill(0); _sampleCount = _writePosition = 0; }
        { std::lock_guard lock(_errorMutex); _error.clear(); }
        { std::lock_guard lock(_startMutex); _startFinished = false; }
        _running.store(true);
        _thread = std::thread(&AudioSpectrum::readLoop,this);
        std::unique_lock lock(_startMutex);
        _startCondition.wait(lock,[this] { return _startFinished; });
        return _running.load();
    }
    void AudioSpectrum::stop()
    {
        _running.store(false);
        if (_thread.joinable()) _thread.join();
    }

    void AudioSpectrum::readLoop()
    {
        const auto started = [this]
        {
            { std::lock_guard lock(_startMutex); _startFinished = true; }
            _startCondition.notify_one();
        };
        const auto fail = [this,&started](const char* operation,HRESULT result)
        {
            { std::lock_guard lock(_errorMutex); _error = win::error(operation,static_cast<DWORD>(result)); }
            _running.store(false); started();
        };
        ComApartment apartment;
        if (FAILED(apartment.Result)) { fail("CoInitializeEx",apartment.Result); return; }
        win::ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(enumerator.put()));
        if (FAILED(hr)) { fail("audio device enumerator",hr); return; }
        const bool capture = _source.starts_with("capture:") || _source == "default-input";
        const bool explicitDevice = _source.starts_with("capture:") || _source.starts_with("loopback:");
        win::ComPtr<IMMDevice> device;
        if (explicitDevice) hr = enumerator->GetDevice(win::wide(_source.substr(_source.find(':') + 1)).c_str(),device.put());
        else hr = enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender,eMultimedia,device.put());
        if (FAILED(hr)) { fail("selecting audio endpoint",hr); return; }
        win::ComPtr<IAudioClient> client;
        hr = device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.put()));
        if (FAILED(hr)) { fail("activating WASAPI",hr); return; }
        WAVEFORMATEX format{}; format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = Channels; format.nSamplesPerSec = SampleRate; format.wBitsPerSample = 32;
        format.nBlockAlign = BytesPerFrame; format.nAvgBytesPerSec = SampleRate * BytesPerFrame;
        DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (!capture) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,200000,0,&format,nullptr);
        if (FAILED(hr)) { fail("initializing WASAPI capture",hr); return; }
        win::ComPtr<IAudioCaptureClient> reader;
        hr = client->GetService(__uuidof(IAudioCaptureClient),reinterpret_cast<void**>(reader.put()));
        if (FAILED(hr)) { fail("WASAPI capture service",hr); return; }
        hr = client->Start();
        if (FAILED(hr)) { fail("starting WASAPI",hr); return; }
        started();
        auto lastPacket = std::chrono::steady_clock::now();
        while (_running.load())
        {
            UINT32 available = 0;
            hr = reader->GetNextPacketSize(&available);
            if (FAILED(hr)) { fail("WASAPI endpoint disconnected",hr); break; }
            if (!available)
            {
                // Loopback produces no packets for a silent render endpoint.
                if (std::chrono::steady_clock::now() - lastPacket > std::chrono::milliseconds(50))
                { std::lock_guard lock(_sampleMutex); _samples.fill(0); _sampleCount = static_cast<int>(FFTSize); }
                std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue;
            }
            BYTE* data = nullptr; UINT32 frames = 0; DWORD bufferFlags = 0;
            hr = reader->GetBuffer(&data,&frames,&bufferFlags,nullptr,nullptr);
            if (FAILED(hr)) { fail("reading WASAPI samples",hr); break; }
            {
                std::lock_guard lock(_sampleMutex);
                for (UINT32 frame = 0; frame < frames; ++frame)
                {
                    float sample = 0;
                    if (!(bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) && data)
                    {
                        float stereo[2]; std::memcpy(stereo,data + frame * BytesPerFrame,BytesPerFrame);
                        sample = (stereo[0] + stereo[1]) * 0.5f;
                        if (!std::isfinite(sample)) sample = 0;
                    }
                    _samples[_writePosition] = sample;
                    _writePosition = (_writePosition + 1) % static_cast<int>(FFTSize);
                    _sampleCount = std::min(_sampleCount + 1,static_cast<int>(FFTSize));
                }
            }
            hr = reader->ReleaseBuffer(frames);
            if (FAILED(hr)) { fail("releasing WASAPI buffer",hr); break; }
            lastPacket = std::chrono::steady_clock::now();
        }
        client->Stop(); _running.store(false);
    }
}
#endif
