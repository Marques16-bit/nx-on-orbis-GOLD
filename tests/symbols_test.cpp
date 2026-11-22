// SPDX-License-Identifier: GPL-3.0-or-later
// Host test of frontend/symbols.h: build with `g++ -std=c++20 -Ifrontend -Itests tests/symbols_test.cpp -o symbols_test`.
#include <cstdio>
#include <cstdlib>

#include "symbols_checks.h"

namespace {
int g_failed = 0;
int g_checks = 0;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED: %s\n", what);
    }
}
} // namespace

int main() {
    SymbolsChecks::Run(&Check);
    if (g_failed != 0) {
        std::printf("symbols test: %d of %d checks FAILED\n", g_failed, g_checks);
        return 1;
    }
    std::printf("symbols test: %d checks, PASSED\n", g_checks);
    return 0;
}
