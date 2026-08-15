from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    file = Path(path)
    text = file.read_text()
    if new in text:
        return
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one replacement target, found {count}")
    file.write_text(text.replace(old, new, 1))


replace_once("CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)", "cmake_minimum_required(VERSION 3.23)")
replace_once(
    "CMakeLists.txt",
    "add_subdirectory(protocol)\n",
    "set(LIBHAT_TESTING OFF CACHE BOOL \"\" FORCE)\n"
    "set(LIBHAT_EXAMPLES OFF CACHE BOOL \"\" FORCE)\n"
    "set(LIBHAT_INSTALL_TARGET OFF CACHE BOOL \"\" FORCE)\n"
    "set(LIBHAT_WARNINGS_AS_ERRORS OFF CACHE BOOL \"\" FORCE)\n"
    "set(LIBHAT_FEATURE_AVX512 ON CACHE BOOL \"\" FORCE)\n"
    "set(LIBHAT_HINT_X86_64 ON CACHE BOOL \"\" FORCE)\n\n"
    "add_subdirectory(protocol)\n",
)
replace_once("CMakeLists.txt", "add_subdirectory(vendor/zydis)\n", "add_subdirectory(vendor/zydis)\nadd_subdirectory(vendor/libhat)\n")
replace_once("CMakeLists.txt", "        Zydis\n)", "        Zydis\n        libhat::libhat\n)")

replace_once(
    "src/runtime/RuntimeNative.cpp",
    "#include \"quartz/client/Model.hpp\"\n",
    "#include \"quartz/client/Model.hpp\"\n#include <libhat/scanner.hpp>\n#include <libhat/signature.hpp>\n",
)
replace_once(
    "src/runtime/RuntimeNative.cpp",
    "        const auto opcodePatterns = binding.SignaturePatternKind == RuntimeSignaturePatternKind::OpcodePattern ? parseRuntimeOpcodePattern(binding.Signature) : std::vector<std::string>{};\n"
    "        const std::size_t ScanBudget = binding.SignaturePatternKind == RuntimeSignaturePatternKind::OpcodePattern ? 32 * 1024 : 256 * 1024;\n"
    "        const std::size_t ReadChunk = binding.SignaturePatternKind == RuntimeSignaturePatternKind::OpcodePattern ? 32 * 1024 : 128 * 1024;\n",
    "        const auto opcodePatterns = binding.SignaturePatternKind == RuntimeSignaturePatternKind::OpcodePattern ? parseRuntimeOpcodePattern(binding.Signature) : std::vector<std::string>{};\n"
    "        hat::signature libhatSignature;\n"
    "        if (binding.SignaturePatternKind == RuntimeSignaturePatternKind::HexadecimalPattern)\n"
    "        {\n"
    "            libhatSignature.reserve(binding.SignatureBytes.size());\n"
    "            for (std::size_t i = 0; i < binding.SignatureBytes.size(); ++i)\n"
    "                libhatSignature.emplace_back(static_cast<std::byte>(binding.SignatureBytes[i]), static_cast<std::byte>(binding.SignatureMasks[i]));\n"
    "        }\n"
    "        const hat::scan_hint libhatHint = binding.SignatureExecutableOnly ? hat::scan_hint::x86_64 : hat::scan_hint::none;\n"
    "        const std::size_t ScanBudget = binding.SignaturePatternKind == RuntimeSignaturePatternKind::OpcodePattern ? 32 * 1024 : 8 * 1024 * 1024;\n"
    "        const std::size_t ReadChunk = binding.SignaturePatternKind == RuntimeSignaturePatternKind::OpcodePattern ? 32 * 1024 : 4 * 1024 * 1024;\n",
)
replace_once(
    "src/runtime/RuntimeNative.cpp",
    "            const std::size_t last = buffer.size() - binding.SignatureBytes.size();\n"
    "            for (std::size_t offset = 0; offset <= last; ++offset)\n"
    "            {\n"
    "                std::size_t opcodeLength = 0;\n"
    "                const bool matched = binding.SignaturePatternKind == RuntimeSignaturePatternKind::HexadecimalPattern ? runtimeSignatureMatches(buffer, offset, binding) : runtimeOpcodePatternMatches(std::span<const std::uint8_t>(buffer).subspan(offset), binding.SignatureCursor + offset, opcodePatterns, opcodeLength);\n"
    "                if (!matched) continue;\n"
    "                const std::uintptr_t match = binding.SignatureCursor + offset;\n",
    "            std::optional<std::size_t> matchOffset;\n"
    "            if (binding.SignaturePatternKind == RuntimeSignaturePatternKind::HexadecimalPattern)\n"
    "            {\n"
    "                const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(buffer.data()), buffer.size()};\n"
    "                const auto result = hat::find_pattern(bytes, libhatSignature, hat::scan_alignment::X1, libhatHint);\n"
    "                if (result.has_result()) matchOffset = static_cast<std::size_t>(result.get() - bytes.data());\n"
    "            }\n"
    "            else\n"
    "            {\n"
    "                const std::size_t last = buffer.size() - binding.SignatureBytes.size();\n"
    "                for (std::size_t offset = 0; offset <= last; ++offset)\n"
    "                {\n"
    "                    std::size_t opcodeLength = 0;\n"
    "                    if (!runtimeOpcodePatternMatches(std::span<const std::uint8_t>(buffer).subspan(offset), binding.SignatureCursor + offset, opcodePatterns, opcodeLength)) continue;\n"
    "                    matchOffset = offset;\n"
    "                    break;\n"
    "                }\n"
    "            }\n"
    "            if (matchOffset)\n"
    "            {\n"
    "                const std::size_t offset = *matchOffset;\n"
    "                const std::uintptr_t match = binding.SignatureCursor + offset;\n",
)

replace_once(
    "src/ui/RuntimeUI.cpp",
    "namespace quartz::client\n{\n    void mapSpectrumToColumns",
    "namespace quartz::client\n{\n"
    "    void drawIndeterminateProgressBar(ImVec2 size)\n"
    "    {\n"
    "        if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;\n"
    "        if (size.y <= 0.0f) size.y = ImGui::GetFrameHeight();\n"
    "        const ImVec2 min = ImGui::GetCursorScreenPos();\n"
    "        const ImVec2 max{min.x + size.x, min.y + size.y};\n"
    "        ImGui::Dummy(size);\n"
    "        ImDrawList* drawList = ImGui::GetWindowDrawList();\n"
    "        const float rounding = ImGui::GetStyle().FrameRounding;\n"
    "        drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);\n"
    "        const float segmentWidth = std::max(size.x * 0.28f, 24.0f);\n"
    "        const float phase = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.85f, 1.0f);\n"
    "        const float left = min.x - segmentWidth + (size.x + segmentWidth) * phase;\n"
    "        const float clippedLeft = std::max(left, min.x);\n"
    "        const float clippedRight = std::min(left + segmentWidth, max.x);\n"
    "        if (clippedRight > clippedLeft) drawList->AddRectFilled({clippedLeft, min.y}, {clippedRight, max.y}, ImGui::GetColorU32(ImGuiCol_PlotHistogram), rounding);\n"
    "    }\n\n"
    "    void mapSpectrumToColumns",
)
replace_once(
    "src/ui/RuntimeUI.cpp",
    "                if (binding.SignatureResolvedAddress == 0 && binding.SignatureProgress > 0.0f && binding.SignatureInstructionAddress == 0) ImGui::ProgressBar(binding.SignatureProgress, ImVec2(320.0f, 0.0f));",
    "                if (binding.SignatureResolvedAddress == 0 && binding.SignatureInstructionAddress == 0 && binding.SignatureStatus.starts_with(\"Scanning pattern\")) drawIndeterminateProgressBar(ImVec2(320.0f, 0.0f));",
)

print("libhat migration applied")
