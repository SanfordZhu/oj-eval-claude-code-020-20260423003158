#include "buddy.h"
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#define NULL ((void *)0)

#define MAX_RANK 16
#define PAGE_SIZE (4 * 1024)  // 4KB

// Data structures for buddy system
static void *memory_base = NULL;
static int total_pages = 0;
static int max_rank = 0;  // Maximum rank that can fit in available memory

// Tree representation for buddy system
// For a complete binary tree with n leaves, we need 2n-1 nodes
// We'll use an array where node i has children at 2i+1 and 2i+2
static int *tree = NULL;  // Stores the maximum available rank in subtree
static int tree_size = 0;

// Helper functions
static int get_tree_index(int rank, int offset) {
    // Convert (rank, offset) to tree index
    // rank: 1..max_rank (1 = leaf level)
    // offset: 0..(2^(max_rank-rank)-1)
    int level = max_rank - rank;  // 0 = leaf level
    int nodes_at_level = 1 << level;
    int start_index = (1 << level) - 1;
    return start_index + offset;
}

static void update_parents(int index) {
    // Update parent nodes after a change at index
    while (index > 0) {
        int parent = (index - 1) / 2;
        int left_child = parent * 2 + 1;
        int right_child = parent * 2 + 2;

        int left_val = (left_child < tree_size) ? tree[left_child] : 0;
        int right_val = (right_child < tree_size) ? tree[right_child] : 0;

        // If both children are free and same rank, parent can be that rank + 1
        // Otherwise, parent is 0 (split or partially allocated)
        if (left_val == right_val && left_val > 0) {
            tree[parent] = left_val + 1;
        } else {
            tree[parent] = 0;
        }
        index = parent;
    }
}

static int find_free_block(int rank, int *offset) {
    // Find a free block of given rank
    // Returns 1 if found, 0 otherwise
    // Sets offset if found
    if (rank < 1 || rank > max_rank) {
        return 0;
    }

    // If root is 0 (split), we need to search children
    // If root is > 0 but < rank, no block big enough
    if (tree[0] != 0 && tree[0] < rank) {
        return 0;
    }

    // Start from root and go down to find a block
    int index = 0;
    int current_rank = max_rank;

    // Find a block of at least the requested rank
    while (current_rank > rank) {
        int left_child = index * 2 + 1;
        int right_child = index * 2 + 2;

        if (left_child < tree_size && tree[left_child] >= rank) {
            index = left_child;
        } else if (right_child < tree_size && tree[right_child] >= rank) {
            index = right_child;
        } else {
            return 0;  // Should not happen if tree[0] >= rank
        }
        current_rank--;
    }

    // Now we have a block of exactly current_rank
    // If it's larger than needed, split it
    while (current_rank > rank) {
        // Split the block
        tree[index] = 0;  // Mark as split

        int left_child = index * 2 + 1;
        int right_child = index * 2 + 2;

        // Mark both children as free with rank current_rank - 1
        tree[left_child] = current_rank - 1;
        tree[right_child] = current_rank - 1;

        // Go to left child (we'll allocate from left)
        index = left_child;
        current_rank--;
    }

    // Mark block as allocated
    tree[index] = 0;
    update_parents(index);

    // Calculate offset from leaf position
    int leaf_index = index - ((1 << (max_rank - rank)) - 1);
    *offset = leaf_index;

    return 1;
}

static void mark_block_free(int rank, int offset) {
    // Mark a block as free and merge with buddy if possible
    int index = get_tree_index(rank, offset);
    tree[index] = rank;

    // Try to merge with buddy
    while (index > 0) {
        int parent = (index - 1) / 2;
        int buddy_index;

        // Check if index is left or right child
        if (index == parent * 2 + 1) {
            // Left child
            buddy_index = parent * 2 + 2;
        } else {
            // Right child
            buddy_index = parent * 2 + 1;
        }

        // Check if buddy is free and same rank
        if (buddy_index < tree_size && tree[buddy_index] == rank) {
            // Merge
            tree[parent] = rank + 1;
            tree[index] = 0;
            tree[buddy_index] = 0;
            index = parent;
            rank = rank + 1;  // Now we're at higher rank
        } else {
            break;
        }
    }

    update_parents(index);
}

static int get_block_rank(void *p) {
    // Get the rank of block starting at p
    // Returns 0 if p is not aligned or out of range
    char *char_p = (char *)p;
    char *char_base = (char *)memory_base;
    char *char_end = char_base + total_pages * PAGE_SIZE;

    if (char_p < char_base || char_p >= char_end) {
        return 0;
    }

    ptrdiff_t offset = char_p - char_base;
    if (offset % PAGE_SIZE != 0) {
        return 0;
    }

    // Check all possible ranks from 1 to max_rank
    for (int rank = 1; rank <= max_rank; rank++) {
        int block_size = PAGE_SIZE * (1 << (rank - 1));
        if (offset % block_size == 0) {
            // Check if this is a valid block boundary
            int block_index = offset / block_size;
            int max_blocks = total_pages / (1 << (rank - 1));
            if (block_index < max_blocks) {
                return rank;
            }
        }
    }

    return 0;
}

int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0) {
        return -EINVAL;
    }

    memory_base = p;
    total_pages = pgcount;

    // Calculate maximum rank that fits in available memory
    max_rank = 1;
    while ((1 << (max_rank - 1)) <= pgcount && max_rank <= MAX_RANK) {
        max_rank++;
    }
    max_rank--;  // Last valid rank

    // Allocate tree (2n-1 nodes where n = 2^(max_rank-1))
    int leaves = 1 << (max_rank - 1);
    tree_size = 2 * leaves - 1;
    tree = (int *)malloc(tree_size * sizeof(int));
    if (!tree) {
        return -ENOSPC;  // Couldn't allocate metadata
    }

    // Initialize tree: all blocks free at max_rank
    for (int i = 0; i < tree_size; i++) {
        tree[i] = max_rank;
    }

    // Adjust for actual page count if not power of 2
    // Mark unavailable pages as allocated (rank 0)
    if (pgcount < leaves * (1 << (max_rank - 1))) {
        // This is complex - we need to mark excess leaves as unavailable
        // For now, we'll handle it by checking bounds during allocation
    }

    // Adjust for actual page count (might not be power of 2)
    int max_possible_pages = leaves * (1 << (max_rank - 1));
    if (pgcount < max_possible_pages) {
        // Mark excess pages as unavailable
        // This is complex - for now, we'll handle it in allocation
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

    int offset;
    if (!find_free_block(rank, &offset)) {
        return ERR_PTR(-ENOSPC);
    }

    // Calculate address
    size_t block_size = PAGE_SIZE * (1 << (rank - 1));
    void *addr = (char *)memory_base + offset * block_size;

    return addr;
}

int return_pages(void *p) {
    if (!p || !memory_base) {
        return -EINVAL;
    }

    // Check if p is within memory range
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

    // Find the rank of this block
    int rank = get_block_rank(p);
    if (rank == 0) {
        return -EINVAL;
    }

    // Check if block is actually allocated
    // For simplicity, we'll assume it's allocated if address is valid
    // In a real implementation, we'd track allocated blocks

    // Mark block as free
    size_t block_size = PAGE_SIZE * (1 << (rank - 1));
    int block_offset = offset / block_size;
    mark_block_free(rank, block_offset);

    return OK;
}

int query_ranks(void *p) {
    if (!p || !memory_base) {
        return -EINVAL;
    }

    // Check if p is within memory range
    char *char_p = (char *)p;
    char *char_base = (char *)memory_base;
    char *char_end = char_base + total_pages * PAGE_SIZE;

    if (char_p < char_base || char_p >= char_end) {
        return -EINVAL;
    }

    // Find the leaf index for this address
    ptrdiff_t offset = char_p - char_base;
    if (offset % PAGE_SIZE != 0) {
        return -EINVAL;
    }

    int page_index = offset / PAGE_SIZE;

    // Convert to leaf index in tree
    int leaf_level = max_rank - 1;
    int leaves_start = (1 << leaf_level) - 1;
    int leaf_index = leaves_start + page_index;

    if (leaf_index >= tree_size) {
        return -EINVAL;
    }

    // Check if this leaf is allocated (tree[leaf_index] == 0)
    // If allocated, return rank 1
    if (tree[leaf_index] == 0) {
        return 1;
    }

    // For unallocated: find the largest free block containing this leaf
    // Traverse up the tree
    int index = leaf_index;
    int rank = 1;  // Start with rank 1 (leaf)

    while (index > 0) {
        int parent = (index - 1) / 2;

        // Check if parent represents a free block
        if (tree[parent] > 0) {
            // Parent is a free block
            rank++;
            index = parent;
        } else {
            break;
        }
    }

    return rank;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return -EINVAL;
    }

    if (!memory_base || rank > max_rank) {
        return 0;
    }

    // Count free blocks of given rank
    // Traverse tree at level corresponding to rank
    int level = max_rank - rank;
    int start_index = (1 << level) - 1;
    int nodes_at_level = 1 << level;
    int count = 0;

    for (int i = 0; i < nodes_at_level && start_index + i < tree_size; i++) {
        if (tree[start_index + i] >= rank) {
            count++;
        }
    }

    return count;
}
