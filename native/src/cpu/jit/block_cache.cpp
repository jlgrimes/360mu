/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Block Cache - Manages compiled JIT code blocks
 * 
 * Features:
 * - O(1) block lookup via hash table
 * - LRU eviction when cache is full
 * - Self-modifying code detection
 * - Block linking for direct jumps
 */

#include "jit.h"
#include <cstring>
#include <algorithm>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-jit"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[BlockCache] " __VA_ARGS__); printf("\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

/**
 * Block Cache implementation
 * 
 * Uses a combination of:
 * 1. Hash table for O(1) address lookup
 * 2. Doubly-linked list for LRU tracking
 * 3. Page-granular tracking for invalidation
 */
class BlockCache {
public:
    // Page size for SMC detection (4KB)
    static constexpr u32 PAGE_SIZE = 4096;
    static constexpr u32 PAGE_SHIFT = 12;
    
    // Maximum blocks before eviction
    static constexpr u32 MAX_BLOCKS = 16384;
    
    // Hash table size (power of 2)
    static constexpr u32 HASH_SIZE = 32768;
    static constexpr u32 HASH_MASK = HASH_SIZE - 1;

    BlockCache() {
        hash_table_.resize(HASH_SIZE, nullptr);
    }
    
    ~BlockCache() {
        clear();
    }
    
    /**
     * Look up compiled block for guest address
     */
    CompiledBlock* lookup(GuestAddr addr) {
        u32 hash = compute_hash(addr);
        CompiledBlock* block = hash_table_[hash];
        
        while (block) {
            if (block->start_addr == addr) {
                // Move to front of LRU
                promote_block(block);
                return block;
            }
            block = block->hash_next;
        }
        
        return nullptr;
    }
    
    /**
     * Insert compiled block
     */
    void insert(CompiledBlock* block) {
        // Check if we need to evict
        if (block_count_ >= MAX_BLOCKS) {
            evict_lru();
        }
        
        // Add to hash table
        u32 hash = compute_hash(block->start_addr);
        block->hash_next = hash_table_[hash];
        if (hash_table_[hash]) {
            hash_table_[hash]->hash_prev = block;
        }
        block->hash_prev = nullptr;
        hash_table_[hash] = block;
        
        // Add to LRU (front = most recent)
        add_to_lru_front(block);
        
        // Register pages for invalidation
        register_pages(block);
        
        block_count_++;
    }
    
    /**
     * Invalidate blocks overlapping address range
     */
    void invalidate(GuestAddr addr, u32 size) {
        GuestAddr start_page = addr >> PAGE_SHIFT;
        GuestAddr end_page = (addr + size - 1) >> PAGE_SHIFT;
        
        for (GuestAddr page = start_page; page <= end_page; page++) {
            auto it = page_blocks_.find(page);
            if (it != page_blocks_.end()) {
                // Copy list since we'll modify it
                std::vector<CompiledBlock*> blocks_to_remove = it->second;
                
                for (CompiledBlock* block : blocks_to_remove) {
                    // Check if block actually overlaps
                    GuestAddr block_end = block->start_addr + block->size * 4;
                    if (block->start_addr < addr + size && block_end > addr) {
                        remove_block(block);
                    }
                }
            }
        }
    }
    
    /**
     * Clear entire cache
     */
    void clear() {
        // Delete all blocks
        for (CompiledBlock* block = lru_head_; block;) {
            CompiledBlock* next = block->lru_next;
            delete block;
            block = next;
        }
        
        // Reset state
        std::fill(hash_table_.begin(), hash_table_.end(), nullptr);
        page_blocks_.clear();
        lru_head_ = nullptr;
        lru_tail_ = nullptr;
        block_count_ = 0;
    }
    
    /**
     * Get cache statistics
     */
    struct Stats {
        u32 block_count;
        u32 lookup_hits;
        u32 lookup_misses;
        u32 evictions;
        u32 invalidations;
    };
    
    Stats get_stats() const {
        return {
            block_count_,
            lookup_hits_,
            lookup_misses_,
            evictions_,
            invalidations_
        };
    }
    
    /**
     * Try to link a block's exits to other blocks
     */
    void link_block(CompiledBlock* block) {
        for (auto& link : block->links) {
            if (link.linked) continue;
            
            CompiledBlock* target = lookup(link.target);
            if (target) {
                // Patch the branch instruction to jump directly to target
                u32* patch_addr = reinterpret_cast<u32*>(
                    static_cast<u8*>(block->code) + link.patch_offset
                );
                
                s64 offset = static_cast<u8*>(target->code) - 
                            reinterpret_cast<u8*>(patch_addr);
                
                // Check if offset fits in branch immediate
                if (offset >= -128*1024*1024 && offset < 128*1024*1024) {
                    // Patch unconditional branch
                    s32 imm26 = offset >> 2;
                    *patch_addr = 0x14000000 | (imm26 & 0x03FFFFFF);
                    
                    link.linked = true;
                    
                    // Flush icache for this location
#ifdef __aarch64__
                    __builtin___clear_cache(
                        reinterpret_cast<char*>(patch_addr),
                        reinterpret_cast<char*>(patch_addr) + 4
                    );
#endif
                }
            }
        }
    }
    
    /**
     * Unlink all references to a block
     */
    void unlink_block(CompiledBlock* block) {
        // Find all blocks that link to this one and unlink them
        // This is O(n) but only happens on invalidation
        for (CompiledBlock* other = lru_head_; other; other = other->lru_next) {
            for (auto& link : other->links) {
                if (link.target == block->start_addr && link.linked) {
                    // Restore original branch to exit stub
                    // For simplicity, we'll just mark as unlinked and 
                    // let the dispatcher handle it
                    link.linked = false;
                }
            }
        }
    }
    
private:
    // Hash table for address lookup
    std::vector<CompiledBlock*> hash_table_;
    
    // LRU list
    CompiledBlock* lru_head_ = nullptr;
    CompiledBlock* lru_tail_ = nullptr;
    
    // Page -> blocks mapping for invalidation
    std::unordered_map<GuestAddr, std::vector<CompiledBlock*>> page_blocks_;
    
    // Statistics
    u32 block_count_ = 0;
    mutable u32 lookup_hits_ = 0;
    mutable u32 lookup_misses_ = 0;
    u32 evictions_ = 0;
    u32 invalidations_ = 0;
    
    // Hash function for guest addresses
    u32 compute_hash(GuestAddr addr) const {
        // Simple but effective hash for code addresses
        // Most code is aligned, so shift out low bits
        u64 h = addr >> 2;
        h = (h ^ (h >> 16)) & HASH_MASK;
        return static_cast<u32>(h);
    }
    
    // LRU management
    void add_to_lru_front(CompiledBlock* block) {
        block->lru_prev = nullptr;
        block->lru_next = lru_head_;
        
        if (lru_head_) {
            lru_head_->lru_prev = block;
        }
        lru_head_ = block;
        
        if (!lru_tail_) {
            lru_tail_ = block;
        }
    }
    
    void remove_from_lru(CompiledBlock* block) {
        if (block->lru_prev) {
            block->lru_prev->lru_next = block->lru_next;
        } else {
            lru_head_ = block->lru_next;
        }
        
        if (block->lru_next) {
            block->lru_next->lru_prev = block->lru_prev;
        } else {
            lru_tail_ = block->lru_prev;
        }
        
        block->lru_prev = nullptr;
        block->lru_next = nullptr;
    }
    
    void promote_block(CompiledBlock* block) {
        if (block == lru_head_) return;
        
        remove_from_lru(block);
        add_to_lru_front(block);
    }
    
    // Evict least recently used block
    void evict_lru() {
        if (!lru_tail_) return;
        
        CompiledBlock* victim = lru_tail_;
        remove_block(victim);
        evictions_++;
    }
    
    // Remove block from all data structures
    void remove_block(CompiledBlock* block) {
        // Remove from hash table
        u32 hash = compute_hash(block->start_addr);
        
        if (block->hash_prev) {
            block->hash_prev->hash_next = block->hash_next;
        } else {
            hash_table_[hash] = block->hash_next;
        }
        
        if (block->hash_next) {
            block->hash_next->hash_prev = block->hash_prev;
        }
        
        // Remove from LRU
        remove_from_lru(block);
        
        // Remove from page tracking
        unregister_pages(block);
        
        // Unlink other blocks pointing to this one
        unlink_block(block);
        
        block_count_--;
        invalidations_++;
        
        delete block;
    }
    
    // Page tracking for invalidation
    void register_pages(CompiledBlock* block) {
        GuestAddr start_page = block->start_addr >> PAGE_SHIFT;
        GuestAddr end_page = (block->start_addr + block->size * 4 - 1) >> PAGE_SHIFT;
        
        for (GuestAddr page = start_page; page <= end_page; page++) {
            page_blocks_[page].push_back(block);
        }
    }
    
    void unregister_pages(CompiledBlock* block) {
        GuestAddr start_page = block->start_addr >> PAGE_SHIFT;
        GuestAddr end_page = (block->start_addr + block->size * 4 - 1) >> PAGE_SHIFT;
        
        for (GuestAddr page = start_page; page <= end_page; page++) {
            auto it = page_blocks_.find(page);
            if (it != page_blocks_.end()) {
                auto& vec = it->second;
                vec.erase(std::remove(vec.begin(), vec.end(), block), vec.end());
                if (vec.empty()) {
                    page_blocks_.erase(it);
                }
            }
        }
    }
};

// Add LRU/hash pointers to CompiledBlock (extend the struct in jit.h)
// For now, we use a simple external approach

} // namespace x360mu
