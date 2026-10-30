// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: what the menu can change. See settings_model.h.

#include <algorithm>

#include "menu/settings_model.h"

namespace Menu {

namespace {

const char* const HeaderComment = "# Escrito pelo menu do NX on Orbis. Pode ser editado a mão. / Written by the menu; hand edits are kept.";

} // Anonymous namespace

const std::vector<Option>& Catalog() {
    // The defaults here are the ones main.cpp sets in ApplyPs4Settings(): keep them in step.
    static const std::vector<Option> catalog = {
        // ---------------------------------------------------------------- Video / GPU
        {"mode", Group::Video, "Modo do console", "Console mode",
         "Portátil renderiza em 720p (mais leve). TV renderiza em 1080p (bem mais pesado).",
         "Handheld renders at 720p (lighter). Docked renders at 1080p (much heavier).",
         {{"handheld", "Portátil (720p)", "Handheld (720p)"}, {"docked", "TV (1080p)", "Docked (1080p)"}}, 0},
        {"resolution", Group::Video, "Escala de resolução", "Resolution scale",
         "Multiplica a resolução interna do jogo. Acima de 1x pesa muito na GPU do PS4.",
         "Multiplies the game's internal resolution. Above 1x is very heavy for the PS4 GPU.",
         {{"0.25", "0,25x", "0.25x"}, {"0.5", "0,5x", "0.5x"}, {"0.75", "0,75x", "0.75x"}, {"1", "1x", "1x"}, {"1.25", "1,25x", "1.25x"},
          {"1.5", "1,5x", "1.5x"}, {"2", "2x", "2x"}}, 3},
        {"speed_limit", Group::Video, "Limitar velocidade", "Speed limit",
         "Desligado deixa o jogo rodar o mais rápido possível (ele pode acelerar).",
         "Off lets the game run as fast as it can (it may speed up).",
         {{"on", "Ligado", "On"}, {"off", "Desligado", "Off"}}, 0},
        {"gpu_accuracy", Group::Video, "Precisão da GPU", "GPU accuracy",
         "Baixa é mais rápida (menos sincronizações). Alta corrige efeitos em alguns jogos, mas pesa. Média fica no meio e só existe se esta versão do Eden a tiver (senão o boot.log avisa).",
         "Low is faster (fewer syncs). High fixes effects in some games but is heavier. Medium is in between and exists only if this Eden has it (otherwise boot.log says so).",
         {{"low", "Baixa (rápida)", "Low (fast)"}, {"medium", "Média", "Medium"}, {"high", "Alta", "High"}}, 0},
        {"reactive_flushing", Group::Video, "Flush reativo", "Reactive flushing",
         "Copia dados da GPU para a CPU quando o jogo os lê. Mais preciso, porém mais lento.",
         "Copies GPU data back when the game reads it. More accurate, but slower.",
         {{"off", "Desligado", "Off"}, {"on", "Ligado", "On"}}, 0},
        {"async_shaders", Group::Video, "Shaders assíncronos", "Asynchronous shaders",
         "Compila shaders em segundo plano: menos travadas, mas um efeito pode faltar por instantes.",
         "Compiles shaders in the background: fewer stutters, but an effect may be missing for a moment.",
         {{"on", "Ligado", "On"}, {"off", "Desligado", "Off"}}, 0},
        {"astc", Group::Video, "Texturas ASTC", "ASTC textures",
         "CPU é o modo mais estável no PS4. GPU e CPU assíncrona são experimentais.",
         "CPU is the most stable mode on the PS4. GPU and async CPU are experimental.",
         {{"cpu", "CPU (estável)", "CPU (stable)"}, {"gpu", "GPU", "GPU"}, {"async", "CPU assíncrona", "Async CPU"}}, 0},
        {"shader_cache", Group::Video, "Cache de shaders no disco", "Shader cache on disk",
         "Guarda os shaders compilados para as próximas partidas. Desligue só se suspeitar de um cache corrompido.",
         "Keeps compiled shaders for the next runs. Turn it off only if you suspect a corrupt cache.",
         {{"on", "Ligado", "On"}, {"off", "Desligado", "Off"}}, 0},
        {"shader_preload", Group::Perf, "Cache de shaders carregado ao iniciar", "Shader cache loaded at start",
         "Tamanho máximo do cache de shaders do jogo que é compilado antes do jogo começar (cerca de 32 KiB por shader). Se o cache passar do limite ele é guardado como vulkan.bin.bak e um novo começa; o limite protege a memória, e o resto compila durante o jogo.",
         "Largest size of the game's shader cache that is compiled before the game starts (about 32 KiB per shader). A bigger cache is kept as vulkan.bin.bak and a new one starts; the limit protects memory, and the rest compiles while you play.",
         {{"0", "Nenhum (cache novo)", "None (new cache)"}, {"128", "Até ~4 MiB", "Up to ~4 MiB"}, {"256", "Até ~8 MiB", "Up to ~8 MiB"}, {"1024", "Até ~32 MiB", "Up to ~32 MiB"}, {"all", "Sem limite (arriscado)", "No limit (risky)"}}, 2},
        {"shader_notice", Group::Perf, "Aviso de shaders e memória", "Shader and memory notice",
         "Mostra no canto da tela quantos shaders estão sendo compilados (como o Cemu) e avisa quando a memória direta está quase no fim. Também liga e desliga pelo menu L3 + R3.",
         "Shows in the corner of the screen how many shaders are being compiled (like Cemu) and warns when direct memory is nearly gone. Also toggled from the L3 + R3 menu.",
         {{"on", "Ligado", "On"}, {"off", "Desligado", "Off"}}, 0},
        {"driver_cache", Group::Perf, "Cache de pipelines do driver", "Driver pipeline cache",
         "O arquivo de cache do próprio driver de vídeo (vulkan_pipelines.bin). Desligar economiza memória e disco, e descarta uma fonte possível de falhas ao carregar shaders; os shaders compilam do zero.",
         "The video driver's own cache file (vulkan_pipelines.bin). Turning it off saves memory and disk and rules out one possible source of crashes while loading shaders; shaders compile from scratch.",
         {{"on", "Ligado", "On"}, {"off", "Desligado", "Off"}}, 0},
        {"dyna_state", Group::Video, "Estado dinâmico do Vulkan", "Vulkan dynamic state",
         "Nível das extensões de estado dinâmico (0 a 3). Se o driver falhar, tente um valor menor.",
         "Level of the dynamic-state extensions (0 to 3). If the driver fails, try a lower value.",
         {{"0", "0", "0"}, {"1", "1", "1"}, {"2", "2", "2"}, {"3", "3", "3"}}, 2},
        {"vertex_input_dynamic", Group::Video, "Entrada de vértices dinâmica", "Dynamic vertex input",
         "Extensão do Vulkan. Desligue se houver falhas gráficas ou travamentos.",
         "Vulkan extension. Turn it off if you get graphics glitches or hangs.",
         {{"on", "Ligado", "On"}, {"off", "Desligado", "Off"}}, 0},
        // From Eden's own graphics screen (0.7.4). Each is applied only if this Eden has the setting.
        {"vsync", Group::Video, "Modo de VSync", "VSync mode",
         "Como os quadros vão para a tela. FIFO (ligado) não rasga a imagem. Imediato e Caixa de entrada dependem do driver de vídeo do PS4, que pode ignorá-los: se nada mudar, veja o boot.log.",
         "How frames go to the screen. FIFO (on) does not tear. Immediate and Mailbox depend on the PS4 video driver, which may ignore them: if nothing changes, see boot.log.",
         {{"fifo", "FIFO (ligado)", "FIFO (on)"}, {"mailbox", "Caixa de entrada", "Mailbox"}, {"immediate", "Imediato (desligado)", "Immediate (off)"},
          {"relaxed", "FIFO relaxado", "Relaxed FIFO"}}, 0},
        {"scaling_filter", Group::Video, "Filtro de adaptação da janela", "Window adapting filter",
         "Filtro que ajusta a imagem do jogo ao tamanho da tela (importa quando a resolução interna não é a da tela). Bilinear é o padrão e o mais leve; Bicúbico, FSR e os outros pesam mais na GPU.",
         "The filter that fits the game image to the screen size (it matters when the internal resolution is not the screen's). Bilinear is the default and the lightest; Bicubic, FSR and the others cost more GPU.",
         {{"bilinear", "Bilinear", "Bilinear"}, {"nearest", "Vizinho mais próximo", "Nearest neighbour"}, {"bicubic", "Bicúbico", "Bicubic"},
          {"gaussian", "Gaussiano", "Gaussian"}, {"scaleforce", "ScaleForce", "ScaleForce"}, {"fsr", "AMD FidelityFX Super Resolution", "AMD FidelityFX Super Resolution"},
          {"area", "Área", "Area"}}, 0},
        {"anti_aliasing", Group::Video, "Anti-aliasing", "Anti-aliasing",
         "Suaviza as bordas serrilhadas. FXAA é leve; SMAA é melhor e pesa mais. Custa GPU, que no PS4 é pouca.",
         "Smooths jagged edges. FXAA is light; SMAA looks better and costs more. It costs GPU, which the PS4 has little of.",
         {{"none", "Nenhum", "None"}, {"fxaa", "FXAA", "FXAA"}, {"smaa", "SMAA", "SMAA"}}, 0},
        {"anisotropy", Group::Video, "Filtragem anisotrópica", "Anisotropic filtering",
         "Melhora a qualidade das texturas vistas de ângulo. Padrão deixa o jogo decidir; 16x é o mais nítido.",
         "Improves textures seen at an angle. Default lets the game decide; 16x is the sharpest.",
         {{"default", "Padrão", "Default"}, {"auto", "Automático", "Auto"}, {"2", "2x", "2x"}, {"4", "4x", "4x"}, {"8", "8x", "8x"}, {"16", "16x", "16x"}}, 0},
        {"dma_accuracy", Group::Video, "Precisão do DMA", "DMA accuracy",
         "Controla as operações de DMA. Segura pode corrigir problemas em alguns jogos, mas pode pesar. Na dúvida, deixe em Padrão.",
         "Controls DMA operations. Safe can fix problems in some games but may cost speed. If unsure, keep Default.",
         {{"default", "Padrão", "Default"}, {"unsafe", "Insegura", "Unsafe"}, {"safe", "Segura", "Safe"}}, 0},
        {"sync_memory", Group::Video, "Sincronizar operações de memória", "Sync memory operations",
         "Garante a consistência dos dados entre operações de computação e memória. Pode corrigir problemas em alguns jogos (Unreal Engine 4 é o mais afetado), mas reduz o desempenho.",
         "Keeps data consistent between compute and memory operations. It can fix problems in some games (Unreal Engine 4 is the most affected) but lowers performance.",
         {{"off", "Desligado", "Off"}, {"on", "Ligado", "On"}}, 0},
        {"speed_percent", Group::Video, "Porcentagem do limite de velocidade", "Speed limit percentage",
         "Velocidade máxima da emulação quando o limite de velocidade está ligado. 100% é o normal; menos deixa o jogo mais lento, mais deixa mais rápido.",
         "Top emulation speed while the speed limit is on. 100% is normal; lower slows the game, higher speeds it up.",
         {{"50", "50%", "50%"}, {"75", "75%", "75%"}, {"100", "100%", "100%"}, {"150", "150%", "150%"}, {"200", "200%", "200%"}}, 2},
        // ---------------------------------------------------------------- CPU and memory
        {"cpu_accuracy", Group::Cpu, "Precisão da CPU", "CPU accuracy",
         "Rápida usa ponto flutuante reduzido (mais velocidade). Automática e Precisa são mais fiéis e mais lentas.",
         "Fast uses reduced-precision floating point (more speed). Auto and Accurate are more faithful and slower.",
         {{"unsafe", "Rápida", "Fast"}, {"auto", "Automática", "Auto"}, {"accurate", "Precisa", "Accurate"}}, 0},
        {"multicore", Group::Cpu, "Multinúcleo", "Multicore",
         "Emula os 4 núcleos da CPU do Switch em threads separadas. Desligar é bem mais lento.",
         "Emulates the Switch's 4 CPU cores on separate threads. Turning it off is much slower.",
         {{"on", "Ligado", "On"}, {"off", "Desligado", "Off"}}, 0},
        {"vram", Group::Cpu, "Uso de VRAM", "VRAM usage",
         "Conservador usa menos memória de vídeo. Agressivo guarda mais texturas e pode faltar memória.",
         "Conservative uses less video memory. Aggressive keeps more textures and may run out.",
         {{"conservative", "Conservador", "Conservative"}, {"aggressive", "Agressivo", "Aggressive"}}, 0},
        {"env:ORBIS_ARENA_MIB", Group::Cpu, "Arena do driver de vídeo", "Video driver arena",
         "Memória reservada ao driver (MiB). Mais evita erros de memória de vídeo, mas sobra menos para o jogo.",
         "Memory reserved for the driver (MiB). More avoids video-memory errors but leaves less for the game.",
         {{"1024", "1024 MiB", "1024 MiB"}, {"1280", "1280 MiB", "1280 MiB"}, {"1536", "1536 MiB", "1536 MiB"}, {"1792", "1792 MiB", "1792 MiB"}}, 1},
        {"env:ORBIS_VRAM_GARLIC", Group::Cpu, "Texturas dentro da arena", "Textures inside the arena",
         "Experimental, último recurso: mantém imagens na arena do driver em vez de memória extra. Gasta menos memória direta, mas o driver avisa que custa velocidade de GPU.",
         "Experimental, last resort: keeps images in the driver's arena instead of extra memory. Uses less direct memory, but the driver warns it costs GPU speed.",
         {{"1", "Desligado (padrão)", "Off (default)"}, {"0", "Ligado (experimental)", "On (experimental)"}}, 0},
        {"skip_cpu_invalidation", Group::Cpu, "Ignorar invalidação interna da CPU", "Skip inner CPU invalidation",
         "Ignora certas invalidações de cache do lado da CPU durante atualizações de memória: usa menos CPU (o ponto fraco do PS4), mas pode causar falhas ou travamentos em alguns jogos. Experimental.",
         "Skips some CPU-side cache invalidations during memory updates: uses less CPU (the PS4's weak point) but may cause glitches or hangs in some games. Experimental.",
         {{"off", "Desligado", "Off"}, {"on", "Ligado (experimental)", "On (experimental)"}}, 0},
        // ---------------------------------------------------------------- Advanced
        {"bgra", Group::Advanced, "Correção de cor B8G8R8A8", "B8G8R8A8 colour fix",
         "Corrige vermelho e azul trocados nas texturas. Se as cores do jogo estiverem trocadas (céu rosa, chão azul), use Automático ou Desligado.",
         "Fixes swapped red and blue in textures. If a game's colours are swapped (pink sky, blue ground), use Auto or Off.",
         {{"auto", "Automático", "Auto"}, {"on", "Ligado", "On"}, {"off", "Desligado", "Off"}}, 0},
        {"a2b10", Group::Advanced, "Correção A2B10G10R10", "A2B10G10R10 fix",
         "Correção de texturas de 10 bits, decidida por um teste ao iniciar.",
         "Fix for 10-bit textures, decided by a test at startup.",
         {{"auto", "Automático", "Auto"}, {"off", "Desligado", "Off"}}, 0},
        {"rg", Group::Advanced, "Correção R8G8", "R8G8 fix",
         "Corrige vermelho e verde trocados em texturas de 2 canais. Automático decide por um teste.",
         "Fixes swapped red and green in 2-channel textures. Auto decides from a test.",
         {{"auto", "Automático", "Auto"}, {"on", "Ligado", "On"}, {"off", "Desligado", "Off"}}, 0},
        {"swizzle", Group::Advanced, "Teste de mapeamento", "Mapping self-test",
         "Roda um teste de componentes no início. Só para diagnóstico.",
         "Runs a component-mapping test at startup. Diagnostics only.",
         {{"off", "Desligado", "Off"}, {"test", "Rodar teste", "Run test"}}, 0},
        {"colour_swap", Group::Advanced, "Trocar vermelho e azul na tela", "Swap red and blue on the screen",
         "Troca vermelho e azul na saída de vídeo, para o céu azul não sair rosa. Desligue se as cores já estiverem certas.",
         "Swaps red and blue in the video output, so the blue sky does not come out pink. Turn it off if the colours are already right.",
         {{"on", "Ligado", "On"}, {"off", "Desligado", "Off"}}, 0},
        {"hud", Group::Perf, "HUD de desempenho", "Performance HUD",
         "Mostra FPS, núcleos de CPU em uso, memória e temperatura sobre o jogo. Também liga e desliga pelo menu L3 + R3.",
         "Shows FPS, CPU cores in use, memory and temperature over the game. Also toggled from the L3 + R3 menu.",
         {{"off", "Desligado", "Off"}, {"on", "Ligado", "On"}}, 0},
        {"hud_size", Group::Perf, "Tamanho do HUD", "HUD size",
         "Normal ou pequeno (metade do tamanho).", "Normal or small (half the size).",
         {{"normal", "Normal", "Normal"}, {"small", "Pequeno", "Small"}}, 0},
        {"hud_position", Group::Perf, "Posição do HUD", "HUD position",
         "Canto de cima da tela onde o HUD aparece.", "The top corner of the screen the HUD is drawn in.",
         {{"left", "Esquerda", "Left"}, {"right", "Direita", "Right"}}, 0},
        {"overlay", Group::Perf, "Sobreposição de vídeo", "Video overlay",
         "O gancho do vídeo: HUD, menu L3 + R3 e troca de cores. Desligue se um jogo não abrir e você suspeitar dele.",
         "The video hook: HUD, L3 + R3 menu and colour swap. Turn it off if a game will not open and you suspect it.",
         {{"on", "Ligada", "On"}, {"off", "Desligada", "Off"}}, 0},
        {"overlay_draw", Group::Perf, "Desenhar HUD e menu sobre o jogo", "Draw the HUD and menu over the game",
         "Desligado deixa só a troca de cores: o HUD e o menu L3 + R3 não aparecem. Use para ver se o desenho sobre o jogo causa tela preta ou falhas gráficas.",
         "Off keeps only the colour swap: the HUD and the L3 + R3 menu do not show. Use it to see whether drawing over the game causes a black screen or glitches.",
         {{"on", "Ligado", "On"}, {"off", "Desligado", "Off"}}, 0},
        {"overlay_delay", Group::Perf, "Espera antes de desenhar o HUD (ms)", "Wait before drawing the HUD (ms)",
         "Tempo que a GPU tem para terminar o quadro antes de o HUD e o menu serem desenhados nele. Se o HUD ainda piscar, aumente; se ele aparecer atrasado, diminua.",
         "How long the GPU gets to finish a frame before the HUD and menu are drawn into it. If the HUD still flickers, raise it; if it shows late, lower it.",
         {{"0", "0", "0"}, {"1", "1", "1"}, {"2", "2", "2"}, {"4", "4", "4"}, {"8", "8", "8"}}, 2},
        {"profile", Group::Advanced, "Perfilador no log", "Profiler in the log",
         "Registra no boot.log onde a CPU gasta tempo. Útil para relatar problemas.",
         "Records in boot.log where the CPU spends time. Useful for bug reports.",
         {{"on", "Ligado", "On"}, {"off", "Desligado", "Off"}}, 0},
        {"auto_stub", Group::Advanced, "Usar Auto Stub", "Use auto stub",
         "Preenche automaticamente serviços e funções que faltam. Pode ajudar um jogo que para por falta de um serviço, mas também causa falhas e instabilidade. Só para testar.",
         "Fills in missing services and functions automatically. It can help a game that stops for lack of a service, but also causes crashes and instability. For testing only.",
         {{"off", "Desligado", "Off"}, {"on", "Ligado (experimental)", "On (experimental)"}}, 0},
        {"region", Group::Advanced, "Região da emulação", "Emulation region",
         "A região do console que o jogo enxerga. Use a do seu jogo se ele reclamar de região ou abrir com idioma ou conteúdo errados.",
         "The console region the game sees. Use your game's own if it complains about region or opens with the wrong language or content.",
         {{"japan", "Japão", "Japan"}, {"usa", "EUA", "USA"}, {"europe", "Europa", "Europe"}, {"australia", "Austrália", "Australia"},
          {"china", "China", "China"}, {"korea", "Coreia", "Korea"}, {"taiwan", "Taiwan", "Taiwan"}}, 1},
        // ---------------------------------------------------------------- System
        {"language", Group::System, "Idioma do menu", "Menu language",
         "Idioma dos textos deste menu.", "Language of this menu's texts.",
         {{"pt", "Português", "Português"}, {"en", "English", "English"}}, 0},
    };
    return catalog;
}

const std::vector<Preset>& Presets() {
    // These name only what each option means (more speed, or more fidelity). They are ways to try a
    // game quickly, not a promise that it will open.
    static const std::vector<Preset> presets = {
        {"Padrão", "Default",
         "Todas as opções de vídeo e CPU nos valores desta versão.",
         "Every video and CPU option at this version's values.",
         {}},
        {"Leve (mais velocidade)", "Light (more speed)",
         "Resolução 0,75x, GPU de precisão baixa, sem flush reativo, shaders assíncronos, ASTC pela CPU e VRAM conservadora. Menos qualidade, mais velocidade.",
         "0.75x resolution, low GPU accuracy, no reactive flushing, async shaders, ASTC on the CPU and conservative VRAM. Less quality, more speed.",
         {{"bgra", "auto"}, {"resolution", "0.75"}, {"gpu_accuracy", "low"}, {"reactive_flushing", "off"}, {"async_shaders", "on"},
          {"astc", "cpu"}, {"vram", "conservative"}}},
        {"Compatibilidade (mais fiel)", "Compatibility (more faithful)",
         "GPU de precisão alta, flush reativo, CPU automática, sem estado dinâmico nem entrada de vértices dinâmica, shaders síncronos. Mais lento, usa menos extensões arriscadas do driver.",
         "High GPU accuracy, reactive flushing, auto CPU accuracy, no dynamic state or dynamic vertex input, synchronous shaders. Slower, uses fewer risky driver extensions.",
         {{"bgra", "auto"}, {"gpu_accuracy", "high"}, {"reactive_flushing", "on"}, {"cpu_accuracy", "auto"}, {"dyna_state", "0"},
          {"vertex_input_dynamic", "off"}, {"async_shaders", "off"}}},
        {"Economia de RAM", "Save RAM",
         "Resolução 0,75x, arena de vídeo de 1024 MiB, VRAM conservadora, cache de shaders limitado a ~4 MiB e sem cache do driver. Menos memória usada, para jogos que não abrem ou travam por falta dela.",
         "0.75x resolution, 1024 MiB video arena, conservative VRAM, shader cache limited to ~4 MiB and no driver cache. Less memory used, for games that do not open or hang for lack of it.",
         {{"resolution", "0.75"}, {"env:ORBIS_ARENA_MIB", "1024"}, {"vram", "conservative"}, {"shader_preload", "128"}, {"driver_cache", "off"},
          {"astc", "cpu"}}},
        {"Mais memória de vídeo", "More video memory",
         "Arena de vídeo de 1792 MiB e cache de shaders de até ~32 MiB. Sobra menos memória para o jogo: use se aparecer erro de memória de vídeo.",
         "1792 MiB video arena and a shader cache of up to ~32 MiB. Less memory is left for the game: use it if video-memory errors show up.",
         {{"env:ORBIS_ARENA_MIB", "1792"}, {"shader_preload", "1024"}}},
    };
    return presets;
}

const Option* FindOption(std::string_view key) {
    for (const Option& option : Catalog()) {
        if (key == option.key) {
            return &option;
        }
    }
    return nullptr;
}

SettingsFile::Line SettingsFile::Parse(const std::string& raw) {
    Line line{raw, {}, {}};
    // The same reading as main.cpp's ApplySettingsFile: no '\r', no comment, no spaces.
    std::string text = raw;
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    if (const auto hash = text.find('#'); hash != std::string::npos) {
        text.erase(hash);
    }
    text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
    const auto eq = text.find('=');
    if (text.empty() || eq == std::string::npos || eq == 0) {
        return line;
    }
    line.key = text.substr(0, eq);
    line.value = text.substr(eq + 1);
    if (line.key == "env") {
        const auto split = line.value.find('=');
        if (split == std::string::npos || split == 0) {
            line.key.clear();
            line.value.clear();
        } else {
            line.key = "env:" + line.value.substr(0, split);
            line.value = line.value.substr(split + 1);
        }
    }
    return line;
}

SettingsFile::SettingsFile(const std::string& text) {
    size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const std::string raw = text.substr(start, end == std::string::npos ? end : end - start);
        lines_.push_back(Parse(raw));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
}

std::string SettingsFile::Text() const {
    std::string out;
    for (const Line& line : lines_) {
        out += line.raw;
        out += '\n';
    }
    return out;
}

std::string SettingsFile::Get(std::string_view key) const {
    for (const Line& line : lines_) {
        if (line.key == key) {
            return line.value;
        }
    }
    return {};
}

void SettingsFile::Set(std::string_view key, std::string_view value) {
    std::string raw;
    if (key.substr(0, 4) == "env:") {
        raw = "env=" + std::string(key.substr(4)) + "=" + std::string(value);
    } else {
        raw = std::string(key) + "=" + std::string(value);
    }
    for (Line& line : lines_) {
        if (line.key == key) {
            line = Parse(raw);
            return;
        }
    }
    if (lines_.empty()) {
        lines_.push_back(Parse(HeaderComment));
    }
    lines_.push_back(Parse(raw));
}

void SettingsFile::Erase(std::string_view key) {
    lines_.erase(std::remove_if(lines_.begin(), lines_.end(),
                                [&](const Line& line) { return line.key == key; }),
                 lines_.end());
}

int SettingsFile::ChoiceIndex(const Option& option) const {
    const std::string value = Get(option.key);
    for (size_t i = 0; i < option.choices.size(); ++i) {
        if (value == option.choices[i].value) {
            return int(i);
        }
    }
    return option.default_index;
}

void SettingsFile::SetChoice(const Option& option, int index) {
    if (index == option.default_index) {
        Erase(option.key);
    } else {
        Set(option.key, option.choices[size_t(index)].value);
    }
}

bool SettingsFile::IsDefault(const Option& option) const {
    return ChoiceIndex(option) == option.default_index;
}

void SettingsFile::ResetAll() {
    for (const Option& option : Catalog()) {
        if (option.group != Group::System) {
            Erase(option.key);
        }
    }
}

namespace {

/// Index of `value` among the option's choices; the default when it is not one of them.
int ChoiceOf(const Option& option, std::string_view value) {
    for (size_t i = 0; i < option.choices.size(); ++i) {
        if (value == option.choices[i].value) {
            return int(i);
        }
    }
    return option.default_index;
}

} // Anonymous namespace

int SettingsFile::MatchPreset() const {
    // Every option any profile touches must be what the profile says, or its default.
    const auto& presets = Presets();
    for (size_t p = 0; p < presets.size(); ++p) {
        bool matches = true;
        for (const Preset& any : presets) {
            for (const auto& [key, ignored] : any.values) {
                (void)ignored;
                const Option* option = FindOption(key);
                int expected = option->default_index;
                for (const auto& [pk, pv] : presets[p].values) {
                    if (std::string_view(pk) == key) {
                        expected = ChoiceOf(*option, pv);
                    }
                }
                matches = matches && ChoiceIndex(*option) == expected;
            }
        }
        if (matches) {
            return int(p);
        }
    }
    return -1;
}

void SettingsFile::ApplyPreset(int index) {
    const auto& presets = Presets();
    for (const Preset& any : presets) {
        for (const auto& [key, ignored] : any.values) {
            (void)ignored;
            Erase(key);
        }
    }
    for (const auto& [key, value] : presets[size_t(index)].values) {
        const Option* option = FindOption(key);
        SetChoice(*option, ChoiceOf(*option, value));
    }
}

Lang SettingsFile::Language() const {
    return Get("language") == "en" ? Lang::En : Lang::Pt;
}

} // namespace Menu
