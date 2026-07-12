#ifndef KERNEL_MULTIBOOT_H
#define KERNEL_MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT2_MAGIC 0x36d76289

struct mb_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;   /* 1 = available RAM */
    uint32_t reserved;
} __attribute__((packed));

struct mb_parsed_info {
    uint64_t highest_usable_addr;

    int has_framebuffer;
    uint64_t fb_addr;
    uint32_t fb_pitch;
    uint32_t fb_width;
    uint32_t fb_height;
    uint8_t  fb_bpp;

    struct mb_mmap_entry mmap[64];
    uint32_t mmap_count;
};

void multiboot_parse(uint32_t info_addr, struct mb_parsed_info *out);

#endif
