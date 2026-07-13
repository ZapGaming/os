#ifndef DRIVERS_ATA_H
#define DRIVERS_ATA_H

#include <stdint.h>

#define ATA_SECTOR_SIZE 512

/* Probes the primary IDE bus for a master hard disk (skips ATAPI drives,
 * e.g. the boot CD-ROM, which lives on its own channel/slot). Returns 1
 * if a usable disk was found. */
int ata_init(void);

int ata_read_sectors(uint32_t lba, uint8_t count, void *buf);
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf);

/* Fallback path for when there's no real ATA hardware disk: redirects
 * every read/write above to an in-memory image instead (e.g. a GRUB
 * module embedded in the ISO -- see kernel.c). `base` must already be
 * mapped (true everywhere under this kernel's full identity map). */
void ata_use_ram_disk(void *base, uint32_t size);

#endif
