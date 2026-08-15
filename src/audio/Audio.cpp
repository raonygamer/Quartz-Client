#include "quartz/client/Model.hpp"

namespace quartz::client
{
    std::string shellQuote(const std::string_view value)
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

    std::vector<std::byte> readCommand(const std::string& command, const std::size_t maxBytes)
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

    bool commandExists(const std::string_view command)
    {
        return !trim(bytesToString(readCommand("command -v " + shellQuote(command) + " 2>/dev/null", 4096))).empty();
    }

    std::vector<AudioSourceInfo> enumerateAudioSources()
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


}
