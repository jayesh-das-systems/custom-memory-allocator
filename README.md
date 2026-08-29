# custom-memory-allocator
This project is a header-only, mmap-backed multi-slab allocator for Linux with strict O(1) allocate/deallocate on the hot path, an intrusive freelist + bitset design, ASan-integrated UAF Protection and a live ncurses TUI for inspecting pool share. It is built for RTS-style latency guarentees in C++20.
