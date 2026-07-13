#include <drivers/ata.h>
#include <kernel/io.h>
#include <kernel/serial.h>
#include <string.h>

#define ATA_IO_BASE   0x1F0
#define ATA_CTRL_BASE 0x3F6

#define REG_DATA        (ATA_IO_BASE + 0)
#define REG_SECCOUNT    (ATA_IO_BASE + 2)
#define REG_LBA_LOW     (ATA_IO_BASE + 3)
#define REG_LBA_MID     (ATA_IO_BASE + 4)
#define REG_LBA_HIGH    (ATA_IO_BASE + 5)
#define REG_DRIVE_HEAD  (ATA_IO_BASE + 6)
#define REG_STATUS      (ATA_IO_BASE + 7)
#define REG_COMMAND     (ATA_IO_BASE + 7)

#define CMD_READ_SECTORS  0x20
#define CMD_WRITE_SECTORS 0x30
#define CMD_CACHE_FLUSH   0xE7
#define CMD_IDENTIFY      0xEC

#define STATUS_ERR 0x01
#define STATUS_DRQ 0x08
#define STATUS_DF  0x20
#define STATUS_BSY 0x80

/* Some emulators (QEMU) report status 0x00 on a floating/absent ATA bus;
 * others (e.g. v86, and some real hardware) leave the bus floating high,
 * which reads back as 0xFF. Either one means "nothing is there." Treating
 * only 0x00 as "absent" made ata_wait_bsy() spin forever below on a status
 * register that was permanently stuck at 0xFF (BSY always appears set)
 * whenever no drive was attached under one of those emulators. */
#define ATA_STATUS_FLOATING(s) ((s) == 0x00 || (s) == 0xFF)
#define ATA_WAIT_TIMEOUT 100000

static int disk_present = 0;

/* Non-NULL once ata_use_ram_disk() is called -- every read/write below
 * then serves out of this buffer instead of touching real I/O ports. */
static uint8_t *ram_disk = NULL;
static uint32_t ram_disk_size = 0;

void ata_use_ram_disk(void *base, uint32_t size) {
    ram_disk = (uint8_t *)base;
    ram_disk_size = size;
    disk_present = 1;
    serial_printf("ata: no hardware disk -- using embedded %u-byte RAM disk image instead\n", size);
}

/* Bounded poll -- returns 1 once BSY clears, 0 on timeout (no drive, or a
 * drive that's wedged). Never spins forever on a floating/absent bus. */
static int ata_wait_bsy(void) {
    for (uint32_t i = 0; i < ATA_WAIT_TIMEOUT; i++) {
        if (!(inb(REG_STATUS) & STATUS_BSY)) return 1;
    }
    return 0;
}

/* Polls until the drive is ready to transfer data or reports an error.
 * Returns 1 if DRQ is set (ready), 0 on ERR/DF/timeout. */
static int ata_wait_drq(void) {
    for (uint32_t i = 0; i < ATA_WAIT_TIMEOUT; i++) {
        uint8_t status = inb(REG_STATUS);
        if (status & (STATUS_ERR | STATUS_DF)) return 0;
        if (status & STATUS_DRQ) return 1;
    }
    return 0;
}

int ata_init(void) {
    outb(REG_DRIVE_HEAD, 0xA0); /* master, no LBA bits needed for IDENTIFY */
    outb(REG_SECCOUNT, 0);
    outb(REG_LBA_LOW, 0);
    outb(REG_LBA_MID, 0);
    outb(REG_LBA_HIGH, 0);
    outb(REG_COMMAND, CMD_IDENTIFY);

    uint8_t status = inb(REG_STATUS);
    if (ATA_STATUS_FLOATING(status)) {
        serial_printf("ata: no drive on primary master\n");
        return 0;
    }

    if (!ata_wait_bsy()) {
        serial_printf("ata: primary master timed out leaving BSY -- treating as absent\n");
        return 0;
    }

    /* A non-zero LBA mid/high signature here means this isn't a plain ATA
     * disk (e.g. an ATAPI CD-ROM) -- skip it, we only handle hard disks. */
    if (inb(REG_LBA_MID) || inb(REG_LBA_HIGH)) {
        serial_printf("ata: primary master is not a PATA hard disk (ATAPI?)\n");
        return 0;
    }

    if (!ata_wait_drq()) {
        serial_printf("ata: IDENTIFY failed\n");
        return 0;
    }

    for (int i = 0; i < 256; i++) inw(REG_DATA); /* discard IDENTIFY data */

    disk_present = 1;
    serial_printf("ata: primary master disk ready\n");
    return 1;
}

static void ata_setup_lba(uint32_t lba, uint8_t count) {
    outb(REG_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F)); /* master, LBA mode */
    outb(REG_SECCOUNT, count);
    outb(REG_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buf) {
    if (!disk_present) return 0;
    if (ram_disk) {
        uint32_t offset = lba * ATA_SECTOR_SIZE;
        uint32_t len = (uint32_t)count * ATA_SECTOR_SIZE;
        if (offset + len > ram_disk_size) return 0;
        memcpy(buf, ram_disk + offset, len);
        return 1;
    }
    uint16_t *out = (uint16_t *)buf;

    if (!ata_wait_bsy()) return 0;
    ata_setup_lba(lba, count);
    outb(REG_COMMAND, CMD_READ_SECTORS);

    for (int s = 0; s < count; s++) {
        if (!ata_wait_drq()) return 0;
        for (int i = 0; i < 256; i++) *out++ = inw(REG_DATA);
    }
    return 1;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf) {
    if (!disk_present) return 0;
    if (ram_disk) {
        uint32_t offset = lba * ATA_SECTOR_SIZE;
        uint32_t len = (uint32_t)count * ATA_SECTOR_SIZE;
        if (offset + len > ram_disk_size) return 0;
        memcpy(ram_disk + offset, buf, len);
        return 1;
    }
    const uint16_t *in = (const uint16_t *)buf;

    if (!ata_wait_bsy()) return 0;
    ata_setup_lba(lba, count);
    outb(REG_COMMAND, CMD_WRITE_SECTORS);

    for (int s = 0; s < count; s++) {
        if (!ata_wait_drq()) return 0;
        for (int i = 0; i < 256; i++) outw(REG_DATA, *in++);
    }

    if (!ata_wait_bsy()) return 0;
    outb(REG_COMMAND, CMD_CACHE_FLUSH);
    if (!ata_wait_bsy()) return 0;
    return 1;
}
