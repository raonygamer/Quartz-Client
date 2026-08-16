#include "quartz/client/ui/AddressInput.hpp"
#include "quartz/client/Functions.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <limits>
#include <vector>

namespace quartz::client::ui
{
    namespace
    {
        class AddressExpressionParser
        {
        public:
            AddressExpressionParser(const pid_t pid, const std::string_view source) : _pid(pid), _source(source) {}

            bool parse(std::uintptr_t& value, std::string& error)
            {
                __int128 result = 0;
                if (!expression(result, error)) return false;
                skip();
                if (_position != _source.size()) { error = "unexpected token at offset " + std::to_string(_position); return false; }
                if (result < 0 || static_cast<unsigned __int128>(result) > std::numeric_limits<std::uintptr_t>::max()) { error = "address expression is outside uintptr_t range"; return false; }
                value = static_cast<std::uintptr_t>(result); error.clear(); return true;
            }

        private:
            void skip() { while (_position < _source.size() && std::isspace(static_cast<unsigned char>(_source[_position]))) ++_position; }

            bool expression(__int128& value, std::string& error)
            {
                if (!term(value, error)) return false;
                for (;;)
                {
                    skip(); if (_position >= _source.size() || (_source[_position] != '+' && _source[_position] != '-')) return true;
                    const char op = _source[_position++]; __int128 rhs = 0; if (!term(rhs, error)) return false; value = op == '+' ? value + rhs : value - rhs;
                }
            }

            bool term(__int128& value, std::string& error)
            {
                if (!unary(value, error)) return false;
                for (;;)
                {
                    skip(); if (_position >= _source.size() || (_source[_position] != '*' && _source[_position] != '/')) return true;
                    const char op = _source[_position++]; __int128 rhs = 0; if (!unary(rhs, error)) return false;
                    if (op == '/' && rhs == 0) { error = "division by zero"; return false; }
                    value = op == '*' ? value * rhs : value / rhs;
                }
            }

            bool unary(__int128& value, std::string& error)
            {
                skip();
                if (_position < _source.size() && (_source[_position] == '+' || _source[_position] == '-'))
                {
                    const char op = _source[_position++]; if (!unary(value, error)) return false; if (op == '-') value = -value; return true;
                }
                return primary(value, error);
            }

            bool primary(__int128& value, std::string& error)
            {
                skip();
                if (_position >= _source.size()) { error = "expected address value"; return false; }
                if (_source[_position] == '(')
                {
                    ++_position; if (!expression(value, error)) return false; skip();
                    if (_position >= _source.size() || _source[_position] != ')') { error = "missing ')'"; return false; }
                    ++_position; return true;
                }
                if (_source[_position] == '\'' || _source[_position] == '"') return moduleToken(value, error, true);
                if (std::isdigit(static_cast<unsigned char>(_source[_position]))) return number(value, error);
                return moduleToken(value, error, false);
            }

            bool number(__int128& value, std::string& error)
            {
                const std::size_t begin = _position; int base = 10;
                if (_position + 2 <= _source.size() && _source[_position] == '0' && (_source[_position + 1] == 'x' || _source[_position + 1] == 'X')) { base = 16; _position += 2; }
                const std::size_t digits = _position;
                while (_position < _source.size() && (std::isalnum(static_cast<unsigned char>(_source[_position])) || _source[_position] == '_')) ++_position;
                std::string token(_source.substr(digits, _position - digits)); token.erase(std::remove(token.begin(), token.end(), '_'), token.end());
                if (token.empty()) { error = "invalid number at offset " + std::to_string(begin); return false; }
                std::uint64_t raw = 0; const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), raw, base);
                if (ec != std::errc{} || ptr != token.data() + token.size()) { error = "invalid number: " + std::string(_source.substr(begin, _position - begin)); return false; }
                value = static_cast<__int128>(raw); return true;
            }

            bool moduleToken(__int128& value, std::string& error, const bool quoted)
            {
                std::string token;
                if (quoted)
                {
                    const char quote = _source[_position++]; const std::size_t begin = _position;
                    while (_position < _source.size() && _source[_position] != quote) ++_position;
                    if (_position >= _source.size()) { error = "unterminated module name"; return false; }
                    token.assign(_source.substr(begin, _position - begin)); ++_position;
                }
                else
                {
                    const std::size_t begin = _position;
                    while (_position < _source.size())
                    {
                        const char c = _source[_position];
                        if (std::isspace(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')') break;
                        ++_position;
                    }
                    token.assign(_source.substr(begin, _position - begin));
                }
                if (token.empty()) { error = "expected module name at offset " + std::to_string(_position); return false; }
                if (_pid <= 0) { error = "select a process before using module names"; return false; }
                if (_modules.empty()) _modules = enumerateRuntimeModules(_pid);
                const std::string wanted = runtimeLower(token);
                for (const auto& module : _modules)
                {
                    const std::string name = runtimeLower(module.Name);
                    const std::string path = runtimeLower(module.Path);
                    const std::string filename = runtimeLower(std::filesystem::path(module.Path).filename().string());
                    if (wanted == name || wanted == filename || wanted == path) { value = static_cast<__int128>(module.Base); return true; }
                }
                error = "module not found: " + token; return false;
            }

            pid_t _pid = 0;
            std::string_view _source;
            std::size_t _position = 0;
            std::vector<RuntimeProcessModule> _modules;
        };
    }

    bool evaluateAddressExpression(const pid_t pid, const std::string_view expression, std::uintptr_t& value, std::string& error)
    {
        AddressExpressionParser parser(pid, expression); return parser.parse(value, error);
    }

    bool drawAddressInput(const char* label, char* buffer, const std::size_t bufferSize, const pid_t pid, const float width, const ImGuiInputTextFlags flags)
    {
        if (width != 0.0f) ImGui::SetNextItemWidth(width);
        bool changed = ImGui::InputText(label, buffer, bufferSize, flags);
        std::uintptr_t evaluated = 0; std::string error;
        const bool valid = evaluateAddressExpression(pid, buffer, evaluated, error);
        if (ImGui::IsItemFocused() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_E, false) && valid) { std::snprintf(buffer, bufferSize, "0x%llX", static_cast<unsigned long long>(evaluated)); changed = true; }
        if (ImGui::IsItemHovered())
        {
            if (valid) ImGui::SetTooltip("= 0x%llX\nCtrl+Shift+E replaces the expression with its evaluated address.\nSupports + - * /, parentheses and module names such as Terraria.exe+0x0.", static_cast<unsigned long long>(evaluated));
            else ImGui::SetTooltip("Ctrl+Shift+E evaluates this address expression.\nSupports + - * /, parentheses and module names.\n%s", error.c_str());
        }
        return changed;
    }
}
