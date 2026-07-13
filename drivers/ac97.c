#include <drivers/ac97.h>
#include <drivers/pci.h>
#include <kernel/io.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <string.h>

#define NAM_RESET       0x00
#define NAM_MASTER_VOL  0x02
#define NAM_PCM_OUT_VOL 0x18

/* PCM OUT bus-master register block, offsets from the NABM base. */
#define NABM_PO_BDBAR 0x10 /* 4 bytes -- physical addr of the descriptor list */
#define NABM_PO_CIV   0x14 /* 1 byte, read-only -- index currently playing */
#define NABM_PO_LVI   0x15 /* 1 byte -- index of the last valid descriptor */
#define NABM_PO_SR    0x16 /* 2 bytes -- status */
#define NABM_PO_CR    0x1B /* 1 byte -- control */
#define NABM_GLOB_CNT 0x2C /* 4 bytes */

#define CR_RPBM 0x01 /* run/pause bus master */

/* Deliberately never enabled: this driver polls CIV/SR instead of
 * handling interrupts, and no IRQ handler is ever registered for the
 * AC97 PCI line. Turning on CR_IOCE (or a descriptor's own per-buffer
 * IOC flag) makes the controller assert its (level-triggered, and on
 * real hardware/QEMU's default chipset shared with the RTL8139's IRQ
 * line) interrupt on every buffer completion; since nothing ever
 * clears the condition that's asserting it, the line stays asserted
 * forever and the CPU is re-interrupted continuously -- observed as
 * the whole guest silently freezing (all serial/GUI output stops)
 * within about one buffer's playback time once a real QEMU audio
 * backend was attached (a "none"/absent backend never actually drove
 * the timed DMA far enough to trigger it, which is why this didn't
 * show up until audio was verified with `-audiodev wav`). */

#define SR_DCH  0x01 /* DMA controller halted */

#define BDL_ENTRIES 32
#define MAX_SAMPLES_PER_ENTRY 0xFFFE

struct ac97_bdl_entry {
    uint32_t addr;
    uint16_t samples;
    uint16_t flags;
} __attribute__((packed));

static uint16_t nam_base = 0, nabm_base = 0;
static int present = 0;
static struct ac97_bdl_entry *bdl = NULL;

int ac97_init(void) {
    struct pci_device dev;
    if (!pci_find_device(0x8086, 0x2415, &dev)) {
        serial_printf("ac97: no device found\n");
        return 0;
    }

    pci_enable_bus_mastering(&dev);
    nam_base = (uint16_t)(dev.bar0 & 0xFFFC);
    nabm_base = (uint16_t)(dev.bar1 & 0xFFFC);

    outw(nam_base + NAM_RESET, 1); /* mixer reset */
    for (volatile int i = 0; i < 100000; i++);

    outw(nam_base + NAM_MASTER_VOL, 0x0000);   /* 0 dB attenuation, unmuted */
    outw(nam_base + NAM_PCM_OUT_VOL, 0x0000);

    outb(nabm_base + NABM_PO_CR, 0);   /* make sure nothing is running */
    outl(nabm_base + NABM_GLOB_CNT, 0); /* polled, not interrupt-driven */

    bdl = (struct ac97_bdl_entry *)kmalloc(sizeof(struct ac97_bdl_entry) * BDL_ENTRIES);
    memset(bdl, 0, sizeof(struct ac97_bdl_entry) * BDL_ENTRIES);

    present = 1;
    serial_printf("ac97: ready, nam=%x nabm=%x\n", nam_base, nabm_base);
    return 1;
}

int ac97_is_present(void) {
    return present;
}

void ac97_stop(void) {
    if (!present) return;
    outb(nabm_base + NABM_PO_CR, 0);
    for (int i = 0; i < 100000 && !(inw(nabm_base + NABM_PO_SR) & SR_DCH); i++);
}

int ac97_play_pcm(const int16_t *data, uint32_t sample_count, int stereo) {
    (void)stereo;
    if (!present || sample_count == 0) return 0;

    uint32_t max_total = (uint32_t)MAX_SAMPLES_PER_ENTRY * BDL_ENTRIES;
    if (sample_count > max_total) {
        serial_printf("ac97: clip too long (%u samples, max %u)\n", sample_count, max_total);
        return 0;
    }

    ac97_stop();

    uint32_t remaining = sample_count;
    const int16_t *p = data;
    int idx = 0;
    while (remaining > 0 && idx < BDL_ENTRIES) {
        uint32_t chunk = remaining > MAX_SAMPLES_PER_ENTRY ? MAX_SAMPLES_PER_ENTRY : remaining;
        bdl[idx].addr = (uint32_t)(uintptr_t)p;
        bdl[idx].samples = (uint16_t)chunk;
        bdl[idx].flags = 0; /* no per-descriptor IOC -- see CR_IOCE note above */
        p += chunk;
        remaining -= chunk;
        idx++;
    }

    outl(nabm_base + NABM_PO_BDBAR, (uint32_t)(uintptr_t)bdl);
    outb(nabm_base + NABM_PO_LVI, (uint8_t)(idx - 1));
    outb(nabm_base + NABM_PO_CR, CR_RPBM); /* no CR_IOCE -- see comment on its #define */
    return 1;
}

int ac97_is_playing(void) {
    if (!present) return 0;
    return !(inw(nabm_base + NABM_PO_SR) & SR_DCH);
}
