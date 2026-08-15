#pragma once
#include "quartz/client/runtime/RuntimeBindingEngine.hpp"

namespace quartz::client
{
    struct OpcodePatternEditorState
    {
        TextEditor Editor;
        std::uint64_t BindingId = 0;
        bool Initialized = false;
        std::string Status;
        std::vector<std::string> Diagnostics;
    };
    OpcodePatternEditorState& opcodePatternEditorState();
    const TextEditor::Language* intelAsmPatternLanguage();
    void openOpcodePatternEditor(RuntimeBinding& binding);
    bool lintOpcodePattern(OpcodePatternEditorState& state);
    bool applyOpcodePattern(OpcodePatternEditorState& state, RuntimeBindingEngine& engine);
}
