/*
 * Multi-Slab Bare-Metal Allocator Engine
 * Copyright (C) 2026 Jayesh Kumar Das
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * benchmark.cpp - Standalone microbenchmark for Arena.hpp's hot-path
 * operations (slab_alloc / slab_dealloc), isolated from terminal I/O,
 * TUI rendering, and single-shot clock-read noise.
 *
 * The TUI's "Last Op Execution Latency" times one clock-read -> op ->
 * clock-read window per keypress, which also includes I/O and clock
 * overhead and reports only one sample -- it swings from ~0.2us to
 * ~2.8us for the same operation purely from scheduler/clock jitter.
 * This harness times tens of thousands of calls per run, many runs,
 * and reports min/median/mean/max in ns: min is the closest thing to
 * a true best-case cost (noise can only inflate a run, never make it
 * faster than the real work took); median shows the typical case.
 *
 * BUILD (release, matches what the TUI ships):
 *   g++ -std=c++20 -O2 -DNDEBUG benchmark.cpp -o benchmark
 * Debug-assert cost:
 *   g++ -std=c++20 -O2 benchmark.cpp -o benchmark_debug
 * ASan cost:
 *   g++ -std=c++20 -O2 -DNDEBUG -fsanitize=address -g benchmark.cpp -o benchmark_asan
 *
 * RUN:
 *   ./benchmark [iterations-per-run]      (default: 100000)
 */

#include "Arena.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {

// volatile forces every write to be an observable side effect, so the
// optimizer can't prove the loop body is dead and remove it.
volatile uintptr_t g_sink = 0;

using Clock = std::chrono::steady_clock;

double ns_per_call(Clock::time_point start, Clock::time_point end, size_t iterations) {
    double total_ns = std::chrono::duration<double, std::nano>(end - start).count();
    return total_ns / static_cast<double>(iterations);
}

template <typename Fn>
std::vector<double> repeated_runs(size_t repeats, Fn&& fn) {
    std::vector<double> results;
    results.reserve(repeats);
    for (size_t r = 0; r < repeats; ++r) {
        results.push_back(fn());
    }
    std::sort(results.begin(), results.end());
    return results;
}

void report(const char* label, const std::vector<double>& ns) {
    double min_ns    = ns.front();
    double max_ns     = ns.back();
    double median_ns = ns[ns.size() / 2];
    double mean_ns    = std::accumulate(ns.begin(), ns.end(), 0.0) / static_cast<double>(ns.size());

    std::cout << label << ":\n"
              << "  min:    " << min_ns    << " ns/op\n"
              << "  median: " << median_ns << " ns/op\n"
              << "  mean:   " << mean_ns   << " ns/op\n"
              << "  max:    " << max_ns    << " ns/op  (expect scheduler noise here, not real cost)\n\n";
}

} // namespace

int main(int argc, char** argv) {
    size_t iterations = 100000;
    if (argc > 1) {
        try {
            iterations = static_cast<size_t>(std::stoul(argv[1]));
        } catch (const std::exception&) {
            std::cerr << "Invalid iteration count, using default of " << iterations << ".\n";
        }
    }
    const size_t repeats = 15;

    std::cout << "=== Arena.hpp hot-path microbenchmark ===\n";
    std::cout << "Iterations per run: " << iterations << "\n";
    std::cout << "Runs per measurement: " << repeats << "\n\n";

#ifdef NDEBUG
    std::cout << "Build: release (NDEBUG defined) -- thread-owner asserts compiled out.\n";
#else
    std::cout << "Build: debug (NDEBUG not defined) -- thread-owner asserts ACTIVE.\n";
    std::cout << "       Rebuild with -DNDEBUG for a true release-path measurement.\n";
#endif
#if defined(ARENA_ASAN_ENABLED)
    std::cout << "ASan:  ENABLED -- poison/unpoison calls are active, expect higher latency.\n";
#else
    std::cout << "ASan:  disabled.\n";
#endif
    std::cout << "\n";

    try {
        const size_t block_size = 64;
        const size_t bulk_blocks = iterations + 64;
        const size_t bulk_bytes = bulk_blocks * block_size + 4096;

        ScopedMultiSlabManager scoped_mgr(2);
        MultiSlabManager* mgr = scoped_mgr.get();

        manager_add_pool_strict(mgr, nullptr, bulk_bytes, block_size);
        SlabPool* bulk_pool = &mgr->pools[0];

        manager_add_pool_strict(mgr, nullptr, 4096, block_size);
        SlabPool* churn_pool = &mgr->pools[1];

        std::vector<void*> ptrs(iterations);

        // Reset happens outside start/end so only the pop itself is timed.
        auto alloc_results = repeated_runs(repeats, [&]() -> double {
            for (size_t i = 0; i < iterations; ++i) {
                if (ptrs[i]) slab_dealloc(bulk_pool, ptrs[i]);
            }
            auto start = Clock::now();
            for (size_t i = 0; i < iterations; ++i) {
                ptrs[i] = slab_alloc(bulk_pool);
                g_sink ^= reinterpret_cast<uintptr_t>(ptrs[i]);
            }
            auto end = Clock::now();
            return ns_per_call(start, end, iterations);
        });
        report("slab_alloc (isolated)", alloc_results);

        auto dealloc_results = repeated_runs(repeats, [&]() -> double {
            for (size_t i = 0; i < iterations; ++i) {
                ptrs[i] = slab_alloc(bulk_pool);
            }
            auto start = Clock::now();
            for (size_t i = 0; i < iterations; ++i) {
                slab_dealloc(bulk_pool, ptrs[i]);
            }
            auto end = Clock::now();
            return ns_per_call(start, end, iterations);
        });
        report("slab_dealloc (isolated)", dealloc_results);

        // Alloc immediately followed by dealloc of the same block --
        // the pattern of sustained real usage.
        auto churn_results = repeated_runs(repeats, [&]() -> double {
            auto start = Clock::now();
            for (size_t i = 0; i < iterations; ++i) {
                void* p = slab_alloc(churn_pool);
                g_sink ^= reinterpret_cast<uintptr_t>(p);
                slab_dealloc(churn_pool, p);
            }
            auto end = Clock::now();
            return ns_per_call(start, end, iterations);
        });
        report("alloc+dealloc round-trip (steady-state churn, per pair)", churn_results);

        std::cout << "Sanity check: isolated alloc median + isolated dealloc median = "
                  << (alloc_results[alloc_results.size() / 2] + dealloc_results[dealloc_results.size() / 2])
                  << " ns, vs round-trip median = " << churn_results[churn_results.size() / 2]
                  << " ns (these should be roughly comparable).\n\n";

        std::cout << "(sink=" << g_sink << ", printed only to keep the optimizer honest)\n";

    } catch (const std::exception& e) {
        std::cerr << "Benchmark setup failed: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
