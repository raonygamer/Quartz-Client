#include "quartz/client/ui/I18n.hpp"
#include "quartz/client/Functions.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <string>

namespace quartz::client::ui::i18n
{
    namespace
    {
        struct Entry { std::string_view Key; const char* English; const char* Portuguese; };
        constexpr auto Catalog = std::to_array<Entry>({
            {"nav.section.visual", "VISUAL", "VISUAL"},
            {"nav.section.scripting", "SCRIPTING", "SCRIPTS"},
            {"nav.section.reverseEngineering", "REVERSE ENGINEERING", "ENGENHARIA REVERSA"},
            {"nav.section.device", "DEVICE", "DISPOSITIVO"},
            {"nav.section.diagnostics", "DIAGNOSTICS", "DIAGNÓSTICOS"},
            {"nav.section.other", "OTHER", "OUTROS"},
            {"nav.visualizer", "Visualizer", "Visualizador"},
            {"nav.shaders", "Shaders", "Shaders"},
            {"nav.spectrum", "Spectrum", "Espectro"},
            {"nav.audio", "Audio", "Áudio"},
            {"nav.rgb", "RGB", "RGB"},
            {"nav.javascript", "Scripts", "Scripts"},
            {"nav.profiles", "Profiles", "Perfis"},
            {"nav.native", "Workspace", "Workspace"},
            {"nav.memory-scanner", "Memory Scanner", "Scanner de Memória"},
            {"nav.memory-watch", "Memory Watch", "Monitor de Memória"},
            {"nav.device", "Device", "Dispositivo"},
            {"nav.input", "Input", "Entrada"},
            {"nav.usb", "USB", "USB"},
            {"nav.firmware", "Firmware", "Firmware"},
            {"nav.performance", "Performance", "Desempenho"},
            {"nav.matrix-timing", "Matrix Timing", "Timing da Matriz"},
            {"nav.qrpc", "QRPC", "QRPC"},
            {"nav.timeline", "Timeline", "Linha do Tempo"},
            {"header.byline", "Made by Raony Reis, not affiliated with Redragon", "Feito por Raony Reis, sem afiliação com a Redragon"},
            {"header.keyboard", "Keyboard", "Teclado"},
            {"header.appearance", "Appearance", "Aparência"},
            {"header.terminate", "Terminate", "Encerrar"},
            {"header.terminateTooltip", "Exit Quartz completely. The normal window close button only hides the window.", "Fecha o Quartz completamente. O botão normal de fechar apenas oculta a janela."},
            {"appearance.theme", "Theme", "Tema"},
            {"appearance.language", "Language", "Idioma"},
            {"appearance.suspicious", "Enable suspicious color themes", "Ativar temas de cores suspeitos"},
            {"appearance.suspiciousTooltip", "Unlocks Deviluke Pink, Kurosaki Pink, Yami Golden and Kirisaki Purple.", "Libera Deviluke Pink, Kurosaki Pink, Yami Golden e Kirisaki Purple."},
            {"appearance.suspiciousTitle", "Suspicious color themes", "Temas de cores suspeitos"},
            {"appearance.suspiciousWarning", "Are you sure you want to enable those colors? YOU WILL NEVER BE ABLE TO GO BACK LOL... not really tho", "Tem certeza que quer ativar essas cores? VOCÊ NUNCA MAIS VAI PODER VOLTAR KKK... mentira, vai sim"},
            {"appearance.suspiciousEnable", "Enable suspicious color themes", "Ativar temas suspeitos"},
            {"appearance.suspiciousCancel", "I choose peace", "Eu escolho a paz"},
            {"appearance.globalBrightness", "Global brightness", "Brilho global"},
            {"appearance.previewInterpolation", "Preview interpolation", "Interpolação da prévia"},
            {"appearance.previewInterpolationTooltip", "Preview-only spatial color mixing. It never changes framebuffer data sent over USB.", "Mistura espacial de cores apenas na prévia. Nunca altera o framebuffer enviado por USB."},
            {"appearance.reset", "Reset appearance", "Redefinir aparência"},
            {"keyboardPreview.title", "Keyboard Preview", "Prévia do Teclado"},
            {"usb.description", "USB connection, transport health and low-level transfer diagnostics live here.", "Conexão USB, saúde do transporte e diagnósticos de baixo nível ficam aqui."},
            {"usb.connection", "Connection", "Conexão"},
            {"usb.connected", "Connected", "Conectado"},
            {"usb.disconnected", "Disconnected", "Desconectado"},
            {"usb.connect", "Connect", "Conectar"},
            {"usb.disconnect", "Disconnect", "Desconectar"},
            {"usb.autoReconnect", "Auto reconnect", "Reconectar automaticamente"},
            {"usb.deviceFirmware", "Device %04X:%04X | firmware %s", "Dispositivo %04X:%04X | firmware %s"},
            {"usb.frames", "Frames sent %llu   dropped/busy %llu", "Frames enviados %llu   perdidos/ocupados %llu"},
            {"common.refresh", "Refresh", "Atualizar"},
            {"common.copy", "Copy", "Copiar"},
            {"common.cancel", "Cancel", "Cancelar"},
            {"common.close", "Close", "Fechar"},
            {"re.signatureMaker", "Signature Maker", "Gerador de Assinatura"},
            {"re.makeSignature", "Make signature", "Gerar assinatura"},
            {"re.copySignature", "Copy signature", "Copiar assinatura"},
            {"re.signatureUnique", "Unique match", "Correspondência única"},
            {"re.signatureConflict", "Conflicting matches", "Correspondências conflitantes"},
            {"re.signatureAddress", "Target address", "Endereço alvo"},
            {"re.signatureBytes", "Instruction bytes", "Bytes das instruções"},
            {"re.signatureStatus", "Status", "Status"},
            {"re.signatureDescription", "Build a signature from decoded instructions. Relocation-sensitive operands are wildcarded automatically and libhat extends the pattern until the target is unique.", "Gera uma assinatura a partir das instruções decodificadas. Operandos sensíveis a relocação recebem curingas automaticamente e o libhat estende o padrão até o alvo ser único."},
            {"re.signatureMinimumInstructions", "Minimum instructions", "Instruções mínimas"},
            {"re.signatureMaximumInstructions", "Maximum instructions", "Instruções máximas"},
            {"re.signatureMatches", "Matches", "Correspondências"},
            {"re.inspect", "Inspect", "Inspecionar"}
        });

        Language CurrentLanguage = Language::English;
        std::filesystem::path languagePath() { return settingsPath().parent_path() / "ui.language.ini"; }
    }

    Language language() noexcept { return CurrentLanguage; }

    const char* languageName(const Language value) noexcept
    {
        switch (value)
        {
        case Language::English: return "English";
        case Language::PortugueseBrazil: return "Português (Brasil)";
        case Language::Count: break;
        }
        return "English";
    }

    const char* tr(const std::string_view key) noexcept
    {
        for (const auto& entry : Catalog)
        {
            if (entry.Key != key) continue;
            return CurrentLanguage == Language::PortugueseBrazil ? entry.Portuguese : entry.English;
        }
        thread_local std::string fallback; fallback.assign(key); return fallback.c_str();
    }

    void setLanguage(const Language value) noexcept { CurrentLanguage = value >= Language::English && value < Language::Count ? value : Language::English; }

    void loadLanguagePreference() noexcept
    {
        try
        {
            std::ifstream file(languagePath());
            if (file)
            {
                std::string line;
                while (std::getline(file, line))
                {
                    if (!line.starts_with("Language=")) continue;
                    int value = 0;
                    if (parseNumber(std::string_view(line).substr(9), value)) setLanguage(static_cast<Language>(value));
                    return;
                }
            }
            if (const char* locale = std::getenv("LC_ALL"); locale && std::string_view(locale).starts_with("pt")) CurrentLanguage = Language::PortugueseBrazil;
            else if (const char* locale = std::getenv("LANG"); locale && std::string_view(locale).starts_with("pt")) CurrentLanguage = Language::PortugueseBrazil;
        }
        catch (...) {}
    }

    void saveLanguagePreference() noexcept
    {
        try
        {
            std::error_code ec; std::filesystem::create_directories(languagePath().parent_path(), ec);
            std::ofstream file(languagePath(), std::ios::trunc);
            if (file) file << "Language=" << static_cast<int>(CurrentLanguage) << '\n';
        }
        catch (...) {}
    }

    bool drawLanguageSelector()
    {
        bool changed = false;
        ImGui::SetNextItemWidth(190.0f);
        if (ImGui::BeginCombo(tr("appearance.language"), languageName(CurrentLanguage)))
        {
            for (int i = 0; i < static_cast<int>(Language::Count); ++i)
            {
                const Language candidate = static_cast<Language>(i);
                const bool selected = candidate == CurrentLanguage;
                if (ImGui::Selectable(languageName(candidate), selected)) { CurrentLanguage = candidate; saveLanguagePreference(); changed = true; }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    }
}
