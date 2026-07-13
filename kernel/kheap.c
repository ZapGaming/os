#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <stdint.h>

/* Was 8MB, comfortable back when the framebuffer was 1024x768 (a ~3MB
 * back buffer). At the higher resolution boot/multiboot.asm now
 * requests, the back buffer alone can be ~8MB (1920x1080x32bpp), which
 * left almost nothing for the browser's own fetch/image/CSS buffers on
 * top of everything else sharing this arena. There's no shortage of
 * physical RAM to back a bigger static arena with (pmm typically finds
 * well over 100MB free), so just give it more room. */
#define HEAP_SIZE (32u * 1024 * 1024)

static uint8_t heap_arena[HEAP_SIZE] __attribute__((aligned(16)));

struct block_header {
    size_t size;               /* size of the usable region that follows */
    int free;
    struct block_header *next;
};

static struct block_header *heap_head;

void kheap_init(void) {
    heap_head = (struct block_header *)heap_arena;
    heap_head->size = HEAP_SIZE - sizeof(struct block_header);
    heap_head->free = 1;
    heap_head->next = NULL;
    serial_printf("kheap: %u KB arena ready\n", HEAP_SIZE / 1024);
}

static void split_block(struct block_header *block, size_t size) {
    size_t remaining = block->size - size;
    if (remaining <= sizeof(struct block_header) + 16) return; /* not worth splitting */

    struct block_header *new_block =
        (struct block_header *)((uint8_t *)(block + 1) + size);
    new_block->size = remaining - sizeof(struct block_header);
    new_block->free = 1;
    new_block->next = block->next;

    block->size = size;
    block->next = new_block;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + 15) & ~((size_t)15); /* 16-byte align */

    for (struct block_header *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split_block(b, size);
            b->free = 0;
            return (void *)(b + 1);
        }
    }
    return NULL; /* heap exhausted */
}

static void coalesce(void) {
    for (struct block_header *b = heap_head; b && b->next; b = b->next) {
        if (b->free && b->next->free) {
            b->size += sizeof(struct block_header) + b->next->size;
            b->next = b->next->next;
        }
    }
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct block_header *b = (struct block_header *)ptr - 1;
    b->free = 1;
    coalesce();
}
