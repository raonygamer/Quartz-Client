#pragma once
#include "quartz/client/Forward.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace quartz::client::ui
{
    bool evaluateAddressExpression(pid_t pid, std::string_view expression, std::uintptr_t& value, std::string& error);
    bool drawAddressInput(const char* label, char* buffer, std::size_t bufferSize, pid_t pid, float width = 0.0f);
}
