#pragma once
#include "quartz/client/Functions.hpp"

namespace quartz::client
{
    class AppCpuMeter
    {
    public:
        float update(const double wallTime) noexcept
        {
            double cpuNow = 0.0;
#ifdef _WIN32
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return _usage;
            const auto ticks = [](FILETIME value) { return (std::uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
            cpuNow = static_cast<double>(ticks(kernel) + ticks(user)) / 10000000.0;
#else
            cpuNow = static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
#endif
            if (_lastWallTime < 0.0)
            {
                _lastWallTime = wallTime;
                _lastCpuTime = cpuNow;
                return _usage;
            }

            const double wallDelta = wallTime - _lastWallTime;
            if (wallDelta < 0.25)
                return _usage;

            const double cpuDelta = cpuNow - _lastCpuTime;
            _usage = static_cast<float>(std::max(0.0, cpuDelta / wallDelta * 100.0));
            _lastWallTime = wallTime;
            _lastCpuTime = cpuNow;
            return _usage;
        }

    private:
        double _lastWallTime = -1.0;
        double _lastCpuTime = 0.0;
        float _usage = 0.0f;
    };

}
