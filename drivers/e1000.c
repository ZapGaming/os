#include <drivers/e1000.h>
#include <drivers/pci.h>
#include <kernel/idt.h>
#include <kernel/pic.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <string.h>

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL0   0x5400
#define REG_RAH0   0x5404

#define CTRL_ASDE 0x00000020
#define CTRL_SLU  0x00000040
#define CTRL_RST  0x04000000

#define RCTL_EN     0x00000002
#define RCTL_UPE    0x00000008
#define RCTL_MPE    0x00000010
#define RCTL_BAM    0x00008000
#define RCTL_BSIZE_2048 0x00000000
#define RCTL_SECRC  0x04000000

#define TCTL_EN  0x00000002
#define TCTL_PSP 0x00000008
#define TCTL_CT(x)   ((uint32_t)(x) << 4)
#define TCTL_COLD(x) ((uint32_t)(x) << 12)

#define RX_DESCRIPTORS 32
#define TX_DESCRIPTORS 8
#define RX_BUF_SIZE 2048
#define TX_BUF_SIZE 2048

#define TXD_CMD_EOP  0x01
#define TXD_CMD_IFCS 0x02
#define TXD_CMD_RS   0x08
#define TXD_STATUS_DD 0x01

#define RXD_STATUS_DD 0x01

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

/* Common Intel 8254x-family device IDs -- QEMU's "e1000" (82540EM),
 * VMware's "E1000" virtual NIC (82545EM) and a handful of others that
 * show up on real hardware or other hypervisors. Not exhaustive, but
 * covers what this project is actually tested against. */
static const uint16_t known_device_ids[] = {
    0x100E, 0x100F, 0x1004, 0x1008, 0x1010, 0x1012, 0x101D,
    0x1026, 0x1027, 0x1028, 0x105E, 0x1075, 0x1076, 0x1077,
    0x107C, 0x108B, 0x108C, 0x1096, 0x1098, 0x1099, 0x109A,
};

static volatile uint8_t *mmio_base = NULL;
static struct e1000_rx_desc *rx_ring = NULL;
static struct e1000_tx_desc *tx_ring = NULL;
static uint8_t *rx_buffers[RX_DESCRIPTORS];
static uint8_t *tx_buffers[TX_DESCRIPTORS];
static int tx_next = 0;
static e1000_rx_handler_t rx_handler = NULL;

static uint32_t e1000_read32(uint32_t reg) {
    return *(volatile uint32_t *)(mmio_base + reg);
}
static void e1000_write32(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(mmio_base + reg) = value;
}

/* kmalloc() only guarantees 16-byte-sized blocks, not a 16-byte-aligned
 * *pointer* (the block header in front of the usable region isn't
 * itself a multiple of 16 bytes) -- so this over-allocates and rounds
 * up, which the descriptor rings need since the hardware requires
 * 16-byte alignment. Never freed; these live for the OS's lifetime. */
static void *aligned_alloc16(size_t size) {
    uint8_t *raw = (uint8_t *)kmalloc(size + 16);
    if (!raw) return NULL;
    uintptr_t addr = ((uintptr_t)raw + 15) & ~(uintptr_t)15;
    return (void *)addr;
}

static void e1000_receive_packets(void) {
    uint32_t tail = (e1000_read32(REG_RDT) + 1) % RX_DESCRIPTORS;
    while (rx_ring[tail].status & RXD_STATUS_DD) {
        if (rx_handler) rx_handler(rx_buffers[tail], rx_ring[tail].length);

        rx_ring[tail].status = 0;
        e1000_write32(REG_RDT, tail);
        tail = (tail + 1) % RX_DESCRIPTORS;
    }
}

static void e1000_irq_handler(struct registers *regs) {
    (void)regs;
    uint32_t cause = e1000_read32(REG_ICR); /* reading ICR also acknowledges */
    if (cause) e1000_receive_packets();
}

static int find_device(struct pci_device *out) {
    for (unsigned i = 0; i < sizeof(known_device_ids) / sizeof(known_device_ids[0]); i++) {
        if (pci_find_device(0x8086, known_device_ids[i], out)) return 1;
    }
    return 0;
}

int e1000_init(void) {
    struct pci_device dev;
    if (!find_device(&dev)) {
        serial_printf("e1000: no device found\n");
        return 0;
    }

    pci_enable_bus_mastering(&dev);
    mmio_base = (volatile uint8_t *)(uintptr_t)(dev.bar0 & 0xFFFFFFF0u);

    /* Reset, then wait for it to clear -- same "poll a bounded number of
     * times, don't spin forever" discipline as every other driver here. */
    e1000_write32(REG_CTRL, e1000_read32(REG_CTRL) | CTRL_RST);
    for (int timeout = 1000000; timeout > 0 && (e1000_read32(REG_CTRL) & CTRL_RST); timeout--);

    e1000_write32(REG_CTRL, e1000_read32(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    /* Required init step per the datasheet: the multicast table array
     * must be zeroed, not left however reset happened to leave it. */
    for (int i = 0; i < 128; i++) e1000_write32(REG_MTA + i * 4, 0);

    rx_ring = (struct e1000_rx_desc *)aligned_alloc16(sizeof(struct e1000_rx_desc) * RX_DESCRIPTORS);
    tx_ring = (struct e1000_tx_desc *)aligned_alloc16(sizeof(struct e1000_tx_desc) * TX_DESCRIPTORS);
    if (!rx_ring || !tx_ring) {
        serial_printf("e1000: out of memory for descriptor rings\n");
        return 0;
    }
    memset(rx_ring, 0, sizeof(struct e1000_rx_desc) * RX_DESCRIPTORS);
    memset(tx_ring, 0, sizeof(struct e1000_tx_desc) * TX_DESCRIPTORS);

    for (int i = 0; i < RX_DESCRIPTORS; i++) {
        rx_buffers[i] = (uint8_t *)kmalloc(RX_BUF_SIZE);
        rx_ring[i].addr = (uint64_t)(uintptr_t)rx_buffers[i];
    }
    for (int i = 0; i < TX_DESCRIPTORS; i++) {
        tx_buffers[i] = (uint8_t *)kmalloc(TX_BUF_SIZE);
        tx_ring[i].status = TXD_STATUS_DD; /* mark free/ready-to-use up front */
    }

    e1000_write32(REG_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    e1000_write32(REG_RDBAH, 0);
    e1000_write32(REG_RDLEN, sizeof(struct e1000_rx_desc) * RX_DESCRIPTORS);
    e1000_write32(REG_RDH, 0);
    e1000_write32(REG_RDT, RX_DESCRIPTORS - 1);
    e1000_write32(REG_RCTL, RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);

    e1000_write32(REG_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    e1000_write32(REG_TDBAH, 0);
    e1000_write32(REG_TDLEN, sizeof(struct e1000_tx_desc) * TX_DESCRIPTORS);
    e1000_write32(REG_TDH, 0);
    e1000_write32(REG_TDT, 0);
    /* Standard IEEE 802.3 full-duplex inter-packet-gap timings and
     * collision parameters -- the same values used across effectively
     * every minimal from-scratch e1000 driver, straight from the
     * datasheet's recommended defaults. */
    e1000_write32(REG_TIPG, 0x0060200A);
    e1000_write32(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT(0x0F) | TCTL_COLD(0x40));

    e1000_write32(REG_IMS, 0x1F6DC); /* the usual "everything interesting" mask */

    register_interrupt_handler((uint8_t)(32 + dev.interrupt_line), e1000_irq_handler);
    pic_clear_mask(dev.interrupt_line);

    uint8_t mac[6];
    e1000_get_mac(mac);
    serial_printf("e1000: mac=%x:%x:%x:%x:%x:%x mmio=%x irq=%d\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  (uint32_t)(uintptr_t)mmio_base, dev.interrupt_line);

    return 1;
}

void e1000_get_mac(uint8_t mac[6]) {
    /* RAL0/RAH0 are pre-loaded by firmware/hypervisor with the device's
     * assigned MAC (QEMU and VMware both do this) -- reading those is
     * simpler and more robust across emulators than an EEPROM read. */
    uint32_t low = e1000_read32(REG_RAL0);
    uint32_t high = e1000_read32(REG_RAH0);
    mac[0] = (uint8_t)(low & 0xFF);
    mac[1] = (uint8_t)((low >> 8) & 0xFF);
    mac[2] = (uint8_t)((low >> 16) & 0xFF);
    mac[3] = (uint8_t)((low >> 24) & 0xFF);
    mac[4] = (uint8_t)(high & 0xFF);
    mac[5] = (uint8_t)((high >> 8) & 0xFF);
}

void e1000_send(const void *data, uint16_t len) {
    if (len > TX_BUF_SIZE) len = TX_BUF_SIZE;

    int idx = tx_next;
    tx_next = (tx_next + 1) % TX_DESCRIPTORS;

    /* Wait for this slot's previous transmit to finish -- same bounded
     * poll discipline as everywhere else, not an unbounded spin. */
    for (int timeout = 1000000; timeout > 0 && !(tx_ring[idx].status & TXD_STATUS_DD); timeout--);

    memcpy(tx_buffers[idx], data, len);
    tx_ring[idx].addr = (uint64_t)(uintptr_t)tx_buffers[idx];
    tx_ring[idx].length = len;
    tx_ring[idx].cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    tx_ring[idx].status = 0;

    e1000_write32(REG_TDT, (uint32_t)tx_next);
}

void e1000_set_rx_handler(e1000_rx_handler_t handler) {
    rx_handler = handler;
}
