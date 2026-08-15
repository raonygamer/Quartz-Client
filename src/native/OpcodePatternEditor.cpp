#include "quartz/client/native/OpcodePatternEditor.hpp"
#include "quartz/client/Model.hpp"
#if QUARTZ_HAS_ZYDIS
#include <Zydis/Zydis.h>
#endif

namespace quartz::client
{
    namespace
    {
        TextEditor::Iterator asmIdentifier(TextEditor::Iterator start, const TextEditor::Iterator end)
        {
            auto it = start; if (it == end || !(std::isalpha(static_cast<unsigned char>(*it)) || *it == '_' || *it == '.')) return start; ++it;
            while (it != end && (std::isalnum(static_cast<unsigned char>(*it)) || *it == '_' || *it == '.')) ++it; return it;
        }
        TextEditor::Iterator asmNumber(TextEditor::Iterator start, const TextEditor::Iterator end)
        {
            auto it = start; if (it == end || !std::isdigit(static_cast<unsigned char>(*it))) return start; ++it;
            while (it != end && (std::isxdigit(static_cast<unsigned char>(*it)) || *it == 'x' || *it == 'X' || *it == 'h' || *it == 'H' || *it == '.')) ++it; return it;
        }
    }

    const TextEditor::Language* intelAsmPatternLanguage()
    {
        static const TextEditor::Language language = []
        {
            TextEditor::Language l; l.name = "Intel x86-64 pattern"; l.caseSensitive = false; l.singleLineComment = ";"; l.singleLineCommentAlt = "//"; l.hasSingleQuotedStrings = true; l.hasDoubleQuotedStrings = true; l.getIdentifier = asmIdentifier; l.getNumber = asmNumber;
            l.isPunctuation = [](const ImWchar c) { return std::string_view("[]()+-*/,?:").find(static_cast<char>(c)) != std::string_view::npos; };
#if QUARTZ_HAS_ZYDIS
            for (int i = 0; i <= static_cast<int>(ZYDIS_MNEMONIC_MAX_VALUE); ++i) if (const char* name = ZydisMnemonicGetString(static_cast<ZydisMnemonic>(i)); name && *name) l.keywords.insert(runtimeLower(name));
#else
            l.keywords = {"mov", "lea", "call", "jmp", "cmp", "test", "add", "sub", "imul", "xor", "and", "or", "push", "pop", "ret", "nop"};
#endif
            l.declarations = {"byte", "word", "dword", "qword", "xmmword", "ymmword", "zmmword", "ptr", "short", "near", "far"};
            static constexpr const char* regs[] = {"rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp","rip","eax","ebx","ecx","edx","esi","edi","ebp","esp","ax","bx","cx","dx","si","di","bp","sp","al","bl","cl","dl","ah","bh","ch","dh","spl","bpl","sil","dil","cs","ds","es","fs","gs","ss"};
            for (const char* reg : regs) l.identifiers.insert(reg); for (int i = 0; i < 32; ++i) { l.identifiers.insert("r" + std::to_string(i)); l.identifiers.insert("r" + std::to_string(i) + "d"); l.identifiers.insert("r" + std::to_string(i) + "w"); l.identifiers.insert("r" + std::to_string(i) + "b"); l.identifiers.insert("xmm" + std::to_string(i)); l.identifiers.insert("ymm" + std::to_string(i)); l.identifiers.insert("zmm" + std::to_string(i)); }
            return l;
        }();
        return &language;
    }

    OpcodePatternEditorState& opcodePatternEditorState() { static OpcodePatternEditorState state; return state; }

    void openOpcodePatternEditor(RuntimeBinding& binding)
    {
        auto& state = opcodePatternEditorState(); state.BindingId = binding.Id; state.Editor.SetLanguage(intelAsmPatternLanguage()); state.Editor.SetShowLineNumbersEnabled(true); state.Editor.SetShowMiniMapEnabled(true); state.Editor.SetText(binding.Signature); state.Initialized = true; state.Status.clear(); lintOpcodePattern(state);
    }

    bool lintOpcodePattern(OpcodePatternEditorState& state)
    {
        state.Diagnostics.clear(); state.Editor.ClearMarkers(); const std::string text = state.Editor.GetText(); std::istringstream stream(text); std::string line; std::size_t lineNumber = 0;
        const auto* language = intelAsmPatternLanguage();
        while (std::getline(stream, line))
        {
            std::string normalized = runtimeNormalizeOpcodeText(line); if (normalized.empty() || normalized.starts_with(";") || normalized.starts_with("#") || normalized.starts_with("//")) { ++lineNumber; continue; }
            int brackets = 0; for (const char c : normalized) { if (c == '[') ++brackets; else if (c == ']') --brackets; if (brackets < 0) break; }
            std::string message;
            if (brackets != 0) message = "unbalanced memory operand brackets";
            else
            {
                const std::size_t split = normalized.find_first_of(" \t"); const std::string mnemonic = normalized.substr(0, split);
                if (mnemonic.find('*') == std::string::npos && mnemonic.find('?') == std::string::npos && !language->keywords.contains(mnemonic)) message = "unknown x86 mnemonic: " + mnemonic;
            }
            if (!message.empty()) { state.Diagnostics.push_back("line " + std::to_string(lineNumber + 1) + ": " + message); state.Editor.AddMarker(lineNumber, IM_COL32(255, 90, 90, 255), IM_COL32(255, 135, 135, 255), message, message); }
            ++lineNumber;
        }
        state.Status = state.Diagnostics.empty() ? "Pattern syntax looks valid" : std::to_string(state.Diagnostics.size()) + " lint issue(s)"; return state.Diagnostics.empty();
    }

    bool applyOpcodePattern(OpcodePatternEditorState& state, RuntimeBindingEngine& engine)
    {
        RuntimeBinding* binding = engine.findBinding(state.BindingId); if (!binding) { state.Status = "target binding no longer exists"; return false; }
        const std::string text = state.Editor.GetText(); if (text.size() >= sizeof(binding->Signature)) { state.Status = "opcode pattern exceeds binding storage"; return false; }
        lintOpcodePattern(state); std::snprintf(binding->Signature, sizeof(binding->Signature), "%s", text.c_str()); binding->SignaturePatternKind = RuntimeSignaturePatternKind::OpcodePattern; binding->AddressMode = ProcessAddressMode::Signature; resetRuntimeSignatureScan(*binding); binding->SignatureConfigHash = 0; binding->NextUpdate = 0.0; engine.markChanged(); state.Status = state.Diagnostics.empty() ? "Applied to binding and scheduled rescan" : "Applied with lint warnings and scheduled rescan"; return true;
    }
}
