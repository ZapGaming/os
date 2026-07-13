#include <drivers/rtl8139.h>
#include <drivers/pci.h>
#include <kernel/io.h>
#include <kernel/idt.h>
#include <kernel/pic.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <string.h>

#define REG_MAC0    0x00
#define REG_TSD0    0x10
#define REG_TSAD0   0x20
#define REG_RBSTART 0x30
#define REG_CR      0x37
#define REG_CAPR    0x38
#define REG_IMR     0x3C
#define REG_ISR     0x3E
#define REG_RCR     0x44
#define REG_CONFIG1 0x52

#define CR_BUFE 0x01
#define CR_TE   0x04
#define CR_RE   0x08
#define CR_RST  0x10

#define ISR_ROK 0x0001
#define ISR_TOK 0x0004

#define RX_RING_SIZE 8192
#define RX_BUFFER_PAD 1792 /* room for one max-size frame to overflow past the ring end (WRAP mode) */
#define TX_BUFFER_SIZE 1536
#define TX_DESCRIPTORS 4

static uint16_t io_base = 0;
static uint8_t *rx_buffer = NULL;
static uint32_t rx_offset = 0;
static uint8_t *tx_buffers[TX_DESCRIPTORS];
static int tx_next = 0;
static rtl8139_rx_handler_t rx_handler = NULL;

static void rtl8139_receive_packets(void) {
    while (!(inb(io_base + REG_CR) & CR_BUFE)) {
        uint8_t *packet = rx_buffer + rx_offset;
        uint16_t rx_status = *(uint16_t *)packet;
        uint16_t rx_len = *(uint16_t *)(packet + 2); /* includes 4-byte trailing CRC */

        if ((rx_status & 0x01) && rx_len >= 4 && rx_len <= 1518) {
            if (rx_handler) rx_handler(packet + 4, rx_len - 4);
        }

        /* advance past this packet's 4-byte header + payload, 4-byte aligned */
        rx_offset = (rx_offset + rx_len + 4 + 3) & ~3u;
        rx_offset %= RX_RING_SIZE;

        /* the NIC prefetches its next header 16 bytes ahead of CAPR, hence
         * the "-16" -- this is a documented quirk of the chip, not a bug */
        outw(io_base + REG_CAPR, (uint16_t)(rx_offset - 16));
    }
}

static void rtl8139_irq_handler(struct registers *regs) {
    (void)regs;
    uint16_t status = inw(io_base + REG_ISR);
    outw(io_base + REG_ISR, status); /* acknowledge */

    if (status & ISR_ROK) {
        rtl8139_receive_packets();
    }
}

int rtl8139_init(void) {
    struct pci_device dev;
    if (!pci_find_device(0x10EC, 0x8139, &dev)) {
        serial_printf("rtl8139: no device found\n");
        return 0;
    }

    pci_enable_bus_mastering(&dev);
    io_base = (uint16_t)(dev.bar0 & 0xFFFC); /* BAR0 bit0=1 marks it an I/O-space BAR */

    outb(io_base + REG_CONFIG1, 0x00); /* power on */

    outb(io_base + REG_CR, CR_RST);
    for (int timeout = 1000000; timeout > 0 && (inb(io_base + REG_CR) & CR_RST); timeout--);

    rx_buffer = (uint8_t *)kmalloc(RX_RING_SIZE + RX_BUFFER_PAD);
    memset(rx_buffer, 0, RX_RING_SIZE + RX_BUFFER_PAD);
    outl(io_base + REG_RBSTART, (uint32_t)(uintptr_t)rx_buffer);

    for (int i = 0; i < TX_DESCRIPTORS; i++) {
        tx_buffers[i] = (uint8_t *)kmalloc(TX_BUFFER_SIZE);
    }

    outw(io_base + REG_IMR, ISR_ROK | ISR_TOK);
    outl(io_base + REG_RCR, 0x0F | 0x80); /* accept all/phys/multi/broadcast, WRAP */
    outb(io_base + REG_CR, CR_RE | CR_TE);

    register_interrupt_handler((uint8_t)(32 + dev.interrupt_line), rtl8139_irq_handler);
    pic_clear_mask(dev.interrupt_line);

    uint8_t mac[6];
    rtl8139_get_mac(mac);
    serial_printf("rtl8139: mac=%x:%x:%x:%x:%x:%x io_base=%x irq=%d\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], io_base, dev.interrupt_line);

    return 1;
}

void rtl8139_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = inb(io_base + REG_MAC0 + i);
}

void rtl8139_send(const void *data, uint16_t len) {
    if (len > TX_BUFFER_SIZE) len = TX_BUFFER_SIZE;

    int idx = tx_next;
    tx_next = (tx_next + 1) % TX_DESCRIPTORS;

    memcpy(tx_buffers[idx], data, len);
    outl(io_base + REG_TSAD0 + idx * 4, (uint32_t)(uintptr_t)tx_buffers[idx]);
    outl(io_base + REG_TSD0 + idx * 4, len);
}

void rtl8139_set_rx_handler(rtl8139_rx_handler_t handler) {
    rx_handler = handler;
}
