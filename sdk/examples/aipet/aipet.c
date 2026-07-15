/* "Pixel Pet" -- the flagship ZapOS SDK example. A Tamagotchi-style
 * virtual pet that lives in its own BORDERLESS, TRANSPARENT desktop
 * window (via zos_win_open(..., ZOS_WIN_BORDERLESS)/zos_win_blit(), see
 * sdk/zapos.h) -- a real floating face on the desktop, no rectangular
 * window box, no title bar, no frame: just the pet's own alpha-
 * composited pixels. It reacts to keypresses and draws its own animated
 * face with plain integer geometry.
 *
 * IMPORTANT, read this before assuming otherwise: the "AI" in "AI pet"
 * is a simple, fully deterministic RULE-BASED STATE MACHINE -- two
 * integers (hunger/happiness) that decay on a timer and get bumped by
 * keypresses, with the face picked by a handful of if/else thresholds.
 * There is NO trained model, NO neural network, NO machine learning of
 * any kind here -- ZapOS has no ML runtime anywhere in it to run one
 * even if this wanted to. If you came here expecting an LLM-driven
 * pet, this isn't that; it's the same kind of logic real 1990s-era
 * Tamagotchi toys ran on far weaker hardware than this kernel emulates.
 *
 * Build (the "full gcc path" -- see sdk/README.md section 2):
 *   gcc -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
 *       -fno-builtin -nostdlib -O2 -mno-sse -mno-sse2 -mno-mmx \
 *       -mno-80387 -mgeneral-regs-only -I../.. \
 *       -c aipet.c -o aipet.o
 *   ld -m elf_i386 -T ../../../userprogs/user.ld -nostdlib \
 *       -o AIPET.ELF aipet.o
 * Then get AIPET.ELF onto zapos_disk.img (see sdk/README.md) and run it
 * from the File Manager or Terminal. Once running, click into the
 * ZapOS desktop and press:
 *   f -- feed the pet (raises hunger satisfaction)
 *   p -- pet/play with it (raises happiness)
 * (Every task shares ZapOS's one single keyboard queue -- there's no
 * per-window input focus yet, so these keys reach THIS pet whenever
 * it's the process actually polling for them; see sdk/README.md.)
 */
#include "../../zapos.h"

#define WIN_W 160
#define WIN_H 120

/* Raw PS/2 Set-1 scancodes for 'f' and 'p' -- zos_poll_key() hands back
 * scancodes, not ASCII (see its doc comment in sdk/zapos.h), so keys
 * are matched here the same way drivers/keyboard.c's own
 * scancode_ascii[] table (and userprogs/doom/doomgeneric_zapos.c's
 * DG_GetKey(), the other consumer of this same raw event queue) does:
 * index 0x21 in that table is 'f', index 0x19 is 'p'. */
#define SCANCODE_F 0x21
#define SCANCODE_P 0x19

/* Tuning constants -- all in PIT ticks (100/sec, see zos_get_ticks()).
 * Chosen so the pet's state visibly drifts over tens of seconds, not
 * so fast it feels twitchy and not so slow a quick test never sees it
 * move. */
#define DECAY_INTERVAL_TICKS 150  /* ~1.5s per -1 hunger/happiness point */
#define BLINK_PERIOD_TICKS   220  /* ~2.2s between blinks */
#define BLINK_DURATION_TICKS 12   /* ~120ms eyes-closed */
#define STATUS_INTERVAL_TICKS 400 /* ~4s between "stats readout" lines */
#define LOOP_SLEEP_MS 50          /* ~20fps redraw/input-poll rate */

/* The pet's whole pixel buffer -- an app window's backing store is
 * exactly a `uint32_t` 0xAARRGGBB array (top byte = alpha), row-major
 * top-to-bottom (see
 * zos_win_blit()'s doc comment). Global, not a stack local: WIN_W*
 * WIN_H*4 bytes (76800) is comfortably fine as static storage but
 * would be a needlessly large stack frame. */
static unsigned int pixels[WIN_W * WIN_H];

static void put_px(int x, int y, unsigned int color) {
    if (x < 0 || x >= WIN_W || y < 0 || y >= WIN_H) return;
    pixels[y * WIN_W + x] = color;
}

static void fill_rect(int x0, int y0, int x1, int y1, unsigned int color) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            put_px(x, y, color);
}

/* Filled circle via the integer midpoint ("Bresenham") circle
 * algorithm -- no floats, no sqrt: each step advances y by 1 and only
 * decides whether x also needs to shrink by comparing an integer error
 * term against zero. Draws four horizontal spans per step (the two
 * octant pairs the algorithm naturally produces), which is what turns
 * the usual "just the outline" version into a solid fill. */
static void fill_circle(int cx, int cy, int r, unsigned int color) {
    int x = r, y = 0, err = 0;
    while (x >= y) {
        fill_rect(cx - x, cy + y, cx + x, cy + y, color);
        fill_rect(cx - x, cy - y, cx + x, cy - y, color);
        fill_rect(cx - y, cy + x, cx + y, cy + x, color);
        fill_rect(cx - y, cy - x, cx + y, cy - x, color);
        y++;
        if (err <= 0) err += 2 * y + 1;
        if (err > 0) { x--; err -= 2 * x + 1; }
    }
}

/* Simple integer-parabola mouth: `bow` = dx*dx/16 is a non-negative
 * "how far from center" term (0 at the middle, largest at the
 * corners); `amplitude` is its value at the corners. A smile is a cup
 * shape -- corners raised (smaller y/higher on screen) and the center
 * dipping down (larger y) -- so the center gets +amplitude/2 and each
 * corner gets -bow; a frown is a cap shape -- the mirror image, center
 * raised and corners drooping -- so the signs simply flip. No floats,
 * no trig anywhere: just one squared integer and one division. */
static void draw_mouth(int cx, int base_y, int half_width, int smile, unsigned int color) {
    int amplitude = (half_width * half_width) / 16;
    for (int dx = -half_width; dx <= half_width; dx++) {
        int bow = (dx * dx) / 16;
        int y = smile ? base_y + amplitude / 2 - bow : base_y - amplitude / 2 + bow;
        put_px(cx + dx, y, color);
        put_px(cx + dx, y + 1, color); /* 2px thick so it reads clearly at this resolution */
    }
}

/* Redraws the whole face into `pixels` from scratch every call --
 * cheap enough at 160x120 to just always fully repaint rather than
 * track dirty regions. `hunger`/`happiness` are each 0-100 (100 =
 * great, 0 = neglected); `blinking` closes the eyes for this frame.
 *
 * The window is borderless (see _start()'s zos_win_open() call) and
 * every app-window pixel is now 0xAARRGGBB (top byte = alpha, see
 * sdk/zapos.h's zos_win_blit() doc comment) -- so every color constant
 * below explicitly carries 0xFF000000 (fully opaque) in its top byte.
 * There is no rectangular background fill anymore: the area outside the
 * face circle is left fully TRANSPARENT (alpha 0, RGB value irrelevant
 * since a 0-alpha pixel is skipped outright by the compositor), so only
 * the round face itself is ever visible, floating directly on the
 * desktop -- no box around it. */
static void draw_face(int hunger, int happiness, int blinking) {
    int mood = (hunger + happiness) / 2; /* 0-100, the simple rule this whole face is driven by */

    /* Fully transparent -- color value 0 is fine, alpha 0 means it's
     * never actually drawn (see blit_app_window_pixels() in
     * gui/compositor.c's alpha=0 early-out). */
    fill_rect(0, 0, WIN_W - 1, WIN_H - 1, 0);

    int cx = WIN_W / 2, cy = WIN_H / 2;
    int head_r = 44;

    /* The face circle's own base color now carries the mood signal that
     * used to live on the (now-gone) rectangular background: the exact
     * same integer lerp from a reddish (mood low) to a greenish (mood
     * high) tone, same thresholds, just tinting the face instead of a
     * background rect -- purely linear, no easing, deliberately simple. */
    unsigned int face_r = (unsigned int)(100 - mood) * 180 / 100 + 40;
    unsigned int face_g = (unsigned int)mood * 170 / 100 + 30;
    unsigned int face_b = 50;
    if (face_r > 255) face_r = 255;
    if (face_g > 255) face_g = 255;
    fill_circle(cx, cy, head_r, 0xFF000000 | (face_r << 16) | (face_g << 8) | face_b);

    int eye_dx = 16, eye_y = cy - 10;
    if (blinking) {
        fill_rect(cx - eye_dx - 6, eye_y, cx - eye_dx + 6, eye_y + 2, 0xFF2A2010);
        fill_rect(cx + eye_dx - 6, eye_y, cx + eye_dx + 6, eye_y + 2, 0xFF2A2010);
    } else {
        fill_circle(cx - eye_dx, eye_y, 6, 0xFFFFFFFF);
        fill_circle(cx + eye_dx, eye_y, 6, 0xFFFFFFFF);
        fill_circle(cx - eye_dx, eye_y, 3, 0xFF1A1A1A);
        fill_circle(cx + eye_dx, eye_y, 3, 0xFF1A1A1A);
    }

    /* Smile once mood is comfortably above the midpoint, frown once
     * comfortably below it -- a small dead zone around 50 avoids the
     * mouth flickering between the two shapes on every single tick. */
    int smile = mood > 55;
    draw_mouth(cx, cy + 18, 18, smile, 0xFF2A2010);
}

/* Hand-written unsigned-to-decimal, appended into `buf` starting at
 * `*pos` -- there's no sprintf/itoa without libc, and zos_write() only
 * takes a ready-made NUL-terminated string. */
static void append_uint(char *buf, int *pos, unsigned int val) {
    char digits[12];
    int n = 0;
    if (val == 0) digits[n++] = '0';
    while (val > 0) { digits[n++] = (char)('0' + val % 10); val /= 10; }
    while (n > 0) buf[(*pos)++] = digits[--n];
}

/* The pet's "stats readout" -- since there's no text-drawing primitive
 * for app windows (only raw pixels), this is how a human watching the
 * serial log / Terminal actually sees the numeric hunger/happiness
 * state, same spirit as any of this kernel's other periodic
 * zos_write()-based status lines. */
static void write_status(int hunger, int happiness) {
    char line[64];
    int pos = 0;
    const char *p1 = "hunger: ";
    for (const char *p = p1; *p; p++) line[pos++] = *p;
    append_uint(line, &pos, (unsigned int)hunger);
    const char *p2 = " happiness: ";
    for (const char *p = p2; *p; p++) line[pos++] = *p;
    append_uint(line, &pos, (unsigned int)happiness);
    line[pos++] = '\n';
    line[pos] = 0;
    zos_write(line);
}

void _start(void) {
    /* ZOS_WIN_BORDERLESS: no rounded frame, no drop shadow, no title
     * bar -- just this pet's own alpha-composited pixels floating
     * directly on the desktop, which is the whole point of a "desktop
     * pet" (see sdk/zapos.h's zos_win_open() doc comment). */
    unsigned int win = zos_win_open("Pixel Pet", WIN_W, WIN_H, ZOS_WIN_BORDERLESS);
    if (win == (unsigned int)-1) {
        zos_write("aipet: zos_win_open failed (no free window slot?)\n");
        zos_exit();
    }

    int hunger = 80, happiness = 80;
    unsigned int last_decay = zos_get_ticks();
    unsigned int last_status = last_decay;

    for (;;) {
        unsigned int now = zos_get_ticks();

        if (now - last_decay >= DECAY_INTERVAL_TICKS) {
            last_decay = now;
            if (hunger > 0) hunger--;
            if (happiness > 0) happiness--;
        }

        /* Drain every pending key event this tick -- poll_key is
         * non-blocking and only ever returns one event per call, so a
         * key pressed while we were asleep (see zos_sleep() below)
         * needs draining in a loop, not a single check. */
        for (int key = zos_poll_key(); key >= 0; key = zos_poll_key()) {
            int pressed = (key >> 8) & 1;
            int scancode = key & 0xFF;
            if (!pressed) continue; /* only react to key-down, not key-up */
            if (scancode == SCANCODE_F) { hunger += 20; if (hunger > 100) hunger = 100; }
            else if (scancode == SCANCODE_P) { happiness += 20; if (happiness > 100) happiness = 100; }
        }

        int phase = (int)(now % BLINK_PERIOD_TICKS);
        int blinking = phase < BLINK_DURATION_TICKS;

        draw_face(hunger, happiness, blinking);
        zos_win_blit(win, pixels);

        if (now - last_status >= STATUS_INTERVAL_TICKS) {
            last_status = now;
            write_status(hunger, happiness);
        }

        zos_sleep(LOOP_SLEEP_MS);
    }
}
