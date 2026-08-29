# custom-memory-allocator
---

## Demo

<p align="center">
  <img src="assets/demo.gif" alt="POSIX Multi-Slab Allocator TUI Demo" width="80%">
</p>

---
# POSIX Multi-Slab Allocator

A header-only C++20 slab allocator backed by `mmap`. Designed for low, bounded latency allocation in single-threaded hot paths. Includes an optional `ncurses` TUI to inspect pool fragmentation and block state.

- **Author:** Jayesh Kumar Das
- **License:** GPLv3
- **Platform:** Linux (x86_64)

---

## How It Works

Instead of relying on `malloc` or `new` (which can stall on lock contention or heap coalescing), this allocator maps fixed memory pools directly with `mmap()`.

* **Allocation & Free ($O(1)$):** Free blocks are tracked using an intrusive singly-linked list (pointers stored inside unallocated blocks) alongside a bitset for instant double-free and state checks.
* **Alignment:** Blocks are strictly aligned to `sizeof(void*)` during pool setup to avoid unaligned pointer dereferencing on the freelist.
* **ASan Support:** Integrates with `sanitizer/asan_interface.h` to poison freed blocks and unpoison active blocks when compiled with `-fsanitize=address`.
* **Threading:** Pools are lock-free and single-threaded. If multiple threads allocate from the same pool, synchronization must be handled externally. In debug builds, thread ID assertions catch cross-thread access.

---

## Files

* `Arena.hpp`: The core allocator. Header-only.
* `Dashboard.hpp`: `ncurses` TUI for visualizing block layouts and allocation state (dev/debug only).
* `main.cpp`: Driver for the interactive dashboard.
* `benchmark.cpp`: Isolated latency microbenchmark.

---

## Build & Run

Requires GCC 11+ / Clang 13+ (C++20) and `libncurses-dev`.

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt update && sudo apt install -y build-essential libncurses-dev

# Run interactive dashboard
g++ -std=c++20 -O2 main.cpp -o slab_allocator -lncurses
./slab_allocator

# Run standalone benchmark
g++ -std=c++20 -O2 -DNDEBUG benchmark.cpp -o benchmark
./benchmark
```

---

## Quick Example

```cpp
#include "Arena.hpp"

struct Particle {
    float x, y, z;
};

int main() {
    // Allocate 1 pool slot
    ScopedMultiSlabManager mgr(1);

    // Create a 4KB pool for Particle structs
    manager_add_pool_strict(mgr.get(), nullptr, 4096, align_to_arch(sizeof(Particle)));
    SlabPool* pool = &mgr.get()->pools[0];

    // Safe API (typed wrapper + bounds validation)
    SafeBlockHandle<Particle> p = slab_alloc_safe<Particle>(pool);
    p->x = 10.0f;
    p->y = 20.0f;
    p->z = 30.0f;
    slab_dealloc_safe(pool, p);

    // Raw API (untyped void* for zero-overhead hot paths)
    void* raw = slab_alloc(pool);
    slab_dealloc(pool, raw);

    return 0; // mmap regions unmapped automatically on scope exit
}
```

---

## Latency Benchmark

Measured with `benchmark.cpp` (15 runs × 100,000 iterations, `-O2 -DNDEBUG`, x86_64):

| Operation | Median |
| :--- | :--- |
| `slab_alloc` | ~4.0 ns |
| `slab_dealloc` | ~1.0 ns |
| **Round-trip (alloc + dealloc)** | **~7.4 ns** |

> **Note:** The latency counter in `Dashboard.hpp` measures ncurses rendering and clock-read overhead alongside the allocation, making it significantly noisier than the standalone benchmark.

---

## Limitations

* **Linux specific:** Uses `MAP_FIXED_NOREPLACE` in custom address mappings.
* **Safe API bypass:** Calling `.get()` on a `SafeBlockHandle` to extract the raw pointer and freeing it with `slab_dealloc()` bypasses the safety checks.

---

## Notes & License

Built as a systems programming project exploring deterministic allocators and intrusive data structures. Early scaffolding and benchmark scripts drafted with LLM assistance.

Licensed under the **GNU General Public License v3.0**.
    return 0; // ScopedMultiSlabManager calls munmap() automatically via RAII
}
