#pragma once
#include "quartz/client/Functions.hpp"

namespace quartz::client
{
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
}
