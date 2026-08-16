#include "quartz/client/ui/TextEditorSupport.hpp"
#include "quartz/client/runtime/QuickJS.hpp"
#include <algorithm>
#include <cctype>
#include <string_view>

namespace quartz::client::ui
{
    namespace
    {
        ImU32 withAlpha(ImVec4 color, const float alpha) noexcept { color.w = alpha; return ImGui::ColorConvertFloat4ToU32(color); }

        std::string_view trimLine(std::string_view line) noexcept
        {
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.remove_suffix(1);
            return line;
        }

        std::string declarationFor(const std::string_view token)
        {
            const std::string_view declarations = runtimeQuickJSTypeDeclarations(); if (declarations.empty() || token.empty()) return {};
            std::string_view name = token; if (const auto dot = name.rfind('.'); dot != std::string_view::npos) name.remove_prefix(dot + 1); if (name.empty()) return {};
            const std::array<std::string, 8> needles = {"interface " + std::string(name), "namespace " + std::string(name), "declare const " + std::string(name), "const " + std::string(name), "function " + std::string(name), std::string(name) + "(", std::string(name) + ":", std::string(name) + "<"};
            std::size_t match = std::string_view::npos;
            for (const auto& needle : needles) { match = declarations.find(needle); if (match != std::string_view::npos) break; }
            if (match == std::string_view::npos) return {};
            const std::size_t begin = declarations.rfind('\n', match), end = declarations.find('\n', match); const std::size_t lineBegin = begin == std::string_view::npos ? 0 : begin + 1; const std::size_t lineEnd = end == std::string_view::npos ? declarations.size() : end;
            const auto line = trimLine(declarations.substr(lineBegin, lineEnd - lineBegin)); return line.empty() ? std::string{} : std::string(line);
        }

        const char* rootDescription(const std::string_view token) noexcept
        {
            if (token == "Process") return "Process selection, module enumeration and process metadata helpers.";
            if (token == "Memory") return "Read, write and inspect process memory from Quartz scripts.";
            if (token == "Signature") return "Pattern scanning helpers, including asynchronous scans for large address spaces.";
            if (token == "Breakpoint") return "Execution and memory-access probe helpers.";
            if (token == "Struct") return "Declare structured memory layouts that can be reused by script APIs.";
            if (token == "Field") return "Typed field descriptors used by Struct definitions.";
            if (token == "Pointer") return "Pointer helpers for native process memory.";
            if (token == "Property") return "Expose configurable script properties in the Quartz UI.";
            if (token == "Runtime") return "Information and controls for the current Quartz script runtime.";
            if (token == "Script") return "Per-script state, persistent storage and lifecycle helpers.";
            if (token == "System") return "Host/system integration exposed by the Quartz SDK.";
            if (token == "Disassembly") return "Native instruction decoding and disassembly helpers.";
            if (token == "Keyboard") return "Keyboard state and key-event helpers.";
            if (token == "Events") return "Script event subscription and dispatch helpers.";
            if (token == "console") return "Script console logging helpers.";
            return nullptr;
        }
    }

    const TextEditor::Language* quartzTypeScriptLanguage()
    {
        static const TextEditor::Language language = []
        {
            TextEditor::Language value; value.name = "TypeScript"; value.singleLineComment = "//"; value.commentStart = "/*"; value.commentEnd = "*/"; value.hasSingleQuotedStrings = true; value.hasDoubleQuotedStrings = true; value.otherStringStart = "`"; value.otherStringEnd = "`"; value.stringEscape = '\\';
            value.keywords = {"abstract","any","as","asserts","async","await","bigint","boolean","break","case","catch","class","const","constructor","continue","debugger","declare","default","delete","do","else","export","extends","finally","for","from","function","get","if","implements","import","in","infer","instanceof","interface","is","keyof","let","module","namespace","never","new","number","object","of","override","private","protected","public","readonly","require","return","satisfies","set","static","string","super","switch","symbol","this","throw","true","try","type","typeof","undefined","unknown","var","void","while","with","yield"};
            value.declarations = {"false","null"};
            value.identifiers = {"Math","JSON","BigInt","Number","String","Boolean","Array","Object","Map","Set","WeakMap","WeakSet","Date","RegExp","Promise","Error","TypeError","NaN","Infinity","Process","Signature","Breakpoint","Struct","Field","Pointer","Property","Runtime","Script","System","Disassembly","Memory","Keyboard","Events","console"};
            value.isPunctuation = [](const ImWchar c) { return std::string_view("[]{}().,;:+-*/%<>=!&|^~?").find(static_cast<char>(c)) != std::string_view::npos; };
            value.getIdentifier = [](TextEditor::Iterator start, const TextEditor::Iterator end) { if (start == end || !(TextEditor::CodePoint::isXidStart(*start) || *start == '_' || *start == '$')) return start; auto current = start; ++current; while (current != end && (TextEditor::CodePoint::isXidContinue(*current) || *current == '$')) ++current; return current; };
            value.getNumber = [](TextEditor::Iterator start, const TextEditor::Iterator end)
            {
                if (start == end) return start; auto current = start; auto next = current; ++next; const auto digit = [](const ImWchar c) { return c >= '0' && c <= '9'; }; const auto hex = [&](const ImWchar c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
                if (*current == '.' && (next == end || !digit(*next))) return start;
                if (*current == '0' && next != end && (*next == 'x' || *next == 'X' || *next == 'b' || *next == 'B' || *next == 'o' || *next == 'O')) { const ImWchar prefix = *next; current = next; ++current; while (current != end && (*current == '_' || (prefix == 'x' || prefix == 'X' ? hex(*current) : prefix == 'b' || prefix == 'B' ? (*current == '0' || *current == '1') : (*current >= '0' && *current <= '7')))) ++current; if (current != end && *current == 'n') ++current; return current; }
                bool dot = false; if (*current == '.') { dot = true; ++current; } while (current != end && (digit(*current) || *current == '_')) ++current; if (!dot && current != end && *current == '.') { ++current; while (current != end && (digit(*current) || *current == '_')) ++current; }
                if (current != end && (*current == 'e' || *current == 'E')) { auto exponent = current; ++exponent; if (exponent != end && (*exponent == '+' || *exponent == '-')) ++exponent; bool any = false; while (exponent != end && (digit(*exponent) || *exponent == '_')) { any |= digit(*exponent); ++exponent; } if (any) current = exponent; }
                if (current != end && *current == 'n') ++current; return current;
            };
            return value;
        }();
        return &language;
    }

    TextEditor::Palette quartzTextEditorPalette()
    {
        auto palette = TextEditor::GetDarkPalette(); const ImVec4 window = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), frame = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg), accent = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive), disabled = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        const ImVec4 background{window.x * 0.72f + frame.x * 0.28f, window.y * 0.72f + frame.y * 0.28f, window.z * 0.72f + frame.z * 0.28f, 0.72f};
        palette[static_cast<std::size_t>(TextEditor::Color::background)] = ImGui::ColorConvertFloat4ToU32(background);
        palette[static_cast<std::size_t>(TextEditor::Color::selection)] = withAlpha(accent, 0.42f);
        palette[static_cast<std::size_t>(TextEditor::Color::lineNumber)] = withAlpha(disabled, 0.82f);
        palette[static_cast<std::size_t>(TextEditor::Color::currentLineNumber)] = withAlpha(text, 0.96f);
        palette[static_cast<std::size_t>(TextEditor::Color::matchingBracketBackground)] = withAlpha(accent, 0.34f);
        palette[static_cast<std::size_t>(TextEditor::Color::keyword)] = IM_COL32(198, 120, 221, 255); palette[static_cast<std::size_t>(TextEditor::Color::declaration)] = IM_COL32(224, 161, 83, 255); palette[static_cast<std::size_t>(TextEditor::Color::number)] = IM_COL32(181, 206, 168, 255); palette[static_cast<std::size_t>(TextEditor::Color::string)] = IM_COL32(152, 195, 121, 255); palette[static_cast<std::size_t>(TextEditor::Color::knownIdentifier)] = IM_COL32(86, 182, 194, 255); palette[static_cast<std::size_t>(TextEditor::Color::identifier)] = withAlpha(text, 0.96f); palette[static_cast<std::size_t>(TextEditor::Color::punctuation)] = withAlpha(text, 0.72f); palette[static_cast<std::size_t>(TextEditor::Color::comment)] = withAlpha(disabled, 0.82f);
        return palette;
    }

    void applyQuartzTextEditorPalette(TextEditor& editor) { editor.SetPalette(quartzTextEditorPalette()); }

    void configureQuartzTypeScriptEditor(TextEditor& editor, const bool readOnly, const bool miniMap)
    {
        editor.SetLanguage(quartzTypeScriptLanguage()); applyQuartzTextEditorPalette(editor); editor.SetTabSize(4); editor.SetInsertSpacesOnTabs(true); editor.SetAutoIndentEnabled(!readOnly); editor.SetShowLineNumbersEnabled(true); editor.SetShowMatchingBrackets(true); editor.SetShowMiniMapEnabled(miniMap); if (miniMap) editor.SetMiniMapColumns(28); editor.SetReadOnlyEnabled(readOnly); editor.SetWordWrapEnabled(false); installQuartzScriptHover(editor);
    }

    void configureQuartzReadOnlyTextEditor(TextEditor& editor, const bool lineNumbers)
    {
        applyQuartzTextEditorPalette(editor); editor.SetReadOnlyEnabled(true); editor.SetCaretsVisible(false); editor.SetShowLineNumbersEnabled(lineNumbers); editor.SetShowMiniMapEnabled(false); editor.SetWordWrapEnabled(false);
    }

    std::string quartzDottedTokenAt(const TextEditor& editor, const TextEditor::DocPos pos)
    {
        const std::string line = editor.GetLineText(pos.line); if (line.empty()) return {}; std::size_t index = std::min(pos.index, line.size()); if (index == line.size() && index) --index;
        const auto allowed = [](const unsigned char c) { return std::isalnum(c) || c == '_' || c == '$' || c == '.'; };
        if (index < line.size() && !allowed(static_cast<unsigned char>(line[index])) && index && allowed(static_cast<unsigned char>(line[index - 1]))) --index; if (index >= line.size() || !allowed(static_cast<unsigned char>(line[index]))) return {};
        std::size_t begin = index, end = index + 1; while (begin && allowed(static_cast<unsigned char>(line[begin - 1]))) --begin; while (end < line.size() && allowed(static_cast<unsigned char>(line[end]))) ++end; return line.substr(begin, end - begin);
    }

    void installQuartzScriptHover(TextEditor& editor)
    {
        editor.SetTextHoverCallback([&editor](TextEditor::PopupData& data)
        {
            const std::string token = quartzDottedTokenAt(editor, data.pos); if (token.empty()) { ImGui::CloseCurrentPopup(); return; }
            if (const char* description = rootDescription(token)) { ImGui::TextUnformatted(token.c_str()); ImGui::Separator(); ImGui::TextWrapped("%s", description); return; }
            const std::string declaration = declarationFor(token); if (declaration.empty()) { ImGui::CloseCurrentPopup(); return; }
            ImGui::TextUnformatted(token.c_str()); ImGui::Separator(); ImGui::TextWrapped("%s", declaration.c_str());
        });
    }
}
