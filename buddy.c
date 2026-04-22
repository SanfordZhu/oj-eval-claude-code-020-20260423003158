#include "buddy.h"
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#define NULL ((void *)0)

#define MAX_RANK 16
#define PAGE_SIZE (4 * 1024)

// Simple bitmap-based buddy system
static void *memory_base = NULL;
static int total_pages = 0;
static int max_rank = 0;

// Free lists: array of linked lists for each rank
typedef struct Block {
    struct Block *next;
    int index;  // Block index at this rank
} Block;

static Block *free_lists[MAX_RANK + 1];  // Index 1..MAX_RANK

// Allocation bitmap: 1 bit per page (allocated or free)
static unsigned char *alloc_bitmap = NULL;
static int bitmap_size = 0;

// Helper: get block index from address
static int addr_to_block_index(void *p, int rank) {
    char *char_p = (char *)p;
    char *char_base = (char *)memory_base;
    ptrdiff_t offset = char_p - char_base;

    int block_size = PAGE_SIZE * (1 << (rank - 1));
    return offset / block_size;
}

// Helper: get address from block index
static void *block_index_to_addr(int index, int rank) {
    int block_size = PAGE_SIZE * (1 << (rank - 1));
    return (char *)memory_base + index * block_size;
}

// Helper: mark pages in bitmap
static void mark_pages(int start_page, int num_pages, int allocated) {
    for (int i = 0; i < num_pages; i++) {
        int page_idx = start_page + i;
        if (page_idx < bitmap_size) {
            if (allocated) {
                alloc_bitmap[page_idx] = 1;
            } else {
                alloc_bitmap[page_idx] = 0;
            }
        }
    }
}

// Helper: check if pages are allocated
static int check_pages_allocated(int start_page, int num_pages) {
    for (int i = 0; i < num_pages; i++) {
        int page_idx = start_page + i;
        if (page_idx < bitmap_size && alloc_bitmap[page_idx]) {
            return 1;
        }
    }
    return 0;
}

// Helper: find buddy index
static int get_buddy_index(int index, int rank) {
    return index ^ 1;  // XOR with 1 toggles last bit
}

// Helper: add block to free list
static void add_free_block(int rank, int index) {
    Block *block = (Block *)malloc(sizeof(Block));
    if (!block) return;

    block->index = index;
    block->next = free_lists[rank];
    free_lists[rank] = block;
}

// Helper: remove block from free list
static int remove_free_block(int rank, int *index) {
    if (!free_lists[rank]) return 0;

    Block *block = free_lists[rank];
    *index = block->index;
    free_lists[rank] = block->next;
    free(block);

    return 1;
}

int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0) {
        return -EINVAL;
    }

    memory_base = p;
    total_pages = pgcount;

    // Calculate maximum rank
    max_rank = 1;
    while ((1 << (max_rank - 1)) <= pgcount && max_rank <= MAX_RANK) {
        max_rank++;
    }
    max_rank--;

    // Initialize free lists
    for (int i = 0; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }

    // Allocate bitmap
    bitmap_size = pgcount;
    alloc_bitmap = (unsigned char *)calloc(pgcount, sizeof(unsigned char));
    if (!alloc_bitmap) {
        return -ENOSPC;
    }

    // Add initial free block(s)
    // Add largest possible blocks
    int remaining = pgcount;
    int current_rank = max_rank;

    while (remaining > 0 && current_rank >= 1) {
        int block_pages = 1 << (current_rank - 1);
        if (block_pages <= remaining) {
            int num_blocks = remaining / block_pages;
            for (int i = 0; i < num_blocks; i++) {
                int block_index = (remaining - block_pages * (i + 1)) / block_pages;
                add_free_block(current_rank, block_index);
            }
            remaining %= block_pages;
        }
        current_rank--;
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return ERR_PTR(-EINVAL);
    }

    if (!memory_base || rank > max_rank) {
        return ERR_PTR(-ENOSPC);
    }

    // Try to find free block of this rank
    int index;
    if (remove_free_block(rank, &index)) {
        // Mark pages as allocated
        int start_page = index * (1 << (rank - 1));
        int num_pages = 1 << (rank - 1);
        mark_pages(start_page, num_pages, 1);

        return block_index_to_addr(index, rank);
    }

    // Try to split larger blocks
    for (int r = rank + 1; r <= max_rank; r++) {
        if (free_lists[r]) {
            // Remove a block of rank r
            int large_index;
            if (remove_free_block(r, &large_index)) {
                // Split it repeatedly until we get rank
                int current_rank = r;
                int current_index = large_index;

                while (current_rank > rank) {
                    // Split into two blocks of rank current_rank-1
                    int left_index = current_index * 2;
                    int right_index = left_index + 1;

                    // Add right block to free list
                    add_free_block(current_rank - 1, right_index);

                    // Continue with left block
                    current_index = left_index;
                    current_rank--;
                }

                // Now we have a block of requested rank
                // Mark pages as allocated
                int start_page = current_index * (1 << (rank - 1));
                int num_pages = 1 << (rank - 1);
                mark_pages(start_page, num_pages, 1);

                return block_index_to_addr(current_index, rank);
            }
        }
    }

    return ERR_PTR(-ENOSPC);
}

int return_pages(void *p) {
    if (!p || !memory_base) {
        return -EINVAL;
    }

    // Check if p is within range
    char *char_p = (char *)p;
    char *char_base = (char *)memory_base;
    char *char_end = char_base + total_pages * PAGE_SIZE;

    if (char_p < char_base || char_p >= char_end) {
        return -EINVAL;
    }

    // Check alignment
    ptrdiff_t offset = char_p - char_base;
    if (offset % PAGE_SIZE != 0) {
        return -EINVAL;
    }

    // Find the rank of allocated block
    // Try all ranks from 1 to max_rank
    for (int rank = 1; rank <= max_rank; rank++) {
        int block_size = PAGE_SIZE * (1 << (rank - 1));
        if (offset % block_size == 0) {
            int index = offset / block_size;
            int max_blocks = total_pages / (1 << (rank - 1));
            if (index < max_blocks) {
                // Check if this block is allocated
                int start_page = index * (1 << (rank - 1));
                int num_pages = 1 << (rank - 1);

                if (check_pages_allocated(start_page, num_pages)) {
                    // Free it
                    mark_pages(start_page, num_pages, 0);

                    // Add to free list and merge with buddy
                    int current_index = index;
                    int current_rank = rank;

                    while (current_rank < max_rank) {
                        // Try to merge with buddy
                        int buddy_index = get_buddy_index(current_index, current_rank);

                        // Check if buddy is free
                        int buddy_free = 0;
                        Block *prev = NULL;
                        Block *curr = free_lists[current_rank];
                        while (curr) {
                            if (curr->index == buddy_index) {
                                // Remove buddy from free list
                                if (prev) {
                                    prev->next = curr->next;
                                } else {
                                    free_lists[current_rank] = curr->next;
                                }
                                free(curr);
                                buddy_free = 1;
                                break;
                            }
                            prev = curr;
                            curr = curr->next;
                        }

                        if (buddy_free) {
                            // Merge with buddy
                            current_index = current_index / 2;  // Parent index
                            current_rank++;
                        } else {
                            break;
                        }
                    }

                    // Add merged block to free list
                    add_free_block(current_rank, current_index);

                    return OK;
                }
            }
        }
    }

    return -EINVAL;
}

int query_ranks(void *p) {
    if (!p || !memory_base) {
        return -EINVAL;
    }

    // Check if p is within range
    char *char_p = (char *)p;
    char *char_base = (char *)memory_base;
    char *char_end = char_base + total_pages * PAGE_SIZE;

    if (char_p < char_base || char_p >= char_end) {
        return -EINVAL;
    }

    ptrdiff_t offset = char_p - char_base;
    if (offset % PAGE_SIZE != 0) {
        return -EINVAL;
    }

    // For allocated pages: return actual rank
    // For unallocated: return maximum rank containing this page

    // Check all ranks from max_rank down to 1
    for (int rank = max_rank; rank >= 1; rank--) {
        int block_size = PAGE_SIZE * (1 << (rank - 1));
        if (offset % block_size == 0) {
            int index = offset / block_size;
            int max_blocks = total_pages / (1 << (rank - 1));
            if (index < max_blocks) {
                // Check if any page in this block is allocated
                int start_page = index * (1 << (rank - 1));
                int num_pages = 1 << (rank - 1);

                if (check_pages_allocated(start_page, num_pages)) {
                    // Some page in block is allocated
                    // Need to find which rank exactly
                    // For simplicity, if any page allocated, return 1
                    // Actually need to check smaller blocks
                    for (int r = 1; r <= rank; r++) {
                        int sub_block_size = PAGE_SIZE * (1 << (r - 1));
                        if (offset % sub_block_size == 0) {
                            int sub_index = offset / sub_block_size;
                            int sub_start = sub_index * (1 << (r - 1));
                            int sub_num = 1 << (r - 1);
                            if (check_pages_allocated(sub_start, sub_num)) {
                                return r;
                            }
                        }
                    }
                    return 1;
                } else {
                    // Entire block is free
                    return rank;
                }
            }
        }
    }

    return -EINVAL;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return -EINVAL;
    }

    if (!memory_base || rank > max_rank) {
        return 0;
    }

    // Count blocks in free list
    int count = 0;
    Block *curr = free_lists[rank];
    while (curr) {
        count++;
        curr = curr->next;
    }

    return count;
}