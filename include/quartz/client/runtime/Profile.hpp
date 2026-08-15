#pragma once
#include "quartz/client/Functions.hpp"

namespace quartz::client
{
    struct RuntimeBindingProfile
    {
        std::uint64_t Id = 0;
        bool Enabled = true;
        char Name[64] = "Profile";
        bool Exclusive = true;
        std::vector<std::uint64_t> BindingIds;
        std::vector<std::uint64_t> ControlIds;
        std::vector<std::uint64_t> ScriptIds;
        bool HotkeyCtrl = false;
        bool HotkeyAlt = false;
        bool HotkeyShift = false;
        int HotkeyKey = 0;
        bool HotkeyDown = false;
    };

}
