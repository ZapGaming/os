//
// ZapOS replacement for i_video.c/i_input.c.
//
// This is the input glue (translate ZapOS keyboard scancodes to DOOM
// key codes and post them as real d_event.h events) plus the indexed-
// palette-to-RGB conversion that feeds doomgeneric's own DG_DrawFrame
// -- the two jobs the stock i_video.c bundles together for every
// platform. Everything fbdev/X11/gamma/mouse-specific in the original
// is gone: there's no framebuffer device to open (DG_Init/DG_DrawFrame
// in doomgeneric_zapos.c own that, via SYS_BLIT), and no mouse support
// at all yet.

#include "doomtype.h"
#include "d_event.h"
#include "doomkeys.h"
#include "i_video.h"
#include "doomgeneric.h"
#include "zapos_syscalls.h"
#include <string.h>

char *video_driver = "";
boolean screenvisible = true;
boolean screensaver_mode = false;
int usegamma = 0;
int vanilla_keyboard_mapping = 1;
byte *I_VideoBuffer = NULL;

int screen_width = SCREENWIDTH;
int screen_height = SCREENHEIGHT;
int screen_bpp = 8;
int fullscreen = 1;
int aspect_ratio_correct = 1;
int show_diskicon = 0;
int diskicon_readbytes = 0;

static struct { byte r, g, b; } palette[256];

/* Raw scancode (SYS_POLL_KEY's low byte -- PS/2 set-1 make codes,
 * same numbering as this kernel's own drivers/keyboard.c) -> DOOM key
 * code (doomkeys.h), covering everything the default control scheme
 * and menus need. Printable ASCII keys fall through to the second
 * table below instead of this one. */
static unsigned char convert_special_key(unsigned char sc) {
    switch (sc) {
        case 0x01: return KEY_ESCAPE;
        case 0x0E: return KEY_BACKSPACE;
        case 0x0F: return KEY_TAB;
        case 0x1C: return KEY_ENTER;
        case 0x1D: return KEY_RCTRL;   /* either ctrl -> fire */
        case 0x2A: case 0x36: return KEY_RSHIFT; /* either shift -> run */
        case 0x38: return KEY_RALT;    /* either alt -> strafe */
        case 0x39: return ' ';         /* space -> use */
        case 0x48: return KEY_UPARROW;
        case 0x50: return KEY_DOWNARROW;
        case 0x4B: return KEY_LEFTARROW;
        case 0x4D: return KEY_RIGHTARROW;
        case 0x0C: return KEY_MINUS;
        case 0x0D: return KEY_EQUALS;
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        default: return 0;
    }
}

/* Plain digits/letters -- DOOM just wants their ASCII value as both
 * the key code and the typed character, same as the original scancode
 * ASCII tables use (weapon-select 1-7, y/n prompts, menu text). */
static const char scancode_ascii[] = {
    0,0,'1','2','3','4','5','6','7','8','9','0',0,0,0,0,
    'q','w','e','r','t','y','u','i','o','p',0,0,0,0,
    'a','s','d','f','g','h','j','k','l',0,0,0,0,0,
    'z','x','c','v','b','n','m',
};

static void I_GetEvent(void) {
    for (;;) {
        int packed = sys_poll_key();
        if (packed < 0) break;

        int pressed = (packed >> 8) & 1;
        unsigned char sc = (unsigned char)(packed & 0x7F);

        unsigned char doomkey = convert_special_key(sc);
        if (!doomkey && sc < sizeof(scancode_ascii)) doomkey = (unsigned char)scancode_ascii[sc];
        if (!doomkey) continue;

        event_t ev;
        ev.type = pressed ? ev_keydown : ev_keyup;
        ev.data1 = doomkey;
        ev.data2 = doomkey;
        ev.data3 = 0;
        D_PostEvent(&ev);
    }
}

void I_InitGraphics(void) {
    I_VideoBuffer = (byte *)malloc(SCREENWIDTH * SCREENHEIGHT);
    memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT);
    screenvisible = true;
}

void I_GraphicsCheckCommandLine(void) { }
void I_ShutdownGraphics(void) { if (I_VideoBuffer) free(I_VideoBuffer); }

void I_SetPalette(byte *pal) {
    for (int i = 0; i < 256; i++) {
        palette[i].r = *pal++;
        palette[i].g = *pal++;
        palette[i].b = *pal++;
    }
}

int I_GetPaletteIndex(int r, int g, int b) {
    int best = 0, best_diff = 0x7fffffff;
    for (int i = 0; i < 256; i++) {
        int dr = palette[i].r - r, dg = palette[i].g - g, db = palette[i].b - b;
        int diff = dr * dr + dg * dg + db * db;
        if (diff < best_diff) { best_diff = diff; best = i; }
    }
    return best;
}

void I_UpdateNoBlit(void) { }

void I_FinishUpdate(void) {
    for (int i = 0; i < SCREENWIDTH * SCREENHEIGHT; i++) {
        byte idx = I_VideoBuffer[i];
        DG_ScreenBuffer[i] = ((uint32_t)palette[idx].r << 16) |
                             ((uint32_t)palette[idx].g << 8) |
                             (uint32_t)palette[idx].b;
    }
    DG_DrawFrame();
}

void I_ReadScreen(byte *scr) { memcpy(scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT); }
void I_BeginRead(void) { }
void I_EndRead(void) { }

void I_SetWindowTitle(char *title) { DG_SetWindowTitle(title); }
void I_CheckIsScreensaver(void) { }
void I_SetGrabMouseCallback(grabmouse_callback_t func) { (void)func; }
void I_DisplayFPSDots(boolean dots_on) { (void)dots_on; }
void I_BindVideoVariables(void) { }
void I_InitWindowTitle(void) { }
void I_InitWindowIcon(void) { }
void I_EnableLoadingDisk(void) { }

void I_StartFrame(void) { }
void I_StartTic(void) { I_GetEvent(); }
