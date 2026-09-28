#pragma once
#ifdef _WIN32
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>
namespace quartz::client
{
    struct WindowsMediaSnapshot
    {
        bool Playing = false;
        std::string Player, Title, Artist, Status;
        std::vector<std::byte> Artwork;
    };
    WindowsMediaSnapshot windowsMediaSnapshot(const std::atomic_bool& running);
}
#endif
