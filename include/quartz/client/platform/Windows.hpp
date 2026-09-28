#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef near
#undef far
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace quartz::client
{
    // Common register vocabulary used by scripts and the RE workspace on both OSes.
    struct NativeRegisters
    {
        std::uint64_t r15{}, r14{}, r13{}, r12{}, rbp{}, rbx{}, r11{}, r10{}, r9{}, r8{};
        std::uint64_t rax{}, rcx{}, rdx{}, rsi{}, rdi{}, orig_rax{}, rip{}, cs{}, eflags{}, rsp{}, ss{};
        std::uint64_t fs_base{}, gs_base{}, ds{}, es{}, fs{}, gs{};
    };

    namespace win
    {
        class Handle
        {
        public:
            explicit Handle(HANDLE value = nullptr) noexcept : _value(value) {}
            ~Handle() { reset(); }
            Handle(const Handle&) = delete;
            Handle& operator=(const Handle&) = delete;
            Handle(Handle&& other) noexcept : _value(std::exchange(other._value, nullptr)) {}
            Handle& operator=(Handle&& other) noexcept { if (this != &other) { reset(); _value = std::exchange(other._value, nullptr); } return *this; }
            HANDLE get() const noexcept { return _value; }
            explicit operator bool() const noexcept { return _value && _value != INVALID_HANDLE_VALUE; }
            void reset(HANDLE value = nullptr) noexcept { if (*this) CloseHandle(_value); _value = value; }
        private:
            HANDLE _value;
        };

        inline std::string utf8(std::wstring_view text)
        {
            if (text.empty()) return {};
            const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string result(size, '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
            return result;
        }

        inline std::wstring wide(std::string_view text)
        {
            if (text.empty()) return {};
            const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring result(size, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
            return result;
        }

        inline std::string error(const char* operation, DWORD code = GetLastError())
        {
            wchar_t* message = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                          nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
            std::string result = std::string(operation) + " (" + std::to_string(code) + "): " + (message ? utf8(message) : "Windows error");
            if (message) LocalFree(message);
            while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) result.pop_back();
            return result;
        }

        template<class T> class ComPtr
        {
        public:
            ~ComPtr() { if (_value) _value->Release(); }
            ComPtr() = default;
            ComPtr(const ComPtr&) = delete;
            ComPtr& operator=(const ComPtr&) = delete;
            T* get() const { return _value; }
            T* operator->() const { return _value; }
            T** put() { if (_value) { _value->Release(); _value = nullptr; } return &_value; }
            explicit operator bool() const { return _value != nullptr; }
        private:
            T* _value = nullptr;
        };
    }
}
#endif
