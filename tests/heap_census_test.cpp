// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for frontend/heap_census.cpp (runs on a PC / in CI, not on the console).
//
// Checks that the live table counts big allocations exactly through malloc, realloc and free, that
// the free() fast path never loses or invents a record, and (with --bench) how much the lock-free
// filter saves under thread contention.
//
//   W="-Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free -Wl,--wrap=posix_memalign -Wl,--wrap=aligned_alloc"
//   g++ -O2 -std=c++20 -fno-omit-frame-pointer -pthread tests/heap_census_test.cpp frontend/heap_census.cpp -o heap_census_test $W
//   ./heap_census_test            # exit code 0 = all good
//   ./heap_census_test --bench 6  # 6 threads of small malloc/free

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

extern "C" void Ps4HeapCensus();

namespace Ps4 {
namespace {
char g_first_line[256];
bool g_have_line = false;
} // namespace

// heap_census.cpp logs through this; keep only the first line ("heap census: N MiB live ...").
void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char line[256];
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (!g_have_line) {
        std::strcpy(g_first_line, line);
        g_have_line = true;
    }
}
const char* FirstLine() {
    return g_first_line;
}
void Reset() {
    g_have_line = false;
}
} // namespace Ps4

namespace {

int g_failures = 0;

/// Live MiB the census reports right now.
unsigned LiveMiB() {
    Ps4::Reset();
    Ps4HeapCensus();
    unsigned mib = ~0u;
    std::sscanf(Ps4::FirstLine(), "heap census: %u MiB live", &mib);
    return mib;
}

void Expect(const char* what, unsigned want) {
    const unsigned got = LiveMiB();
    std::printf("%-44s expected %3u MiB, census says %3u MiB  %s\n", what, want, got,
                got == want ? "ok" : "FAIL");
    if (got != want) {
        ++g_failures;
    }
}

// The optimiser may delete a malloc/free pair whose pointer is never used; touch the block.
void* Big(size_t bytes) {
    void* p = std::malloc(bytes);
    static_cast<volatile char*>(p)[0] = 1;
    asm volatile("" ::"r"(p) : "memory");
    return p;
}

void Correctness() {
    Expect("nothing recorded yet", 0);
    std::vector<void*> big;
    for (int i = 0; i < 10; ++i) {
        big.push_back(Big(2u << 20));
    }
    Expect("10 blocks of 2 MiB", 20);
    for (int i = 0; i < 4; ++i) {
        std::free(big[i]);
    }
    Expect("4 of them freed", 12);
    big[4] = std::realloc(big[4], 3u << 20);
    static_cast<volatile char*>(big[4])[0] = 1;
    Expect("one grown by realloc (2 -> 3 MiB)", 13);
    for (int i = 4; i < 10; ++i) {
        std::free(big[i]);
    }
    Expect("all freed", 0);

    // Small pointers (the fast path) must not disturb a recorded block, however many are freed.
    void* keep = Big(5u << 20);
    for (int i = 0; i < 200000; ++i) {
        std::free(std::malloc(16 + (i % 300)));
    }
    Expect("one 5 MiB block among 200k small frees", 5);
    std::free(keep);
    Expect("and freed", 0);

    // Thousands of recorded blocks (filter slots shared by several pointers).
    std::vector<void*> many;
    for (int i = 0; i < 3000; ++i) {
        many.push_back(Big(1u << 20));
    }
    Expect("3000 blocks of 1 MiB", 3000);
    for (size_t i = 0; i < many.size(); i += 2) {
        std::free(many[i]);
    }
    Expect("every second one freed", 1500);
    for (size_t i = 1; i < many.size(); i += 2) {
        std::free(many[i]);
    }
    Expect("all freed again", 0);
}

void Bench(int threads) {
    constexpr int Iterations = 3000000;
    std::atomic<bool> go{false};
    auto work = [&](int id) {
        while (!go.load()) {
        }
        std::vector<void*> ring(64, nullptr);
        for (int i = 0; i < Iterations; ++i) {
            std::free(ring[i & 63]);
            ring[i & 63] = std::malloc(32 + static_cast<size_t>((i * 7 + id) % 400));
        }
        for (void* p : ring) {
            std::free(p);
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back(work, t);
    }
    const auto start = std::chrono::steady_clock::now();
    go.store(true);
    for (auto& t : pool) {
        t.join();
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("bench: %d threads x %d malloc/free pairs: %.0f ms\n", threads, Iterations, ms);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {
        Bench(argc > 2 ? std::atoi(argv[2]) : 6);
        return 0;
    }
    Correctness();
    std::printf(g_failures == 0 ? "heap census test: PASSED\n" : "heap census test: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
