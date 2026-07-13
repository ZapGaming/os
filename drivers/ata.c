#include <drivers/ata.h>
#include <kernel/io.h>
#include <kernel/serial.h>

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

static int disk_present = 0;

static void ata_wait_bsy(void) {
    while (inb(REG_STATUS) & STATUS_BSY);
}

/* Polls until the drive is ready to transfer data or reports an error.
 * Returns 1 if DRQ is set (ready), 0 on ERR/DF. */
static int ata_wait_drq(void) {
    for (;;) {
        uint8_t status = inb(REG_STATUS);
        if (status & (STATUS_ERR | STATUS_DF)) return 0;
        if (status & STATUS_DRQ) return 1;
    }
}

int ata_init(void) {
    outb(REG_DRIVE_HEAD, 0xA0); /* master, no LBA bits needed for IDENTIFY */
    outb(REG_SECCOUNT, 0);
    outb(REG_LBA_LOW, 0);
    outb(REG_LBA_MID, 0);
    outb(REG_LBA_HIGH, 0);
    outb(REG_COMMAND, CMD_IDENTIFY);

    uint8_t status = inb(REG_STATUS);
    if (status == 0) {
        serial_printf("ata: no drive on primary master\n");
        return 0;
    }

    ata_wait_bsy();

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
    uint16_t *out = (uint16_t *)buf;

    ata_wait_bsy();
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
    const uint16_t *in = (const uint16_t *)buf;

    ata_wait_bsy();
    ata_setup_lba(lba, count);
    outb(REG_COMMAND, CMD_WRITE_SECTORS);

    for (int s = 0; s < count; s++) {
        if (!ata_wait_drq()) return 0;
        for (int i = 0; i < 256; i++) outw(REG_DATA, *in++);
    }

    ata_wait_bsy();
    outb(REG_COMMAND, CMD_CACHE_FLUSH);
    ata_wait_bsy();
    return 1;
}
