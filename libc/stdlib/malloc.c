#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HEAP_ALIGNMENT 16U
#define HEAP_CHUNK_SIZE 4096U

typedef union heap_block heap_block_t;
union heap_block {
    struct {
        size_t size;
        heap_block_t* next;
        int free;
    } info;
    uint8_t alignment[HEAP_ALIGNMENT];
};

static heap_block_t* heap_head;
static heap_block_t* heap_tail;

static size_t align_size(size_t size) {
    return (size + HEAP_ALIGNMENT - 1) & ~(HEAP_ALIGNMENT - 1);
}

static heap_block_t* extend_heap(size_t size) {
    size_t payload_size = size > HEAP_CHUNK_SIZE ? size : HEAP_CHUNK_SIZE;
    if (payload_size > SIZE_MAX - sizeof(heap_block_t) -
                           (HEAP_ALIGNMENT - 1)) {
        return 0;
    }
    size_t total = align_size(payload_size + sizeof(heap_block_t));
    heap_block_t* block = (heap_block_t*)sbrk((intptr_t)total);
    if (block == (void*)-1) {
        return 0;
    }
    block->info.size = total - sizeof(*block);
    block->info.next = 0;
    block->info.free = 1;
    if (heap_tail) {
        heap_tail->info.next = block;
    } else {
        heap_head = block;
    }
    heap_tail = block;
    return block;
}

static void split_block(heap_block_t* block, size_t size) {
    if (block->info.size < size + sizeof(heap_block_t) + HEAP_ALIGNMENT) {
        return;
    }
    heap_block_t* remainder =
        (heap_block_t*)((uint8_t*)(block + 1) + size);
    remainder->info.size = block->info.size - size - sizeof(*block);
    remainder->info.next = block->info.next;
    remainder->info.free = 1;
    block->info.size = size;
    block->info.next = remainder;
    if (heap_tail == block) {
        heap_tail = remainder;
    }
}

void* malloc(size_t size) {
    if (!size) {
        size = 1;
    }
    if (size > SIZE_MAX - HEAP_ALIGNMENT) {
        return 0;
    }
    size = align_size(size);

    for (heap_block_t* block = heap_head; block; block = block->info.next) {
        if (block->info.free && block->info.size >= size) {
            split_block(block, size);
            block->info.free = 0;
            return block + 1;
        }
    }

    heap_block_t* block = extend_heap(size);
    if (!block) {
        return 0;
    }
    split_block(block, size);
    block->info.free = 0;
    return block + 1;
}

void free(void* pointer) {
    if (!pointer) {
        return;
    }
    heap_block_t* block = (heap_block_t*)pointer - 1;
    int belongs_to_heap = 0;
    for (heap_block_t* current = heap_head; current;
         current = current->info.next) {
        if (current == block) {
            belongs_to_heap = 1;
            break;
        }
    }
    if (!belongs_to_heap || block->info.free) {
        return;
    }
    block->info.free = 1;
    for (heap_block_t* current = heap_head; current && current->info.next;) {
        heap_block_t* next = current->info.next;
        if (current->info.free && next->info.free &&
            (uint8_t*)(current + 1) + current->info.size ==
                (uint8_t*)next) {
            current->info.size += sizeof(*next) + next->info.size;
            current->info.next = next->info.next;
            if (heap_tail == next) {
                heap_tail = current;
            }
        } else {
            current = next;
        }
    }
}

void* calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) {
        return 0;
    }
    size_t total = count * size;
    void* memory = malloc(total);
    if (memory) {
        memset(memory, 0, total);
    }
    return memory;
}

void* realloc(void* pointer, size_t size) {
    if (!pointer) {
        return malloc(size);
    }
    if (!size) {
        free(pointer);
        return 0;
    }
    heap_block_t* block = (heap_block_t*)pointer - 1;
    if (block->info.size >= size) {
        return pointer;
    }
    void* replacement = malloc(size);
    if (!replacement) {
        return 0;
    }
    memcpy(replacement, pointer, block->info.size);
    free(pointer);
    return replacement;
}
