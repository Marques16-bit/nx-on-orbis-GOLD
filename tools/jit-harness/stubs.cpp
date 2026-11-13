// What Dynarmic's x64 backend needs from the rest of Eden to link on a PC: the assert hooks, the log sink and the x64 disassembler.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <fmt/format.h>

void AssertFailSoftImpl() { std::fprintf(stderr, "ASSERT soft\n"); }
[[noreturn]] void AssertFatalImpl() { std::fprintf(stderr, "ASSERT fatal\n"); std::abort(); }

namespace Dynarmic::Common {
std::string DisassembleX64(const void*, const void*) { return {}; }
}

namespace Common::Log {
enum class Class : unsigned char {};
enum class Level : unsigned char {};
void FmtLogMessageImpl(Class, Level, const char*, unsigned int, const char* fn, fmt::string_view f, const fmt::format_args& a) {
    std::fprintf(stderr, "LOG %s: %s\n", fn, fmt::vformat(f, a).c_str());
}
}
