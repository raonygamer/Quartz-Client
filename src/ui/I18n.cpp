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
            {"nav.shaders", "Shaders", "Sombreadores"},
            {"nav.spectrum", "Spectrum", "Espectro"},
            {"nav.audio", "Audio", "Áudio"},
            {"nav.rgb", "RGB", "RGB"},
            {"nav.javascript", "Scripts", "Scripts"},
            {"nav.profiles", "Profiles", "Perfis"},
            {"nav.native", "Workspace", "Área de Trabalho"},
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
            {"header.configuration", "Configuration", "Configuração"},
            {"header.theme", "Theme", "Tema"},
            {"header.terminate", "Terminate", "Encerrar"},
            {"header.terminateTooltip", "Exit Quartz completely. The normal window close button only hides the window.", "Fecha o Quartz completamente. O botão normal de fechar apenas oculta a janela."},

            {"appearance.theme", "Theme", "Tema"},
            {"theme.quartzCyan", "Quartz Cyan", "Quartz Ciano"},
            {"theme.graphite", "Graphite", "Grafite"},
            {"theme.oceanBlue", "Ocean Blue", "Azul Oceano"},
            {"theme.emerald", "Emerald", "Esmeralda"},
            {"theme.devilukePink", "Deviluke Pink", "Deviluke Rosa"},
            {"theme.kurosakiPink", "Kurosaki Pink", "Kurosaki Rosa"},
            {"theme.yamiGolden", "Yami Golden", "Yami Dourado"},
            {"theme.kirisakiPurple", "Kirisaki Purple", "Kirisaki Roxo"},
            {"appearance.language", "Language", "Idioma"},
            {"appearance.cornerRounding", "Corner rounding", "Cantos arredondados"},
            {"appearance.suspicious", "Enable suspicious color themes", "Ativar temas de cores suspeitos"},
            {"appearance.suspiciousTooltip", "Unlocks Deviluke Pink, Kurosaki Pink, Yami Golden and Kirisaki Purple.", "Libera Deviluke Rosa, Kurosaki Rosa, Yami Dourado e Kirisaki Roxo."},
            {"appearance.suspiciousTitle", "Suspicious color themes", "Temas de cores suspeitos"},
            {"appearance.suspiciousWarning", "Are you sure you want to enable those colors? YOU WILL NEVER BE ABLE TO GO BACK LOL... not really tho", "Tem certeza que quer ativar essas cores? VOCÊ NUNCA MAIS VAI PODER VOLTAR KKK... mentira, vai sim"},
            {"appearance.suspiciousEnable", "Enable suspicious color themes", "Ativar temas suspeitos"},
            {"appearance.suspiciousCancel", "I choose peace", "Eu escolho a paz"},
            {"appearance.globalBrightness", "Global brightness", "Brilho global"},
            {"appearance.previewInterpolation", "Preview interpolation", "Interpolação da prévia"},
            {"appearance.previewInterpolationTooltip", "Preview-only spatial color mixing. It never changes framebuffer data sent over USB.", "Mistura espacial de cores apenas na prévia. Nunca altera o framebuffer enviado por USB."},
            {"appearance.reset", "Reset appearance", "Redefinir aparência"},
            {"keyboardPreview.title", "Keyboard Preview", "Prévia do Teclado"},

            {"configuration.patternScanning", "Pattern scanning", "Escaneamento de padrões"},
            {"configuration.signatureScanChunk", "Signature scan read chunk (bytes)", "Bloco de leitura do scan de assinatura (bytes)"},
            {"configuration.signatureScanChunkTooltip", "Maximum target-process memory copied per libhat-backed signature scan chunk. Quartz clamps this to 64 KiB–64 MiB and aligns it to 8 bytes. Larger chunks reduce process-memory read overhead, but use more temporary memory and make cancellation less granular.", "Máximo de memória do processo alvo copiada por bloco de scan de assinatura usando libhat. O Quartz limita entre 64 KiB e 64 MiB e alinha o valor em 8 bytes. Blocos maiores reduzem o overhead das leituras de memória, mas usam mais memória temporária e deixam o cancelamento menos granular."},
            {"configuration.signatureScanChunkValue", "Current: %.2f MiB (%zu bytes), aligned to %zu bytes", "Atual: %.2f MiB (%zu bytes), alinhado em %zu bytes"},
            {"configuration.signatureScanChunkTradeoff", "4 MiB is the default. libhat scans the supplied span directly with its selected vectorized backend; the practical tradeoff here is target-memory read overhead versus temporary buffer size and cancellation latency, not a small-buffer limit inside libhat.", "4 MiB é o padrão. O libhat varre diretamente o intervalo fornecido usando o backend vetorizado selecionado; o compromisso prático aqui é overhead das leituras da memória alvo contra tamanho do buffer temporário e latência de cancelamento, não um limite de buffer pequeno dentro do libhat."},
            {"configuration.newScansOnly", "Changes apply to newly started scans; running scans keep the chunk size they started with.", "As alterações se aplicam a novos scans; scans em andamento mantêm o tamanho de bloco com que foram iniciados."},
            {"configuration.reset", "Reset configuration", "Redefinir configuração"},

            {"common.refresh", "Refresh", "Atualizar"},
            {"common.refreshProcesses", "Refresh processes", "Atualizar processos"},
            {"common.process", "Process", "Processo"},
            {"common.selectProcess", "<select process>", "<selecionar processo>"},
            {"common.processSearchHint", "Find PID, process name, title, executable path or command line...", "Buscar PID, nome do processo, título, executável ou linha de comando..."},
            {"common.noProcessMatches", "No process matches the filter.", "Nenhum processo corresponde ao filtro."},
            {"common.copy", "Copy", "Copiar"},
            {"common.cancel", "Cancel", "Cancelar"},
            {"common.close", "Close", "Fechar"},
            {"common.reset", "Reset", "Redefinir"},
            {"common.select", "Select", "Selecionar"},
            {"common.open", "Open", "Abrir"},
            {"common.edit", "Edit...", "Editar..."},

            {"shaders.description", "Shader catalog, external source files, hot reload, compilation and reflected material parameters live here. Opening a file keeps Quartz bound to it; importing copies a fragment shader into the Quartz catalog.", "Catálogo de sombreadores, arquivos externos, recarga automática, compilação e parâmetros de material refletidos ficam aqui. Abrir um arquivo mantém o Quartz vinculado a ele; importar copia um sombreador de fragmento para o catálogo do Quartz."},
            {"shaders.current", "Current shader", "Sombreador atual"},
            {"shaders.catalog", "Catalog", "Catálogo"},
            {"shaders.customExternal", "Custom / external", "Personalizado / externo"},
            {"shaders.refreshCatalog", "Refresh catalog", "Atualizar catálogo"},
            {"shaders.compileCurrent", "Compile current", "Compilar atual"},
            {"shaders.saveDefaults", "Save as Quartz defaults", "Salvar como padrão do Quartz"},
            {"shaders.restoreDefault", "Restore default", "Restaurar padrão"},
            {"shaders.externalFiles", "External files", "Arquivos externos"},
            {"shaders.vertexFile", "Vertex file", "Arquivo de vértices"},
            {"shaders.fragmentFile", "Fragment file", "Arquivo de fragmento"},
            {"shaders.importCatalog", "Import to catalog", "Importar para o catálogo"},
            {"shaders.hotReload", "Hot reload external changes", "Recarregar alterações externas"},
            {"shaders.hotReloadHint", "200 ms debounce/poll; external edits compile automatically", "debounce/poll de 200 ms; edições externas compilam automaticamente"},
            {"shaders.renderSurface", "Render surface", "Superfície de renderização"},
            {"shaders.framebufferSize", "Framebuffer size", "Tamanho do framebuffer"},
            {"shaders.regenerate", "Regenerate", "Regenerar"},
            {"shaders.downsample", "Downsample", "Redução de amostragem"},
            {"shaders.crossfade", "Crossfade", "Transição"},
            {"shaders.recompileChange", "Recompile on editor text change", "Recompilar ao alterar o texto do editor"},
            {"shaders.keyUniforms", "Key-state uniforms", "Uniformes de estado das teclas"},
            {"shaders.capsColor", "Caps Lock fixed color", "Cor fixa do Caps Lock"},
            {"shaders.scrollColor", "Scroll Lock fixed color", "Cor fixa do Scroll Lock"},
            {"shaders.materialParameters", "Material parameters", "Parâmetros de material"},

            {"re.workspaceDescription", "Native reverse-engineering workspace. Memory, disassembly, watches and signatures share addresses directly so you can move through a process without copy/pasting hexadecimal values between unrelated tools.", "Área de trabalho de engenharia reversa nativa. Memória, desmontagem, monitores e assinaturas compartilham endereços diretamente para navegar pelo processo sem copiar e colar valores hexadecimais entre ferramentas."},
            {"re.memoryDisassembly", "Memory / disassembly", "Memória / desmontagem"},
            {"re.memoryScanner", "Memory Scanner...", "Scanner de Memória..."},
            {"re.memoryWatch", "Memory Watch...", "Monitor de Memória..."},
            {"re.signatureFromAddress", "Signature from address", "Assinatura do endereço"},
            {"re.manualWatch", "Manual watch", "Monitor manual"},
            {"re.signatures", "Signatures", "Assinaturas"},
            {"re.signatureSearch", "Search", "Buscar"},
            {"re.objectExperiments", "Object experiments", "Experimentos com Objetos"},
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
            {"re.inspect", "Inspect", "Inspecionar"},

            {"usb.description", "USB connection, transport health and low-level transfer diagnostics live here.", "Conexão USB, saúde do transporte e diagnósticos de baixo nível ficam aqui."},
            {"usb.connection", "Connection", "Conexão"},
            {"usb.connected", "Connected", "Conectado"},
            {"usb.disconnected", "Disconnected", "Desconectado"},
            {"usb.connect", "Connect", "Conectar"},
            {"usb.disconnect", "Disconnect", "Desconectar"},
            {"usb.autoReconnect", "Auto reconnect", "Reconectar automaticamente"},
            {"usb.deviceFirmware", "Device %04X:%04X | firmware %s", "Dispositivo %04X:%04X | firmware %s"},
            {"usb.frames", "Frames sent %llu   dropped/busy %llu", "Frames enviados %llu   perdidos/ocupados %llu"}
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
