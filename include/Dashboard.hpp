/*
 * Multi-Slab Bare-Metal Allocator Engine
 * Copyright (C) 2026
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

#pragma once

#include "Arena.hpp"
#include <ncurses.h>

// ============================================================================
// UI LAYOUT & RENDERING CONSTANTS
// ============================================================================
namespace UIConfig {
    constexpr size_t MAX_RENDERED_BLOCKS = 128; // Cap block visualization rendering to prevent screen overflow
    constexpr size_t BLOCKS_PER_ROW      = 32;  // Maximum blocks displayed per line before wrapping
    constexpr int    GRID_INDENT_COL      = 6;   // Horizontal column offset for block state grid
    
    // Screen bottom safety margins:
    // Reserved space at screen bottom for latency status line and control guide
    constexpr int    FOOTER_RESERVED_ROWS = 2;   

    // Needs 3 lines below current_row to ensure space for footer telemetry + controls
    constexpr int    GRID_LOOP_MARGIN     = 3;   

    // Needs 4 lines below current_row to allow rendering pool headers, metrics line, and grid start
    constexpr int    POOL_LOOP_MARGIN     = 4;   
}

inline void render_dashboard(const MultiSlabManager& mgr, size_t active_pool_idx, double latency_us) noexcept {
    ::erase();

    int max_y = getmaxy(stdscr);

    ::attron(COLOR_PAIR(3) | A_BOLD);
    ::mvprintw(1, 2, "===================================================================");
    ::mvprintw(2, 2, "    MULTI-SLAB INSPECTOR (ZERO-HEAP METADATA / ARCH-AGNOSTIC)      ");
    ::mvprintw(3, 2, "===================================================================");
    ::attroff(COLOR_PAIR(3) | A_BOLD);

    int current_row = 5;

    for (size_t p = 0; p < mgr.pool_count; ++p) {
        if (current_row >= max_y - UIConfig::POOL_LOOP_MARGIN) {
            break;
        }

        const SlabPool& pool = mgr.pools[p];
        double efficiency = (pool.requested_span > 0) 
            ? (static_cast<double>(pool.usable_bytes) / static_cast<double>(pool.requested_span)) * 100.0 
            : 0.0;

        if (p == active_pool_idx) {
            ::attron(COLOR_PAIR(4) | A_BOLD);
            ::mvprintw(current_row++, 2, ">>> SLAB POOL #%zu [ACTIVE FOCUS] <<<", p);
            ::attroff(COLOR_PAIR(4) | A_BOLD);
        } else {
            ::attron(COLOR_PAIR(3));
            ::mvprintw(current_row++, 2, "--- SLAB POOL #%zu ---", p);
            ::attroff(COLOR_PAIR(3));
        }

        // Metrics formatting with multi-line argument alignment
        ::mvprintw(
            current_row++, 
            4, 
            "Base: %p | Span: %zu B | Block: %zu B | Count: %zu | Slack: %zu B (%.1f%% Eff)",
            static_cast<void*>(pool.blocks_base_ptr),
            pool.requested_span,
            pool.block_size,
            pool.total_blocks,
            pool.tail_slack_bytes,
            efficiency
        );

        int col = UIConfig::GRID_INDENT_COL;
        size_t render_limit = (pool.total_blocks > UIConfig::MAX_RENDERED_BLOCKS) 
            ? UIConfig::MAX_RENDERED_BLOCKS 
            : pool.total_blocks;

        // Render Block State Grid
        for (size_t i = 0; i < render_limit; ++i) {
            if (current_row >= max_y - UIConfig::GRID_LOOP_MARGIN) {
                break;
            }

            size_t word_idx = i >> BITSET_SHIFT;
            size_t bit_pos  = i & BITSET_MASK;
            bool is_allocated = (pool.bitset[word_idx] & (static_cast<uintptr_t>(1) << bit_pos)) != 0;

            if (is_allocated) {
                ::attron(COLOR_PAIR(2) | A_BOLD);
                ::mvprintw(current_row, col, "#");
                ::attroff(COLOR_PAIR(2) | A_BOLD);
            } else {
                ::attron(COLOR_PAIR(1));
                ::mvprintw(current_row, col, ".");
                ::attroff(COLOR_PAIR(1));
            }

            // Row-Wrap Logic for Grid
            col += 2;
            if ((i + 1) % UIConfig::BLOCKS_PER_ROW == 0 && (i + 1) < render_limit) {
                current_row++;
                col = UIConfig::GRID_INDENT_COL;
            }
        }

        if (pool.total_blocks > UIConfig::MAX_RENDERED_BLOCKS) {
            ::mvprintw(current_row++, col + 2, "... (+%zu more blocks)", pool.total_blocks - UIConfig::MAX_RENDERED_BLOCKS);
        } else {
            current_row++;
        }
        current_row++;
    }

    // Render Telemetry & Control Footer
    if (current_row < max_y - UIConfig::FOOTER_RESERVED_ROWS) {
        ::attron(COLOR_PAIR(4));
        ::mvprintw(current_row++, 2, "Last Op Execution Latency: %.3f microseconds (us)", latency_us);
        ::attroff(COLOR_PAIR(4));

        ::attron(COLOR_PAIR(3));
        ::mvprintw(current_row + 1, 2, "Controls: [TAB] Switch Active Pool | [A] Alloc Active | [D] Dealloc Active | [Q] Quit");
        ::attroff(COLOR_PAIR(3));
    }

    ::refresh();
}
