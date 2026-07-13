#include <drivers/keyboard.h>
#include <kernel/idt.h>
#include <kernel/io.h>

#define KBD_DATA_PORT 0x60
#define BUF_SIZE 256

static char ring[BUF_SIZE];
static uint32_t ring_head = 0, ring_tail = 0;

static uint8_t key_state[128];

static int shift_held = 0;
static int ctrl_held = 0;
static int caps_lock = 0;

static const char scancode_ascii[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,
    /* rest (F-keys, numpad, etc.) unmapped */
};

static const char scancode_ascii_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,
};

static void ring_push(char c) {
    uint32_t next = (ring_head + 1) % BUF_SIZE;
    if (next == ring_tail) return; /* full, drop */
    ring[ring_head] = c;
    ring_head = next;
}

char keyboard_getchar(void) {
    if (ring_tail == ring_head) return 0;
    char c = ring[ring_tail];
    ring_tail = (ring_tail + 1) % BUF_SIZE;
    return c;
}

int keyboard_key_pressed(uint8_t scancode) {
    return key_state[scancode & 0x7F];
}

#define EVENT_BUF_SIZE 64
struct key_event { uint8_t scancode; uint8_t pressed; };
static struct key_event event_ring[EVENT_BUF_SIZE];
static uint32_t event_head = 0, event_tail = 0;

static void event_push(uint8_t scancode, uint8_t pressed) {
    uint32_t next = (event_head + 1) % EVENT_BUF_SIZE;
    if (next == event_tail) return; /* full, drop */
    event_ring[event_head].scancode = scancode;
    event_ring[event_head].pressed = pressed;
    event_head = next;
}

int keyboard_poll_event(uint8_t *scancode, uint8_t *pressed) {
    if (event_tail == event_head) return 0;
    *scancode = event_ring[event_tail].scancode;
    *pressed = event_ring[event_tail].pressed;
    event_tail = (event_tail + 1) % EVENT_BUF_SIZE;
    return 1;
}

static void keyboard_handler(struct registers *regs) {
    (void)regs;
    uint8_t code = inb(KBD_DATA_PORT);

    int release = code & 0x80;
    uint8_t sc = code & 0x7F;
    if (sc < 128) key_state[sc] = release ? 0 : 1;

    /* Every make/break goes into the raw event queue regardless of
     * whether it also has an ASCII meaning below -- SYS_POLL_KEY's
     * consumers care about press/release edges for keys (arrows, ctrl)
     * the ASCII ring never represents at all. */
    if (sc < 128) event_push(sc, (uint8_t)(!release));

    switch (sc) {
        case 0x2A: case 0x36: shift_held = !release; return; /* shift */
        case 0x1D: ctrl_held = !release; return;              /* ctrl */
        case 0x3A: if (!release) caps_lock = !caps_lock; return; /* capslock */
        default: break;
    }

    if (release) return;

    if (sc < 128) {
        int use_shift = shift_held ^ (caps_lock && ((scancode_ascii[sc] >= 'a' && scancode_ascii[sc] <= 'z')));
        char c = use_shift ? scancode_ascii_shift[sc] : scancode_ascii[sc];
        /* Ctrl+letter -> the standard C0 control code (Ctrl+A=0x01 ...
         * Ctrl+Z=0x1A), the same mapping real terminals use -- lets
         * ASCII-ring consumers (the text editor's Ctrl+S save, the
         * terminal's own future use) tell a chord apart from the bare
         * letter without needing the separate raw scancode queue. */
        if (ctrl_held && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
        else if (ctrl_held && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
        if (c) ring_push(c);
    }
}

void keyboard_init(void) {
    register_interrupt_handler(33, keyboard_handler); /* IRQ1 */
}
