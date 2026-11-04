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

#pragma once

/*
 * =====================================================================================
 *                                   ARENA ALLOCATOR
 * =====================================================================================
 *  CRITICAL ARCHITECTURE & THREAD SAFETY NOTICE FOR DEVELOPERS:
 * -------------------------------------------------------------------------------------
 *  1. SINGLE-THREADED DESIGN INVARIANT:
 *     SlabPool and MultiSlabManager are ZERO-LOCK, SINGLE-THREADED primitives by design.
 *     Sharing a SlabPool handle across multiple concurrent threads WITHOUT explicit 
 *     external synchronization (e.g., std::mutex, spinlocks, or thread_local instances) 
 *     WILL cause race conditions and memory corruption. Debug builds enforce thread-id checks.
 *
 *  2. SAFE HANDLE OWNERSHIP CONTRACT:
 *     When using 'SafeBlockHandle<T>', ALWAYS deallocate using 'slab_dealloc_safe'. 
 *     Extracting raw pointers via '.get()' and passing them to raw 'slab_dealloc()' 
 *     bypasses handle state invalidation.
 *
 *  3. ADDRESS SANITIZER (ASan) INTEGRATION:
 *     Built-in ASan memory poisoning automatically marks freed slab memory as unaddressable,
 *     catching Use-After-Free (UAF) and out-of-bounds writes directly in custom mmap pools.
 * =====================================================================================
 */

#include <sys/mman.h>   // mmap, munmap, madvise
#include <cstddef>      // size_t, uintptr_t
#include <cstdint>      // uint8_t
#include <stdexcept>    // std::runtime_error
#include <cassert>      // assert
#include <pthread.h>    // pthread_self, pthread_equal
#include <string>

// ============================================================================
// ADDRESS SANITIZER (ASan) INTEGRATION HOOKS
// ============================================================================
#if defined(__has_feature)
  #if __has_feature(address_sanitizer)
    #define ARENA_ASAN_ENABLED 1
  #endif
#elif defined(__SANITIZE_ADDRESS__)
  #define ARENA_ASAN_ENABLED 1
#endif

#if defined(ARENA_ASAN_ENABLED)
  #include <sanitizer/asan_interface.h>
  #define ARENA_POISON_MEMORY_REGION(addr, size)   ASAN_POISON_MEMORY_REGION(addr, size)
  #define ARENA_UNPOISON_MEMORY_REGION(addr, size) ASAN_UNPOISON_MEMORY_REGION(addr, size)
#else
  #define ARENA_POISON_MEMORY_REGION(addr, size)   ((void)0)
  #define ARENA_UNPOISON_MEMORY_REGION(addr, size) ((void)0)
#endif

// POSIX Fallback Flags
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif

// ============================================================================
// PORTABLE BRANCH HINT MACROS
// ============================================================================
// NOTE: [[likely]]/[[unlikely]] (C++20 statement attributes) cannot be
// correctly expressed as a function-like macro wrapped around a boolean
// expression -- they only apply when attached directly to an `if`/`else`/
// `switch` statement, not to a sub-expression inside a condition. Rather
// than emit syntax that looks like a hint but is silently ignored by the
// compiler, the non-GCC/Clang fallback below is an honest no-op: it just
// passes the value through with no optimization hint at all.
#if defined(__GNUC__) || defined(__clang__)
  #define LIKELY(x)   __builtin_expect(!!(x), 1)
  #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
  #define LIKELY(x)   (x)
  #define UNLIKELY(x) (x)
#endif

constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024; // 2 MB HugeTLB boundary
constexpr size_t SYSTEM_PAGE_SIZE = 4096;          // Standard 4KB page boundary

inline constexpr size_t MAX_SAFE_SPAN = ~static_cast<size_t>(0) >> 1;

// ============================================================================
// CUSTOM EXCEPTION CLASSES
// ============================================================================

class InvalidAllocationRequest : public std::runtime_error {
public:
    explicit InvalidAllocationRequest(const char* msg) 
        : std::runtime_error(msg) {}
    explicit InvalidAllocationRequest(const std::string& msg) 
        : std::runtime_error(msg) {}
};

class MemoryOverlapException : public std::runtime_error {
public:
    explicit MemoryOverlapException(const char* msg) 
        : std::runtime_error(msg) {}
    explicit MemoryOverlapException(const std::string& msg) 
        : std::runtime_error(msg) {}
};

class CorruptedPointerException : public std::runtime_error {
public:
    explicit CorruptedPointerException(const char* msg) 
        : std::runtime_error(msg) {}
    explicit CorruptedPointerException(const std::string& msg) 
        : std::runtime_error(msg) {}
};

// ============================================================================
// UNIVERSAL TARGET ARCHITECTURE ABSTRACTIONS
// ============================================================================

inline constexpr size_t ARCH_WORD_BYTES = sizeof(uintptr_t);
inline constexpr size_t ARCH_WORD_BITS  = ARCH_WORD_BYTES * 8;

inline constexpr size_t count_trailing_zeros(size_t val) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return val == 0 ? ARCH_WORD_BITS : static_cast<size_t>(__builtin_ctzl(val));
#else
    size_t count = 0;
    while ((val & 1) == 0 && count < ARCH_WORD_BITS) {
        val >>= 1;
        count++;
    }
    return count;
#endif
}

inline constexpr size_t BITSET_SHIFT = count_trailing_zeros(ARCH_WORD_BITS);
inline constexpr size_t BITSET_MASK  = ARCH_WORD_BITS - 1;

inline constexpr size_t align_to_arch(size_t n) noexcept {
    return (n + (ARCH_WORD_BYTES - 1)) & ~static_cast<size_t>(ARCH_WORD_BYTES - 1);
}

inline constexpr uintptr_t align_up(uintptr_t addr, size_t alignment) noexcept {
    return (addr + (alignment - 1)) & ~static_cast<uintptr_t>(alignment - 1);
}

inline constexpr size_t bitset_words_needed(size_t num_bits) noexcept {
    return (num_bits + BITSET_MASK) >> BITSET_SHIFT;
}

// ============================================================================
// DATA STRUCTURES
// ============================================================================

enum class AllocationState : uint8_t {
    UNMAPPED = 0,
    MAPPED   = 1
};

struct SlabPool {
    uint8_t* raw_mmap_ptr;    
    uint8_t* blocks_base_ptr; 
    uintptr_t* bitset;        
    size_t requested_span;    
    size_t block_size;        
    size_t total_blocks;      
    size_t usable_bytes;      
    size_t tail_slack_bytes;  
    void* free_head;          
    AllocationState state;    
    bool uses_huge_pages;     
    pthread_t owner_thread_id; 
    bool has_owner_thread;     
};

struct MultiSlabManager {
    SlabPool* pools;          
    size_t pool_count;        
    size_t max_pools;         
    size_t manager_mmap_size; 
};

// ============================================================================
// USE-AFTER-FREE (UAF) SAFE BLOCK HANDLE
// ============================================================================

template <typename T>
class [[nodiscard("SAFE_HANDLE_MUST_NOT_BE_DISCARDED")]] SafeBlockHandle {
private:
    T* ptr_{nullptr};
    SlabPool* owner_pool_{nullptr};
    bool is_freed_{false};

public:
    SafeBlockHandle() noexcept = default;
    
    SafeBlockHandle(void* raw_ptr, SlabPool* pool) noexcept 
        : ptr_(static_cast<T*>(raw_ptr)), owner_pool_(pool), is_freed_(raw_ptr == nullptr) {}

    SafeBlockHandle(const SafeBlockHandle&) = delete;
    SafeBlockHandle& operator=(const SafeBlockHandle&) = delete;

    SafeBlockHandle(SafeBlockHandle&& other) noexcept 
        : ptr_(other.ptr_), owner_pool_(other.owner_pool_), is_freed_(other.is_freed_) {
        other.ptr_ = nullptr;
        other.owner_pool_ = nullptr;
        other.is_freed_ = true;
    }

    SafeBlockHandle& operator=(SafeBlockHandle&& other) noexcept {
        if (this != &other) {
            ptr_ = other.ptr_;
            owner_pool_ = other.owner_pool_;
            is_freed_ = other.is_freed_;
            other.ptr_ = nullptr;
            other.owner_pool_ = nullptr;
            other.is_freed_ = true;
        }
        return *this;
    }

    [[nodiscard]] T* get() const {
        if (UNLIKELY(is_freed_ || ptr_ == nullptr)) {
            throw CorruptedPointerException("USE-AFTER-FREE DETECTED: Attempted to access a dereferenced or freed slab block.");
        }
        return ptr_;
    }

    [[nodiscard]] T* operator->() const { return get(); }
    [[nodiscard]] T& operator*() const { return *get(); }

    [[nodiscard]] SlabPool* get_owner_pool() const noexcept { return owner_pool_; }

    void mark_freed() noexcept {
        ptr_ = nullptr;
        owner_pool_ = nullptr;
        is_freed_ = true;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return !is_freed_ && ptr_ != nullptr;
    }
};

// ============================================================================
// HELPER & MANAGEMENT FUNCTIONS
// ============================================================================

inline bool check_address_overlap(const MultiSlabManager* mgr, uintptr_t new_start, size_t new_span) noexcept {
    if (UNLIKELY(!mgr || !mgr->pools)) return false;
    
    if (UNLIKELY(new_start + new_span < new_start)) return true;

    uintptr_t new_end = new_start + new_span;

    for (size_t i = 0; i < mgr->pool_count; ++i) {
        if (mgr->pools[i].state != AllocationState::MAPPED) {
            continue;
        }

        uintptr_t existing_start = reinterpret_cast<uintptr_t>(mgr->pools[i].raw_mmap_ptr);
        uintptr_t existing_end   = existing_start + mgr->pools[i].requested_span;

        if (UNLIKELY(new_start < existing_end && new_end > existing_start)) {
            return true;
        }
    }
    return false;
}

inline bool manager_init(MultiSlabManager* mgr, size_t capacity) noexcept {
    if (UNLIKELY(!mgr || capacity == 0 || capacity > MAX_SAFE_SPAN)) return false;

    mgr->max_pools = capacity;
    mgr->pool_count = 0;
    mgr->manager_mmap_size = align_to_arch(capacity * sizeof(SlabPool));

    void* raw_mgr_mem = ::mmap(nullptr, mgr->manager_mmap_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (UNLIKELY(raw_mgr_mem == MAP_FAILED)) {
        return false;
    }

    mgr->pools = static_cast<SlabPool*>(raw_mgr_mem);

    for (size_t i = 0; i < capacity; ++i) {
        mgr->pools[i].state = AllocationState::UNMAPPED;
        mgr->pools[i].raw_mmap_ptr = nullptr;
        mgr->pools[i].uses_huge_pages = false;
        mgr->pools[i].has_owner_thread = false;
    }

    return true;
}

inline void manager_add_pool_strict(MultiSlabManager* mgr, void* custom_addr, size_t total_bytes, size_t user_block_size) {
    if (UNLIKELY(!mgr)) {
        throw InvalidAllocationRequest("Manager handle cannot be null.");
    }
    
    if (UNLIKELY(total_bytes == 0 || total_bytes > MAX_SAFE_SPAN)) {
        throw InvalidAllocationRequest("Requested span is invalid, zero, or results from negative integer wrap-around.");
    }

    if (UNLIKELY(user_block_size == 0 || user_block_size > MAX_SAFE_SPAN)) {
        throw InvalidAllocationRequest("Block size is invalid, zero, or results from negative integer wrap-around.");
    }

    if (UNLIKELY(user_block_size > total_bytes)) {
        throw InvalidAllocationRequest("Block size exceeds requested span.");
    }

    uintptr_t start_addr = reinterpret_cast<uintptr_t>(custom_addr);
    if (custom_addr != nullptr) {
        if (UNLIKELY(start_addr % SYSTEM_PAGE_SIZE != 0)) {
            throw InvalidAllocationRequest("Custom target base address must be strictly 4KB page-aligned.");
        }
        if (UNLIKELY(start_addr + total_bytes < start_addr)) {
            throw InvalidAllocationRequest("Target base address + span causes virtual memory address wraparound.");
        }
    }

    if (UNLIKELY(!mgr->pools || mgr->pool_count >= mgr->max_pools)) {
        throw InvalidAllocationRequest("Manager pool capacity exceeded (max_pools reached).");
    }

    size_t expected_aligned_block = align_to_arch(user_block_size);
    if (expected_aligned_block < sizeof(void*)) {
        expected_aligned_block = sizeof(void*);
    }

    if (UNLIKELY(user_block_size != expected_aligned_block)) {
        throw InvalidAllocationRequest("Block size is misaligned for target architecture word width.");
    }

    if (custom_addr != nullptr) {
        if (UNLIKELY(check_address_overlap(mgr, start_addr, total_bytes))) {
            throw MemoryOverlapException("Specified base address range overlaps with an existing mapped pool.");
        }
    }

    SlabPool* p = &mgr->pools[mgr->pool_count];
    p->requested_span = total_bytes;
    p->block_size = expected_aligned_block;
    p->owner_thread_id = ::pthread_self();
    p->has_owner_thread = true;

    int base_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (custom_addr != nullptr) {
        base_flags |= MAP_FIXED_NOREPLACE;
    }

    void* mapped_mem = MAP_FAILED;
    bool used_huge = false;

    if (p->requested_span >= HUGE_PAGE_SIZE) {
        mapped_mem = ::mmap(custom_addr, p->requested_span, PROT_READ | PROT_WRITE, base_flags | MAP_HUGETLB, -1, 0);
        if (mapped_mem != MAP_FAILED) {
            used_huge = true;
        }
    }

    if (mapped_mem == MAP_FAILED) {
        mapped_mem = ::mmap(custom_addr, p->requested_span, PROT_READ | PROT_WRITE, base_flags, -1, 0);
    }

    if (UNLIKELY(mapped_mem == MAP_FAILED)) {
        p->state = AllocationState::UNMAPPED;
        throw MemoryOverlapException("Kernel mmap failed: Address occupied (EEXIST) or insufficient page frame space (ENOMEM).");
    }

#ifdef MADV_HUGEPAGE
    if (!used_huge && p->requested_span >= HUGE_PAGE_SIZE) {
        ::madvise(mapped_mem, p->requested_span, MADV_HUGEPAGE);
    }
#endif

    p->raw_mmap_ptr = static_cast<uint8_t*>(mapped_mem);
    p->state = AllocationState::MAPPED;
    p->uses_huge_pages = used_huge;

    uintptr_t actual_start = reinterpret_cast<uintptr_t>(p->raw_mmap_ptr);
    if (UNLIKELY(check_address_overlap(mgr, actual_start, total_bytes))) {
        ::munmap(p->raw_mmap_ptr, p->requested_span);
        p->state = AllocationState::UNMAPPED;
        throw MemoryOverlapException("Kernel mapped region intersects an active pool address space. Aborting.");
    }

    size_t est_blocks = p->requested_span / p->block_size;
    size_t bitset_words = bitset_words_needed(est_blocks);
    size_t bitset_bytes = bitset_words * sizeof(uintptr_t);

    if (UNLIKELY(bitset_bytes >= p->requested_span)) {
        ::munmap(p->raw_mmap_ptr, p->requested_span);
        p->state = AllocationState::UNMAPPED;
        throw InvalidAllocationRequest("Span too small to hold required bitset metadata header.");
    }

    p->bitset = reinterpret_cast<uintptr_t*>(p->raw_mmap_ptr);

    uintptr_t raw_payload_addr = reinterpret_cast<uintptr_t>(p->raw_mmap_ptr) + bitset_bytes;
    uintptr_t aligned_payload_addr = align_up(raw_payload_addr, p->block_size);

    size_t alignment_padding = aligned_payload_addr - raw_payload_addr;
    size_t total_header_bytes = bitset_bytes + alignment_padding;

    if (UNLIKELY(total_header_bytes >= p->requested_span)) {
        ::munmap(p->raw_mmap_ptr, p->requested_span);
        p->state = AllocationState::UNMAPPED;
        throw InvalidAllocationRequest("Span too small after metadata header and block alignment padding.");
    }

    uint8_t* payload_start = reinterpret_cast<uint8_t*>(aligned_payload_addr);
    size_t payload_span = p->requested_span - total_header_bytes;

    p->total_blocks = payload_span / p->block_size;
    if (UNLIKELY(p->total_blocks == 0)) {
        ::munmap(p->raw_mmap_ptr, p->requested_span);
        p->state = AllocationState::UNMAPPED;
        throw InvalidAllocationRequest("Requested span results in 0 usable blocks after metadata and alignment padding.");
    }

    p->blocks_base_ptr = payload_start;
    p->usable_bytes = p->total_blocks * p->block_size;
    p->tail_slack_bytes = p->requested_span - (total_header_bytes + p->usable_bytes);

    size_t actual_bitset_words = bitset_words_needed(p->total_blocks);
    for (size_t i = 0; i < actual_bitset_words; ++i) {
        p->bitset[i] = static_cast<uintptr_t>(0);
    }

    p->free_head = p->blocks_base_ptr;
    uint8_t* curr = p->blocks_base_ptr;
    for (size_t i = 0; i + 1 < p->total_blocks; ++i) {
        uint8_t* next = curr + p->block_size;
        *reinterpret_cast<void**>(curr) = next;
        curr = next;
    }
    *reinterpret_cast<void**>(curr) = nullptr;

    // Poison freelist payload blocks initially for AddressSanitizer
    ARENA_POISON_MEMORY_REGION(p->blocks_base_ptr, p->usable_bytes);

    mgr->pool_count++;
}

inline void manager_free_all(MultiSlabManager* mgr) noexcept {
    if (UNLIKELY(!mgr)) return;

    for (size_t i = 0; i < mgr->pool_count; ++i) {
        SlabPool& pool = mgr->pools[i];
        if (pool.state == AllocationState::MAPPED && pool.raw_mmap_ptr != nullptr) {
            ARENA_UNPOISON_MEMORY_REGION(pool.raw_mmap_ptr, pool.requested_span);
            ::munmap(pool.raw_mmap_ptr, pool.requested_span);
            pool.state = AllocationState::UNMAPPED;
            pool.raw_mmap_ptr = nullptr;
            pool.has_owner_thread = false;
        }
    }

    if (mgr->pools != nullptr) {
        ::munmap(mgr->pools, mgr->manager_mmap_size);
        mgr->pools = nullptr;
    }
    
    mgr->pool_count = 0;
    mgr->max_pools = 0;
}

class ScopedMultiSlabManager {
private:
    MultiSlabManager mgr_{};

public:
    explicit ScopedMultiSlabManager(size_t capacity) {
        if (!manager_init(&mgr_, capacity)) {
            throw MemoryOverlapException("Failed to initialize ScopedMultiSlabManager virtual memory mapping.");
        }
    }

    ~ScopedMultiSlabManager() noexcept {
        manager_free_all(&mgr_);
    }

    ScopedMultiSlabManager(const ScopedMultiSlabManager&) = delete;
    ScopedMultiSlabManager& operator=(const ScopedMultiSlabManager&) = delete;

    ScopedMultiSlabManager(ScopedMultiSlabManager&& other) noexcept {
        mgr_ = other.mgr_;
        other.mgr_ = MultiSlabManager{};
    }

    [[nodiscard]] MultiSlabManager* get() noexcept { return &mgr_; }
    [[nodiscard]] const MultiSlabManager* get() const noexcept { return &mgr_; }
    [[nodiscard]] MultiSlabManager* operator->() noexcept { return &mgr_; }
};

// HOT PATH: O(1) Intrusive Freelist Allocation
inline void* slab_alloc(SlabPool* p) noexcept {
#ifndef NDEBUG
    if (p && p->has_owner_thread) {
        assert(::pthread_equal(p->owner_thread_id, ::pthread_self()) != 0 && 
               "THREAD SAFETY BREACH: Concurrent access to a single-threaded SlabPool!");
    }
#endif

    if (UNLIKELY(!p || !p->free_head)) return nullptr;

    void* block = p->free_head;

    // Unpoison block header momentarily to read intrusive next pointer
    ARENA_UNPOISON_MEMORY_REGION(block, sizeof(void*));
    p->free_head = *reinterpret_cast<void**>(block);

    // Unpoison full payload block for application use
    ARENA_UNPOISON_MEMORY_REGION(block, p->block_size);

    size_t block_idx = (static_cast<uint8_t*>(block) - p->blocks_base_ptr) / p->block_size;
    
    size_t word_idx = block_idx >> BITSET_SHIFT;
    size_t bit_pos  = block_idx & BITSET_MASK;
    p->bitset[word_idx] |= (static_cast<uintptr_t>(1) << bit_pos);

    return block;
}

template <typename T>
inline SafeBlockHandle<T> slab_alloc_safe(SlabPool* p) {
    if (UNLIKELY(!p)) {
        throw InvalidAllocationRequest("Target SlabPool cannot be null.");
    }

    if (UNLIKELY(sizeof(T) > p->block_size)) {
        throw InvalidAllocationRequest("TYPE BOUND EXCEEDED: sizeof(T) exceeds pool block_size.");
    }

    void* raw_ptr = slab_alloc(p);
    return SafeBlockHandle<T>(raw_ptr, p);
}

// HOT PATH: O(1) Short-Circuit Safe Deallocation
inline bool slab_dealloc(SlabPool* p, void* ptr) noexcept {
#ifndef NDEBUG
    if (p && p->has_owner_thread) {
        assert(::pthread_equal(p->owner_thread_id, ::pthread_self()) != 0 && 
               "THREAD SAFETY BREACH: Concurrent deallocation on a single-threaded SlabPool!");
    }
#endif

    if (UNLIKELY(!p || !ptr)) return false;

    uint8_t* byte_ptr = static_cast<uint8_t*>(ptr);

    if (UNLIKELY(byte_ptr < p->blocks_base_ptr || byte_ptr >= (p->blocks_base_ptr + p->usable_bytes))) {
        return false;
    }

    uintptr_t offset = byte_ptr - p->blocks_base_ptr;
    if (UNLIKELY(offset % p->block_size != 0)) {
        return false;
    }

    size_t block_idx = offset / p->block_size;
    size_t word_idx  = block_idx >> BITSET_SHIFT;
    size_t bit_pos   = block_idx & BITSET_MASK;

    if (UNLIKELY(!(p->bitset[word_idx] & (static_cast<uintptr_t>(1) << bit_pos)))) {
        return false;
    }

    p->bitset[word_idx] &= ~(static_cast<uintptr_t>(1) << bit_pos);

    // Unpoison header to write intrusive freelist link, then poison payload for ASan
    ARENA_UNPOISON_MEMORY_REGION(ptr, sizeof(void*));
    *reinterpret_cast<void**>(ptr) = p->free_head;
    p->free_head = ptr;

    ARENA_POISON_MEMORY_REGION(ptr, p->block_size);

    return true;
}

template <typename T>
inline bool slab_dealloc_safe(SlabPool* p, SafeBlockHandle<T>& handle) noexcept {
    if (UNLIKELY(!handle.is_valid() || handle.get_owner_pool() != p)) {
        return false;
    }
    
    void* raw_ptr = static_cast<void*>(handle.get());
    bool result = slab_dealloc(p, raw_ptr);
    if (result) {
        handle.mark_freed();
    }
    return result;
}
