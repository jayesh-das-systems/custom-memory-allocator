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

#include "Arena.hpp"
#include "Dashboard.hpp"

#include <chrono>
#include <string>
#include <iostream>
#include <limits>
#include <algorithm> // std::min
#include <cassert>   // Explicit assert dependency for debug builds

static void print_hardware_architecture_warning() {
    std::cout << "===================================================================\n";
    std::cout << "  [!] PHYSICAL HARDWARE ARCHITECTURE & MEMORY WARNING [!]\n";
    std::cout << "===================================================================\n";
    std::cout << "  1. THIS ALLOCATOR IS DESIGNED FOR PHYSICAL HARDWARE RAM.\n";
    std::cout << "     It relies directly on Linux kernel mmap() page tables and physical\n";
    std::cout << "     RAM page frames. Virtual overcommit or unbacked VM swap spaces\n";
    std::cout << "     WILL CRASH OR SEVERE PERFORMANCE PENALTIES WILL OCCUR.\n\n";
    std::cout << "  2. PLEASE VERIFY YOUR SYSTEM ARCHITECTURE BEFORE PROCEEDING:\n";
    std::cout << "     - Check physical RAM installed (e.g., free -h or lsmem).\n";
    std::cout << "     - Ensure Linux kernel VM overcommit settings (/proc/sys/vm/overcommit_memory)\n";
    std::cout << "       are configured correctly for physical memory allocations.\n";
    std::cout << "     - Virtual memory without physical RAM backing will fail mmap requests.\n";
    std::cout << "===================================================================\n\n";
}

static size_t read_strictly_positive_input(const std::string& prompt_msg) {
    while (true) {
        std::cout << prompt_msg;
        long long user_input = 0;

        if (std::cin >> user_input) {
            if (user_input > 0) {
                return static_cast<size_t>(user_input);
            }
            std::cout << "  [!] INVALID INPUT: Value must be strictly positive (> 0). You entered: " 
                      << user_input << "\n";
        } else {
            std::cout << "  [!] INVALID INPUT: Expected a positive integer, received non-numeric text.\n";
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }
    }
}

int main() {
    print_hardware_architecture_warning();

    size_t target_pools = read_strictly_positive_input("Enter total number of slab pools to create (1 - 16): ");
    if (target_pools > 16) {
        std::cout << "  [!] Note: Limiting pool count to 16 for TUI dashboard compatibility.\n";
        target_pools = 16;
    }

    try {
        ScopedMultiSlabManager scoped_mgr(target_pools);
        MultiSlabManager* manager = scoped_mgr.get();

        std::cout << "=== Multi-Slab Bare-Metal Allocator Setup ===\n";
        std::cout << "Target architecture word width detected: " << ARCH_WORD_BITS << "-bit (" << ARCH_WORD_BYTES << " bytes).\n\n";

        for (size_t i = 0; i < target_pools; ++i) {
            std::cout << "\n--- Configuring Slab Pool #" << i << " ---\n";
            
            size_t requested_blocks = read_strictly_positive_input("  Desired Number of Blocks (>= 1): ");
            size_t raw_block_size   = read_strictly_positive_input("  Raw Object Size in Bytes (>= 1): ");

            size_t aligned_block = align_to_arch(raw_block_size);
            if (aligned_block < sizeof(void*)) {
                aligned_block = sizeof(void*);
            }

            if (raw_block_size != aligned_block) {
                std::cout << "\n  [!] MISALIGNED BLOCK SIZE DETECTED:\n";
                std::cout << "      " << raw_block_size << "B is misaligned for " << ARCH_WORD_BITS << "-bit hardware.\n";
                std::cout << "      Valid hardware target span requires block size: " << aligned_block << "B.\n";
                std::cout << "      Would you like to switch to " << aligned_block << "B block size? (y/n): ";
                
                char choice;
                std::cin >> choice;
                if (choice == 'y' || choice == 'Y') {
                    raw_block_size = aligned_block;
                    std::cout << "      [+] Switched block size to " << aligned_block << "B.\n";
                } else {
                    throw InvalidAllocationRequest("Execution rejected by user for misaligned block size " + 
                        std::to_string(raw_block_size) + "B.");
                }
            }

            size_t bitset_words = bitset_words_needed(requested_blocks);
            size_t header_bytes = (bitset_words * sizeof(uintptr_t)) + raw_block_size;
            size_t total_span   = (requested_blocks * raw_block_size) + header_bytes;

            manager_add_pool_strict(manager, nullptr, total_span, raw_block_size);
            std::cout << "  [+] Pool #" << i << " mapped and verified successfully (" 
                      << manager->pools[i].total_blocks << " blocks usable).\n";
        }

        // Guarded ncurses TUI initialization
        ::initscr();
        ::cbreak();
        ::noecho();
        ::curs_set(0);
        ::keypad(stdscr, TRUE);
        ::start_color();
        ::init_pair(1, COLOR_GREEN, COLOR_BLACK);  // Free
        ::init_pair(2, COLOR_RED, COLOR_BLACK);    // Allocated
        ::init_pair(3, COLOR_CYAN, COLOR_BLACK);   // Headers / Focus
        ::init_pair(4, COLOR_YELLOW, COLOR_BLACK); // Telemetry

        void* allocated_ptrs[16][1024];
        size_t ptr_counts[16] = {0};

        size_t active_pool_idx = 0;
        double latency_us = 0.0;
        bool running = true;

        while (running) {
            render_dashboard(*manager, active_pool_idx, latency_us);

            int ch = ::getch();
            auto start_time = std::chrono::high_resolution_clock::now();

            if (ch == '\t' || ch == 9) {
                active_pool_idx = (active_pool_idx + 1) % manager->pool_count;
            } 
            else if (ch == 'a' || ch == 'A') {
                SlabPool* p = &manager->pools[active_pool_idx];
                size_t max_trackable_blocks = std::min(p->total_blocks, static_cast<size_t>(1024));

                if (active_pool_idx < 16 && ptr_counts[active_pool_idx] < max_trackable_blocks) {
                    void* ptr = slab_alloc(p);
                    if (ptr) {
                        allocated_ptrs[active_pool_idx][ptr_counts[active_pool_idx]++] = ptr;
                    }
                }
            } 
            else if (ch == 'd' || ch == 'D') {
                if (active_pool_idx < 16 && ptr_counts[active_pool_idx] > 0) {
                    SlabPool* p = &manager->pools[active_pool_idx];
                    void* ptr = allocated_ptrs[active_pool_idx][--ptr_counts[active_pool_idx]];
                    
                    bool dealloc_ok = slab_dealloc(p, ptr);
                    (void)dealloc_ok;
                    assert(dealloc_ok && "CRITICAL DEBUG ERROR: slab_dealloc failed on valid active pointer!");
                }
            } 
            else if (ch == 'q' || ch == 'Q') {
                running = false;
            }

            auto end_time = std::chrono::high_resolution_clock::now();
            latency_us = std::chrono::duration<double, std::micro>(end_time - start_time).count();
        }

        ::endwin(); // Clean shutdown on normal exit

    } catch (const std::exception& e) {
        if (!isendwin()) {
            ::endwin();
        }
        std::cerr << "\n\n===================================================\n";
        std::cerr << " [FATAL ERROR DETECTED - PROCESS ABORTED]\n";
        std::cerr << " " << e.what() << "\n";
        std::cerr << "===================================================\n\n";
        return 1;
    }

    std::cout << "\n[+] All Slab Pools & Manager unmapped cleanly from kernel via RAII. Execution finished.\n";
    return 0;
}
