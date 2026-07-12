#include <kernel/multiboot.h>
#include <kernel/serial.h>
#include <string.h>

struct mb_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

void multiboot_parse(uint32_t info_addr, struct mb_parsed_info *out) {
    memset(out, 0, sizeof(*out));

    uint32_t total_size = *(uint32_t *)info_addr;
    uint8_t *ptr = (uint8_t *)(info_addr + 8);
    uint8_t *end = (uint8_t *)(info_addr + total_size);

    while (ptr < end) {
        struct mb_tag *tag = (struct mb_tag *)ptr;
        if (tag->type == 0) break; /* end tag */

        if (tag->type == 6) { /* memory map */
            uint32_t entry_size = *(uint32_t *)(ptr + 8);
            uint8_t *entries = ptr + 16;
            uint8_t *entries_end = ptr + tag->size;
            while (entries < entries_end && out->mmap_count < 64) {
                struct mb_mmap_entry *e = (struct mb_mmap_entry *)entries;
                out->mmap[out->mmap_count++] = *e;
                if (e->type == 1) {
                    uint64_t top = e->base_addr + e->length;
                    if (top > out->highest_usable_addr) out->highest_usable_addr = top;
                }
                entries += entry_size;
            }
        } else if (tag->type == 8) { /* framebuffer info */
            struct fb_tag {
                uint32_t type, size;
                uint64_t addr;
                uint32_t pitch, width, height;
                uint8_t bpp, fb_type, reserved;
            } __attribute__((packed)) *fb = (struct fb_tag *)ptr;

            out->has_framebuffer = 1;
            out->fb_addr   = fb->addr;
            out->fb_pitch  = fb->pitch;
            out->fb_width  = fb->width;
            out->fb_height = fb->height;
            out->fb_bpp    = fb->bpp;
        }

        /* tags are 8-byte aligned */
        ptr += (tag->size + 7) & ~7u;
    }

    serial_printf("multiboot: highest usable addr=%x fb=%d %ux%ux%d pitch=%d\n",
                  (uint32_t)out->highest_usable_addr, out->has_framebuffer,
                  out->fb_width, out->fb_height, out->fb_bpp, out->fb_pitch);
}
