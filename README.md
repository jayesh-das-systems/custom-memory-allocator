# custom-memory-allocator
# POSIX Multi-Slab Allocator

A header-only C++20 slab allocator backed by `mmap`. Designed for bounded-latency allocation in single-threaded hot paths. Includes an ncurses TUI for visualizing pool fragmentation and block state.

**Author:** Jayesh Kumar Das  
**License:** GNU GPLv3  
**Platform:** Linux (x86_64)

---

## Technical Overview

* **O(1) Hot Path:** Allocation and deallocation operate in constant time with zero metadata traversal. Free blocks are tracked via an intrusive singly-linked list embedded directly in unallocated memory.
* **Double-Free Detection:** A fixed per-pool bitset provides O(1) occupancy queries and prevents double-free corruption.
* **ASan Poisoning:** Integrates with `sanitizer/asan_interface.h` to poison freed memory blocks and unpoison active allocations under `-fsanitize=address`.
* **Alignment Enforcement:** Enforces `sizeof(void*)` word alignment at pool initialization to prevent unaligned pointer storage inside the freelist.
* **Thread Model:** Single-threaded and lock-free per pool. Thread ID checks in debug builds assert ownership.

---

## Repository Structure

| File | Purpose |
| :--- | :--- |
| `Arena.hpp` | The allocator itself. `mmap`-backed pools, intrusive freelist, bitset tracking, safe/raw dual API, ASan integration, debug-mode thread-ownership guards. Header-only. |
| `Dashboard.hpp` | An ncurses TUI that renders live pool state (block grid, allocation status, efficiency, latency). Dev/debug tool only. |
| `main.cpp` | Interactive CLI that walks through pool configuration, then drives the TUI. |
| `benchmark.cpp` | Standalone microbenchmark isolating `slab_alloc` / `slab_dealloc` latency from terminal I/O. |

---

## Building

Requires a C++20 compiler and the ncurses development headers.

```bash
# Install dependencies (Debian / Ubuntu)
sudo apt update && sudo apt install -y build-essential libncurses-dev

# Build and run interactive dashboard
g++ -std=c++20 -O2 main.cpp -o slab_allocator -lncurses
./slab_allocator

# Build and run benchmark
g++ -std=c++20 -O2 -DNDEBUG benchmark.cpp -o benchmark
./benchmark

### Build & Run Standalone Benchmark

The microbenchmark tests the core allocator in `Arena.hpp` in isolation without linking against ncurses:

```bash
g++ -std=c++20 -O2 -DNDEBUG benchmark.cpp -o benchmark
./benchmark
