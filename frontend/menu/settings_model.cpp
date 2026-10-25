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
         {{"0.5", "0,5x", "0.5x"}, {"0.75", "0,75x", "0.75x"}, {"1", "1x", "1x"}, {"1.5", "1,5x", "1.5x"}, {"2", "2x", "2x"}}, 2},
        {"speed_limit", Group::Video, "Limitar velocidade", "Speed limit",
         "Desligado deixa o jogo rodar o mais rápido possível (ele pode acelerar).",
         "Off lets the game run as fast as it can (it may speed up).",
         {{"on", "Ligado", "On"}, {"off", "Desligado", "Off"}}, 0},
        {"gpu_accuracy", Group::Video, "Precisão da GPU", "GPU accuracy",
         "Baixa é mais rápida (menos sincronizações). Alta corrige efeitos em alguns jogos, mas pesa.",
         "Low is faster (fewer syncs). High fixes effects in some games but is heavier.",
         {{"low", "Baixa (rápida)", "Low (fast)"}, {"high", "Alta", "High"}}, 0},
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
        {"dyna_state", Group::Video, "Estado dinâmico do Vulkan", "Vulkan dynamic state",
         "Nível das extensões de estado dinâmico (0 a 3). Se o driver falhar, tente um valor menor.",
         "Level of the dynamic-state extensions (0 to 3). If the driver fails, try a lower value.",
         {{"0", "0", "0"}, {"1", "1", "1"}, {"2", "2", "2"}, {"3", "3", "3"}}, 2},
        {"vertex_input_dynamic", Group::Video, "Entrada de vértices dinâmica", "Dynamic vertex input",
         "Extensão do Vulkan. Desligue se houver falhas gráficas ou travamentos.",
         "Vulkan extension. Turn it off if you get graphics glitches or hangs.",
         {{"on", "Ligado", "On"}, {"off", "Desligado", "Off"}}, 0},
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
        // ---------------------------------------------------------------- Advanced
        {"bgra", Group::Advanced, "Correção de cor B8G8R8A8", "B8G8R8A8 colour fix",
         "Corrige vermelho e azul trocados nas texturas. Automático decide por um teste ao iniciar.",
         "Fixes swapped red and blue in textures. Auto decides from a test at startup.",
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
        {"profile", Group::Advanced, "Perfilador no log", "Profiler in the log",
         "Registra no boot.log onde a CPU gasta tempo. Útil para relatar problemas.",
         "Records in boot.log where the CPU spends time. Useful for bug reports.",
         {{"on", "Ligado", "On"}, {"off", "Desligado", "Off"}}, 0},
        // ---------------------------------------------------------------- System
        {"language", Group::System, "Idioma do menu", "Menu language",
         "Idioma dos textos deste menu.", "Language of this menu's texts.",
         {{"pt", "Português", "Português"}, {"en", "English", "English"}}, 0},
    };
    return catalog;
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

Lang SettingsFile::Language() const {
    return Get("language") == "en" ? Lang::En : Lang::Pt;
}

} // namespace Menu
