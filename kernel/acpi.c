/* ACPI table parsing -- just enough to find the MADT (the table that
 * lists every CPU's Local APIC ID and the Local APIC's MMIO address) and
 * nothing else. No AML/DSDT interpretation, no other ACPI table is even
 * looked at -- this pass's entire use for ACPI is "how many CPUs are
 * there and where do I poke to wake one." */
#include <kernel/acpi.h>
#include <kernel/serial.h>
#include <string.h>
#include <stddef.h>

struct acpi_rsdp {
    char     signature[8]; /* "RSD PTR " (note trailing space) */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    /* ACPI 2.0+ fields -- only valid if revision >= 2. */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt_header header;
    uint32_t local_apic_address;
    uint32_t flags;
    uint8_t  entries[]; /* variable-length list of madt_entry_header-prefixed records */
} __attribute__((packed));

struct madt_entry_header {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

#define MADT_ENTRY_LOCAL_APIC 0

struct madt_local_apic {
    struct madt_entry_header hdr;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags; /* bit 0 = enabled */
} __attribute__((packed));

#define MADT_LOCAL_APIC_ENABLED (1u << 0)

static uint8_t sum_bytes(const void *ptr, uint32_t len) {
    const uint8_t *p = (const uint8_t *)ptr;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
    return sum;
}

/* ACPI spec 5.2.5.1's documented RSDP search algorithm: the first 1KB of
 * the Extended BIOS Data Area (whose segment lives at physical 0x40E),
 * then the BIOS read-only memory space 0xE0000-0xFFFFF, both scanned in
 * 16-byte increments for the 8-byte "RSD PTR " signature. Every access
 * below is a plain physical-address dereference -- safe because
 * paging_init() (kernel/paging.c) already identity-maps all of low
 * memory by the time this runs. */
/* GCC's -Warray-bounds assumes address 0 is the base of a zero-sized
 * object and flags any small fixed physical address dereferenced this
 * way as an out-of-bounds access ("source object is likely at address
 * zero") -- a well-known false positive for exactly this kind of BIOS
 * Data Area read, which every real-mode-adjacent OS does. There's no
 * array here at all, just a single volatile read of a known-good
 * physical address; suppressed locally rather than restructured. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
static struct acpi_rsdp *find_rsdp(void) {
    uint16_t ebda_seg = *(const volatile uint16_t *)(uintptr_t)0x40E;
    uint32_t ebda_addr = (uint32_t)ebda_seg << 4;

    struct { uint32_t start, end; int valid; } regions[2] = {
        { ebda_addr, ebda_addr + 1024, ebda_seg != 0 && ebda_addr >= 0x1000 },
        { 0xE0000u, 0x100000u, 1 },
    };

    for (int r = 0; r < 2; r++) {
        if (!regions[r].valid) continue;
        for (uint32_t addr = regions[r].start; addr + 8 <= regions[r].end; addr += 16) {
            struct acpi_rsdp *rsdp = (struct acpi_rsdp *)(uintptr_t)addr;
            if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) continue;
            if (sum_bytes(rsdp, 20) != 0) continue; /* ACPI 1.0 checksum, first 20 bytes */
            if (rsdp->revision >= 2 && sum_bytes(rsdp, rsdp->length) != 0) continue; /* extended checksum */
            return rsdp;
        }
    }
    return NULL;
}
#pragma GCC diagnostic pop

static struct acpi_madt *find_madt(const struct acpi_rsdp *rsdp) {
    int use_xsdt = (rsdp->revision >= 2 && rsdp->xsdt_address != 0);
    struct acpi_sdt_header *sdt = use_xsdt
        ? (struct acpi_sdt_header *)(uintptr_t)rsdp->xsdt_address
        : (struct acpi_sdt_header *)(uintptr_t)rsdp->rsdt_address;

    if (sum_bytes(sdt, sdt->length) != 0) {
        serial_printf("acpi: %s at %x failed checksum, aborting\n",
                       use_xsdt ? "XSDT" : "RSDT", (uint32_t)(uintptr_t)sdt);
        return NULL;
    }

    uint32_t entry_size = use_xsdt ? 8u : 4u;
    uint8_t *entries = (uint8_t *)sdt + sizeof(struct acpi_sdt_header);
    uint32_t count = (sdt->length - (uint32_t)sizeof(struct acpi_sdt_header)) / entry_size;

    serial_printf("acpi: %s at %x, %u table pointer(s)\n",
                   use_xsdt ? "XSDT" : "RSDT", (uint32_t)(uintptr_t)sdt, count);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t table_addr;
        if (use_xsdt) {
            uint64_t addr64;
            memcpy(&addr64, entries + (uint32_t)i * 8u, 8);
            table_addr = (uint32_t)addr64; /* tables above 4GB can't exist on this 32-bit kernel anyway */
        } else {
            memcpy(&table_addr, entries + (uint32_t)i * 4u, 4);
        }

        struct acpi_sdt_header *table = (struct acpi_sdt_header *)(uintptr_t)table_addr;
        if (memcmp(table->signature, "APIC", 4) == 0) return (struct acpi_madt *)table;
    }
    return NULL;
}

int acpi_find_cpus(struct acpi_cpu_info *out) {
    memset(out, 0, sizeof(*out));

    struct acpi_rsdp *rsdp = find_rsdp();
    if (!rsdp) {
        serial_printf("acpi: no RSDP found in EBDA or 0xE0000-0xFFFFF -- no usable ACPI tables\n");
        return 0;
    }
    serial_printf("acpi: RSDP found at %x (ACPI revision %u)\n", (uint32_t)(uintptr_t)rsdp, rsdp->revision);

    struct acpi_madt *madt = find_madt(rsdp);
    if (!madt) {
        serial_printf("acpi: no MADT (APIC table) found in RSDT/XSDT\n");
        return 0;
    }
    serial_printf("acpi: MADT found at %x, Local APIC address=%x\n",
                   (uint32_t)(uintptr_t)madt, madt->local_apic_address);
    out->lapic_phys_addr = madt->local_apic_address;

    uint8_t *entries = madt->entries;
    uint8_t *end = (uint8_t *)madt + madt->header.length;
    while (entries + sizeof(struct madt_entry_header) <= end) {
        struct madt_entry_header *eh = (struct madt_entry_header *)entries;
        if (eh->length < sizeof(struct madt_entry_header)) break; /* malformed entry -- stop rather than loop forever */

        if (eh->type == MADT_ENTRY_LOCAL_APIC && eh->length >= sizeof(struct madt_local_apic)) {
            struct madt_local_apic *lapic = (struct madt_local_apic *)entries;
            int enabled = (lapic->flags & MADT_LOCAL_APIC_ENABLED) != 0;
            serial_printf("acpi: CPU entry: processor_id=%u apic_id=%u enabled=%d\n",
                           lapic->acpi_processor_id, lapic->apic_id, enabled);
            if (enabled && out->cpu_count < ACPI_MAX_CPUS) {
                out->apic_ids[out->cpu_count++] = lapic->apic_id;
            }
        }
        entries += eh->length;
    }

    serial_printf("acpi: %u enabled CPU(s) reported by MADT\n", out->cpu_count);
    return 1;
}
