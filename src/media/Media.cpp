#include "quartz/client/Model.hpp"

namespace quartz::client
{
    std::string bytesToString(const std::vector<std::byte>& bytes)
    {
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }


    std::string trim(std::string value)
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

    std::vector<std::string> split(const std::string_view value, const char delimiter)
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

    std::string percentDecode(const std::string_view value)
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


}
