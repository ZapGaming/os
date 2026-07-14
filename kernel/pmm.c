#include <kernel/pmm.h>
#include <kernel/serial.h>
#include <string.h>

#define MAX_TRACKED_MEM (256u * 1024 * 1024)
#define MAX_FRAMES (MAX_TRACKED_MEM / PMM_FRAME_SIZE)

extern uint8_t kernel_start[];
extern uint8_t kernel_end[];

static uint8_t frame_bitmap[MAX_FRAMES / 8];
static uint32_t total_frames = 0;
static uint32_t free_frames = 0;

static inline void bitmap_set(uint32_t bit) {
    frame_bitmap[bit / 8] |= (1 << (bit % 8));
}
static inline void bitmap_clear(uint32_t bit) {
    frame_bitmap[bit / 8] &= ~(1 << (bit % 8));
}
static inline int bitmap_test(uint32_t bit) {
    return frame_bitmap[bit / 8] & (1 << (bit % 8));
}

static void mark_used_range(uint64_t start, uint64_t length) {
    uint32_t first = (uint32_t)(start / PMM_FRAME_SIZE);
    uint32_t count = (uint32_t)((length + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t frame = first + i;
        if (frame >= total_frames) continue;
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            free_frames--;
        }
    }
}

void pmm_init(const struct mb_parsed_info *info) {
    uint64_t highest = info->highest_usable_addr;
    if (highest > MAX_TRACKED_MEM) highest = MAX_TRACKED_MEM;

    total_frames = (uint32_t)(highest / PMM_FRAME_SIZE);
    if (total_frames > MAX_FRAMES) total_frames = MAX_FRAMES;

    /* Start with everything marked used; free only the RAM the bootloader
     * reported as available, then re-reserve the kernel image and the low
     * 1 MiB (BIOS/IVT/video memory). */
    memset(frame_bitmap, 0xFF, sizeof(frame_bitmap));
    free_frames = 0;

    for (uint32_t i = 0; i < info->mmap_count; i++) {
        if (info->mmap[i].type != 1) continue;
        uint64_t base = info->mmap[i].base_addr;
        uint64_t len  = info->mmap[i].length;
        uint32_t first = (uint32_t)(base / PMM_FRAME_SIZE);
        uint32_t count = (uint32_t)(len / PMM_FRAME_SIZE);
        for (uint32_t f = first; f < first + count && f < total_frames; f++) {
            if (bitmap_test(f)) {
                bitmap_clear(f);
                free_frames++;
            }
        }
    }

    mark_used_range(0, 0x100000); /* low 1 MiB */
    mark_used_range((uint64_t)(uintptr_t)kernel_start,
                     (uint64_t)(uintptr_t)kernel_end - (uint64_t)(uintptr_t)kernel_start);

    serial_printf("pmm: %u total frames, %u free (%u KB)\n",
                  total_frames, free_frames, free_frames * 4);
}

uint32_t pmm_alloc_frame(void) {
    for (uint32_t i = 0; i < total_frames; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_frames--;
            return i * PMM_FRAME_SIZE;
        }
    }
    return 0; /* out of memory */
}

void pmm_free_frame(uint32_t frame_addr) {
    uint32_t frame = frame_addr / PMM_FRAME_SIZE;
    if (frame >= total_frames) return;
    if (bitmap_test(frame)) {
        bitmap_clear(frame);
        free_frames++;
    }
}

uint32_t pmm_free_frame_count(void) {
    return free_frames;
}

/* See the doc comment in pmm.h -- deliberately a single-use flag, not a
 * general sub-1MB allocator. If a second caller ever needs a low frame,
 * that's the signal to design a real one, not to loosen this. */
static int low_frame_claimed = 0;

uint32_t pmm_alloc_low_frame(uint32_t phys_addr) {
    if (phys_addr >= 0x100000 || (phys_addr & (PMM_FRAME_SIZE - 1)) != 0) {
        serial_printf("pmm: pmm_alloc_low_frame(%x) rejected -- not a page-aligned sub-1MB address\n", phys_addr);
        return 0;
    }
    if (low_frame_claimed) {
        serial_printf("pmm: pmm_alloc_low_frame(%x) rejected -- a low frame was already claimed\n", phys_addr);
        return 0;
    }
    low_frame_claimed = 1;
    serial_printf("pmm: carved out low frame %x from the sub-1MB reservation\n", phys_addr);
    return phys_addr;
}
