#ifdef _WIN32
#include "quartz/client/platform/Windows.hpp"
#include "quartz/client/platform/WindowsMedia.hpp"
#include <roapi.h>
#include <winstring.h>
#include <asyncinfo.h>
#include <shcore.h>
#include <chrono>
#include <thread>

namespace quartz::client
{
    namespace
    {
        // ABI prefixes for Windows.Media.Control (Windows 10 1809+). MinGW does
        // not yet ship this header. Method order/IIDs follow Microsoft's metadata:
        // https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/winrt/windows.media.control.h
        template<class T> struct Async : IInspectable
        {
            virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown**) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetResults(T**) = 0;
        };
        struct StreamReference : IInspectable { virtual HRESULT STDMETHODCALLTYPE OpenReadAsync(Async<IUnknown>**) = 0; };
        struct Properties : IInspectable
        {
            virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_Subtitle(HSTRING*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_AlbumTitle(HSTRING*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_TrackNumber(INT32*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_Genres(IInspectable**) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_AlbumTrackCount(INT32*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_PlaybackType(IInspectable**) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_Thumbnail(StreamReference**) = 0;
        };
        struct Playback : IInspectable
        {
            virtual HRESULT STDMETHODCALLTYPE get_Controls(IInspectable**) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(INT32*) = 0;
        };
        struct Session : IInspectable
        {
            virtual HRESULT STDMETHODCALLTYPE get_SourceAppUserModelId(HSTRING*) = 0;
            virtual HRESULT STDMETHODCALLTYPE TryGetMediaPropertiesAsync(Async<Properties>**) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetTimelineProperties(IInspectable**) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetPlaybackInfo(Playback**) = 0;
        };
        struct Sessions : IInspectable
        {
            virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, Session**) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32*) = 0;
        };
        struct Manager : IInspectable
        {
            virtual HRESULT STDMETHODCALLTYPE GetCurrentSession(Session**) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetSessions(Sessions**) = 0;
        };
        struct Factory : IInspectable { virtual HRESULT STDMETHODCALLTYPE RequestAsync(Async<Manager>**) = 0; };
        constexpr GUID FactoryId{0x2050c4ee,0x11a0,0x57de,{0xae,0xd7,0xc9,0x7c,0x70,0x33,0x82,0x45}};

        struct String
        {
            HSTRING Value = nullptr;
            ~String() { WindowsDeleteString(Value); }
            std::string text() const { UINT32 count = 0; const wchar_t* data = WindowsGetStringRawBuffer(Value,&count); return win::utf8({data,count}); }
        };
        template<class T> HRESULT await(Async<T>* operation, T** output, const std::atomic_bool& running)
        {
            if (!operation) return E_POINTER;
            win::ComPtr<IAsyncInfo> info;
            HRESULT hr = operation->QueryInterface(__uuidof(IAsyncInfo),reinterpret_cast<void**>(info.put()));
            if (FAILED(hr)) return hr;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            AsyncStatus state = Started;
            while (SUCCEEDED(hr = info->get_Status(&state)) && state == Started)
            {
                if (!running.load() || std::chrono::steady_clock::now() >= deadline) { info->Cancel(); return HRESULT_FROM_WIN32(ERROR_TIMEOUT); }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (FAILED(hr)) return hr;
            if (state != Completed) { info->get_ErrorCode(&hr); return FAILED(hr) ? hr : E_ABORT; }
            return operation->GetResults(output);
        }
        struct MediaApartment
        {
            HRESULT Result = RoInitialize(RO_INIT_MULTITHREADED);
            ~MediaApartment() { if (SUCCEEDED(Result)) RoUninitialize(); }
        };
    }

    WindowsMediaSnapshot windowsMediaSnapshot(const std::atomic_bool& running)
    {
        WindowsMediaSnapshot result;
        MediaApartment apartment;
        const auto failed = [&](const char* operation,HRESULT error) { result.Status = win::error(operation,static_cast<DWORD>(error)); return result; };
        if (FAILED(apartment.Result)) return failed("Windows media initialization",apartment.Result);
        String className;
        constexpr wchar_t name[] = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
        HRESULT hr = WindowsCreateString(name,static_cast<UINT32>(std::size(name)-1),&className.Value);
        if (FAILED(hr)) return failed("Windows media class",hr);
        win::ComPtr<Factory> factory;
        hr = RoGetActivationFactory(className.Value,FactoryId,reinterpret_cast<void**>(factory.put()));
        if (FAILED(hr)) return failed("Windows media session manager (requires Windows 10 1809+)",hr);
        win::ComPtr<Async<Manager>> request;
        hr = factory->RequestAsync(request.put());
        if (FAILED(hr)) return failed("requesting media sessions",hr);
        win::ComPtr<Manager> manager;
        hr = await(request.get(),manager.put(),running);
        if (FAILED(hr)) return failed("reading media sessions",hr);
        win::ComPtr<Session> current;
        manager->GetCurrentSession(current.put());
        win::ComPtr<Sessions> sessions;
        manager->GetSessions(sessions.put());
        UINT32 count = 0; if (sessions) sessions->get_Size(&count);
        for (UINT32 index = 0; index < count; ++index)
        {
            win::ComPtr<Session> candidate;
            if (FAILED(sessions->GetAt(index,candidate.put()))) continue;
            win::ComPtr<Playback> playback;
            INT32 state = 0;
            if (SUCCEEDED(candidate->GetPlaybackInfo(playback.put())) && playback && SUCCEEDED(playback->get_PlaybackStatus(&state)) && state == 4)
            { candidate->AddRef(); *current.put() = candidate.get(); break; }
        }
        if (!current) { result.Status = "No Windows media sessions"; return result; }
        String player;
        current->get_SourceAppUserModelId(&player.Value); result.Player = player.text();
        win::ComPtr<Playback> playback;
        INT32 state = 0;
        if (SUCCEEDED(current->GetPlaybackInfo(playback.put())) && playback) playback->get_PlaybackStatus(&state);
        result.Playing = state == 4;
        result.Status = result.Playing ? "Windows media playing" : "Windows media paused / stopped";
        win::ComPtr<Async<Properties>> propertiesRequest;
        hr = current->TryGetMediaPropertiesAsync(propertiesRequest.put());
        if (FAILED(hr)) return failed("requesting media properties",hr);
        win::ComPtr<Properties> properties;
        hr = await(propertiesRequest.get(),properties.put(),running);
        if (FAILED(hr)) return failed("reading media properties",hr);
        String title,artist;
        properties->get_Title(&title.Value); properties->get_Artist(&artist.Value);
        result.Title = title.text(); result.Artist = artist.text();
        if (!result.Playing) return result;
        win::ComPtr<StreamReference> thumbnail;
        if (FAILED(properties->get_Thumbnail(thumbnail.put())) || !thumbnail) { result.Status = "Windows media active, no artwork"; return result; }
        win::ComPtr<Async<IUnknown>> imageRequest;
        hr = thumbnail->OpenReadAsync(imageRequest.put());
        if (FAILED(hr)) return failed("requesting media artwork",hr);
        win::ComPtr<IUnknown> image;
        hr = await(imageRequest.get(),image.put(),running);
        if (FAILED(hr)) return failed("reading media artwork",hr);
        win::ComPtr<IStream> stream;
        hr = CreateStreamOverRandomAccessStream(image.get(),IID_IStream,reinterpret_cast<void**>(stream.put()));
        if (FAILED(hr)) return failed("opening media artwork stream",hr);
        STATSTG stats{};
        hr = stream->Stat(&stats,STATFLAG_NONAME);
        if (FAILED(hr) || stats.cbSize.QuadPart == 0 || stats.cbSize.QuadPart > 16*1024*1024)
        { result.Status = "Media artwork is empty or exceeds 16 MiB"; return result; }
        result.Artwork.resize(static_cast<std::size_t>(stats.cbSize.QuadPart));
        ULONG read = 0;
        hr = stream->Read(result.Artwork.data(),static_cast<ULONG>(result.Artwork.size()),&read);
        if (FAILED(hr) || read != result.Artwork.size()) { result.Artwork.clear(); return failed("reading media artwork bytes",FAILED(hr) ? hr : E_FAIL); }
        return result;
    }
}
#endif
