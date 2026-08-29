# custom-memory-allocator
POSIX Multi-Slab Allocator
A header-only C++20 slab allocator backed by mmap. Designed for low, bounded latency allocation in single-threaded hot paths. Includes an optional ncurses TUI to inspect pool fragmentation and block state.
Author: Jayesh Kumar Das
License: GPLv3
Platform: Linux (x86_64)
How It Works
Instead of relying on malloc or new (which can stall on lock contention or heap coalescing), this allocator maps fixed memory pools directly with mmap().
->Allocation & Free: $O(1)$. Free blocks are tracked using an intrusive singly-linked list (pointers stored inside unallocated blocks) alongside a bitset for instant double-free and state checks.
->Alignment: Blocks are strictly aligned to sizeof(void*) during pool setup to avoid unaligned pointer dereferencing on the freelist.
->ASan Support: Integrates with sanitizer/asan_interface.h to poison freed blocks and unpoison active blocks when compiled with -fsanitize=address.
->Threading: Pools are lock-free and single-threaded. If multiple threads allocate from the same pool, synchronization must be handled externally. In debug builds, thread ID assertions catch cross-thread access.
Files
Arena.hpp: The core allocator. Header-only.

Dashboard.hpp: ncurses TUI for visualizing block layouts and allocation state (dev/debug only).

main.cpp: Driver for the interactive dashboard.

benchmark.cpp: Isolated latency microbenchmark.
