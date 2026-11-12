// Keyboard and mouse button prompts (runtime/nvn/kbprompts.h).
//
// The game draws its button icons (A, B, L, ZR, the sticks, the d-pad...) from one font, DeviceFont80.bffnt, in each
// language's LayoutData/FontData.szs (the same file in every language): one 512x1024 BC4 sheet of 80x84 cells, stored
// bottom row first. Nothing of it is shipped here. At the first upload of a texture of that size, the sheet is read
// from the player's own RomFS image and compared with the texture's bytes; that texture then gets, while the player is
// on the keyboard / mouse (Controls tab "Button prompts": Auto follows the device used last), the icons below drawn in
// place of the controller's: key caps with the keys bound in the Controls tab, a WASD cluster for the left stick, a
// mouse for the right one (the mouse camera). Glyphs with no keyboard meaning (Home, Capture, the Joy-Con pictures)
// stay. Each icon is drawn inside the box the game's own glyph covered, so text around it keeps its spacing.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kbprompts.h"
#include "tegra.h"
#include "../input_bind.h"
#include "../paths.h"
#include "../romfs_read.h"
#include "../rt.h"
#include "../settings.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/imgui/imstb_truetype.h"

// ---------------------------------------------------------------- the game's sheet (read from the player's files)
static struct {
    int state;  // 0 not read yet, 1 read, -1 not found
    uint8_t *sheet;  // sheet 0's bytes as stored (block linear)
    uint32_t size, w, h, cellw, cellh, cols, rows;
} ref;

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool parse_font(const uint8_t *f, size_t n) {
    if (n < 0x40 || memcmp(f, "FFNT", 4)) return false;
    const uint8_t *finf = NULL;
    for (size_t i = 0; i + 4 <= n && i < 0x200; i++)
        if (!memcmp(f + i, "FINF", 4)) { finf = f + i; break; }
    if (!finf) return false;
    uint32_t tglp = r32(finf + 8 + 12);
    if (tglp < 8 || tglp + 0x20 > n || memcmp(f + tglp - 8, "TGLP", 4)) return false;
    const uint8_t *t = f + tglp;  // (offsets point at the section's data, past its 8-byte header)
    uint32_t cellw = t[0], cellh = t[1], sheets = t[2], size = r32(t + 4), fmt = r16(t + 10);
    uint32_t cols = r16(t + 12), rows = r16(t + 14), w = r16(t + 16), h = r16(t + 18), data = r32(t + 20);
    if (sheets != 1 || fmt != 0xC || !cellw || !cellh || data + size > n || (uint64_t)w * h / 2 > size) return false;
    ref.sheet = malloc(size);
    if (!ref.sheet) return false;
    memcpy(ref.sheet, f + data, size);
    ref.size = size, ref.w = w, ref.h = h, ref.cellw = cellw, ref.cellh = cellh, ref.cols = cols, ref.rows = rows;
    return true;
}

static void load_ref(void) {
    if (ref.state) return;
    ref.state = -1;
    static const char *const langs[] = {"USen", "EUen", "JPja", "EUfr", "EUde", "EUes", "EUit", "EUnl", "EUru",
                                        "KRko", "CNzh", "TWzh", "USes", "USfr"};
    for (size_t i = 0; i < sizeof langs / sizeof *langs && ref.state < 0; i++) {
        char path[96];
        snprintf(path, sizeof path, "LocalizedData/%s/LayoutData/FontData.szs", langs[i]);
        size_t n = 0, dn = 0, fn = 0;
        uint8_t *raw = romfs_read_file("extracted/romfs.bin", path, &n);
        if (!raw) continue;
        uint8_t *d = yaz0_decode(raw, n, &dn);
        const uint8_t *arc = d ? d : raw;
        const uint8_t *font = sarc_find(arc, d ? dn : n, "DeviceFont80.bffnt", &fn);
        if (font && parse_font(font, fn)) {
            ref.state = 1;
            LOG("[prompts] button icon sheet: %ux%u, %ux%u cells (%s)\n", ref.w, ref.h, ref.cellw, ref.cellh, langs[i]);
        }
        free(d);
        free(raw);
    }
    if (ref.state < 0) LOG("[prompts] the game's button icon font wasn't found: keyboard prompts off\n");
}

bool kbprompts_candidate(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint32_t bpb) {
    return w == 512 && h == 1024 && bw == 4 && bh == 4 && bpb == 8;  // (the sheet's shape: before reading the font)
}

// The game's texture doesn't hold the font file's bytes as they are (its blocks come in another layout), so it is
// recognized by the font's grid: in the texture, exactly as many cells as the font file's sheet has drawn, and those
// the first ones in the grid's order (glyph 0, 1, 2...), the rest empty - a text font's sheet, whose cells are another
// size, doesn't look like that. (The font file's sheet: 16-GOB blocks.)
static void bc4_decode(const uint8_t *b, uint8_t out[16]);
static void cell_ink(const uint8_t *blocks, uint32_t wb, uint32_t hb, uint32_t *ink) {
    uint32_t H = hb * 4, sw = ref.cellw + 1, sh = ref.cellh + 1;
    memset(ink, 0, ref.cols * ref.rows * sizeof *ink);
    for (uint32_t by = 0; by < hb; by++)
        for (uint32_t bx = 0; bx < wb; bx++) {
            uint8_t v[16];
            bc4_decode(blocks + ((size_t)by * wb + bx) * 8, v);
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + i % 4, y = H - 1 - (by * 4 + i / 4), col = x / sw, row = y / sh;  // (y from the top)
                if (v[i] > 128 && col < ref.cols && row < ref.rows) ink[row * ref.cols + col]++;
            }
        }
}
#define INKED 300  // (pixels: a drawn cell; the smallest icon has about a thousand)

bool kbprompts_identify(const uint8_t *blocks, uint32_t wb, uint32_t hb) {
    load_ref();
    if (ref.state != 1 || wb * 4 != ref.w || hb * 4 != ref.h) return false;
    uint32_t n = ref.cols * ref.rows;
    static uint32_t ref_drawn;  // drawn cells in the font file's sheet
    if (!ref_drawn) {
        uint8_t *lin = malloc((size_t)wb * hb * 8);
        uint32_t *ink = malloc(n * sizeof *ink);
        tegra_deswizzle(ref.sheet, lin, wb, hb, 1, 8, 4, 0);
        cell_ink(lin, wb, hb, ink);
        for (uint32_t i = 0; i < n; i++) ref_drawn += ink[i] > INKED;
        free(lin);
        free(ink);
    }
    uint32_t *ink = malloc(n * sizeof *ink), off = 0;
    cell_ink(blocks, wb, hb, ink);
    for (uint32_t i = 0; i < n; i++) off += (ink[i] > INKED) != (i < ref_drawn);
    free(ink);
    if (ref_drawn < 30 || off > 1) {
        static int nlog;
        if (getenv("ODYSSEY_LOG_PROMPTS") && nlog++ < 20)
            LOG("[prompts] a %ux%u BC4 texture isn't the icon sheet (%u of %u cells unlike it)\n", ref.w, ref.h, off, n);
        return false;
    }
    LOG("[prompts] button icons found (%u cells): keyboard and mouse icons ready\n", ref_drawn);
    return true;
}

// ---------------------------------------------------------------- which icons
static _Atomic int g_last_kb = -1;  // the device used last: 1 keyboard / mouse, 0 controller, -1 none yet
void kbprompts_note_device(bool keyboard) { g_last_kb = keyboard; }

static bool keyboard_wanted(void) {
    if (g_settings.prompts == PROMPTS_CONTROLLER) return false;
    if (g_settings.prompts == PROMPTS_KEYBOARD) return true;
    return g_last_kb == 1;
}

uint32_t kbprompts_generation(void) {
    uint32_t h = 2166136261u;  // the bindings' signature: a rebind redraws the key caps
    for (int i = 0; i < BIND_KEY_COUNT; i++) h = (h ^ (uint16_t)g_bind_key[i]) * 16777619u;
    return (h << 1 | keyboard_wanted()) | 0x80000000u;
}

// ---------------------------------------------------------------- drawing (a cell's coverage, 0..1, y down)
typedef struct { float *a; int w, h; } Canvas;
enum { FILL, CUT };

static void put(Canvas *c, int x, int y, float cov, int mode) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h || cov <= 0) return;
    float *p = &c->a[y * c->w + x];
    if (cov > 1) cov = 1;
    *p = mode == FILL ? fmaxf(*p, cov) : *p * (1 - cov);
}

typedef bool (*Inside)(const float *s, float x, float y);
// 4x4 samples per pixel over the shape's bounds
static void shape(Canvas *c, float x0, float y0, float x1, float y1, Inside in, const float *s, int mode) {
    for (int y = (int)floorf(y0); y <= (int)ceilf(y1); y++)
        for (int x = (int)floorf(x0); x <= (int)ceilf(x1); x++) {
            int hit = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) hit += in(s, x + (i + 0.5f) / 4, y + (j + 0.5f) / 4);
            put(c, x, y, hit / 16.0f, mode);
        }
}

static bool in_rrect(const float *s, float x, float y) {  // s: x0 y0 x1 y1 radius
    if (x < s[0] || x > s[2] || y < s[1] || y > s[3]) return false;
    float r = s[4], cx = fminf(fmaxf(x, s[0] + r), s[2] - r), cy = fminf(fmaxf(y, s[1] + r), s[3] - r);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}
static void rrect(Canvas *c, float x0, float y0, float x1, float y1, float r, int mode) {
    r = fminf(r, fminf(x1 - x0, y1 - y0) / 2);
    float s[5] = {x0, y0, x1, y1, r};
    shape(c, x0, y0, x1, y1, in_rrect, s, mode);
}

static bool in_tri(const float *s, float x, float y) {  // s: three corners
    float d1 = (x - s[2]) * (s[1] - s[3]) - (s[0] - s[2]) * (y - s[3]);
    float d2 = (x - s[4]) * (s[3] - s[5]) - (s[2] - s[4]) * (y - s[5]);
    float d3 = (x - s[0]) * (s[5] - s[1]) - (s[4] - s[0]) * (y - s[1]);
    return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
}
static void tri(Canvas *c, float ax, float ay, float bx, float by, float cx, float cy, int mode) {
    float s[6] = {ax, ay, bx, by, cx, cy};
    shape(c, fminf(ax, fminf(bx, cx)), fminf(ay, fminf(by, cy)), fmaxf(ax, fmaxf(bx, cx)), fmaxf(ay, fmaxf(by, cy)),
          in_tri, s, mode);
}
// an arrow head pointing dir (0 up, 1 down, 2 left, 3 right) in the square of size r around (cx, cy)
static void arrow(Canvas *c, float cx, float cy, float r, int dir, int mode) {
    float h = r * 0.87f;
    if (dir == 0) tri(c, cx, cy - h, cx - r, cy + h * 0.75f, cx + r, cy + h * 0.75f, mode);
    else if (dir == 1) tri(c, cx, cy + h, cx - r, cy - h * 0.75f, cx + r, cy - h * 0.75f, mode);
    else if (dir == 2) tri(c, cx - h, cy, cx + h * 0.75f, cy - r, cx + h * 0.75f, cy + r, mode);
    else tri(c, cx + h, cy, cx - h * 0.75f, cy - r, cx - h * 0.75f, cy + r, mode);
}

static stbtt_fontinfo g_font;
static int g_font_ok = -1;
static uint8_t *g_font_data;
static bool font_ready(void) {
    if (g_font_ok >= 0) return g_font_ok;
    g_font_ok = 0;
    char path[1024];
    if (!rt_resource("Roboto-Medium.ttf", "third_party/imgui/misc/fonts/Roboto-Medium.ttf", path, sizeof path)) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_font_data = n > 0 ? malloc((size_t)n) : NULL;
    if (g_font_data && fread(g_font_data, 1, (size_t)n, f) == (size_t)n &&
        stbtt_InitFont(&g_font, g_font_data, stbtt_GetFontOffsetForIndex(g_font_data, 0)))
        g_font_ok = 1;
    fclose(f);
    if (!g_font_ok) LOG("[prompts] the menu font couldn't be read: key caps without names\n");
    return g_font_ok;
}

// text centered on (cx, cy), its capitals at most maxh tall and the whole at most maxw wide
static void text(Canvas *c, const char *s, float cx, float cy, float maxw, float maxh, int mode) {
    if (!*s || !font_ready()) return;
    const float CAP = 0.711f;  // (Roboto's capitals: 0.711 of the em)
    float em = maxh / CAP, scale = stbtt_ScaleForMappingEmToPixels(&g_font, em), width = 0;
    for (const char *p = s; *p; p++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (unsigned char)*p, &adv, &lsb);
        width += adv * scale + (p[1] ? stbtt_GetCodepointKernAdvance(&g_font, (unsigned char)*p, (unsigned char)p[1]) * scale : 0);
    }
    if (width > maxw) { em *= maxw / width; scale = stbtt_ScaleForMappingEmToPixels(&g_font, em); width = maxw; }
    float pen = cx - width / 2, base = cy + CAP * em / 2;
    for (const char *p = s; *p; p++) {
        int ch = (unsigned char)*p, adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&g_font, ch, &adv, &lsb);
        float fx = pen - floorf(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&g_font, ch, scale, scale, fx, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            uint8_t *g = calloc((size_t)gw * gh, 1);
            stbtt_MakeCodepointBitmapSubpixel(&g_font, g, gw, gh, gw, scale, scale, fx, 0, ch);
            int ox = (int)floorf(pen) + x0, oy = (int)lroundf(base) + y0;
            for (int y = 0; y < gh; y++)
                for (int x = 0; x < gw; x++) put(c, ox + x, oy + y, g[y * gw + x] / 255.0f, mode);
            free(g);
        }
        pen += adv * scale + (p[1] ? stbtt_GetCodepointKernAdvance(&g_font, ch, (unsigned char)p[1]) * scale : 0);
    }
}

// a key's name for its cap: short words, arrows as -1..-4 (up, down, left, right)
static int key_name(int sc, char *out, size_t n) {
    switch (sc) {
    case SDL_SCANCODE_UP: return -1;
    case SDL_SCANCODE_DOWN: return -2;
    case SDL_SCANCODE_LEFT: return -3;
    case SDL_SCANCODE_RIGHT: return -4;
    default: break;
    }
    static const struct { int sc; const char *s; } shorts[] = {
        {SDL_SCANCODE_RETURN, "Enter"}, {SDL_SCANCODE_KP_ENTER, "Enter"}, {SDL_SCANCODE_BACKSPACE, "Bksp"},
        {SDL_SCANCODE_SPACE, "Space"}, {SDL_SCANCODE_ESCAPE, "Esc"}, {SDL_SCANCODE_TAB, "Tab"},
        {SDL_SCANCODE_LSHIFT, "Shift"}, {SDL_SCANCODE_RSHIFT, "Shift"}, {SDL_SCANCODE_LCTRL, "Ctrl"},
        {SDL_SCANCODE_RCTRL, "Ctrl"}, {SDL_SCANCODE_LALT, "Alt"}, {SDL_SCANCODE_RALT, "Alt"},
        {SDL_SCANCODE_CAPSLOCK, "Caps"}, {SDL_SCANCODE_DELETE, "Del"}, {SDL_SCANCODE_INSERT, "Ins"},
        {SDL_SCANCODE_HOME, "Home"}, {SDL_SCANCODE_END, "End"}, {SDL_SCANCODE_PAGEUP, "PgUp"},
        {SDL_SCANCODE_PAGEDOWN, "PgDn"},
    };
    for (size_t i = 0; i < sizeof shorts / sizeof *shorts; i++)
        if (shorts[i].sc == sc) { snprintf(out, n, "%s", shorts[i].s); return 1; }
    const char *nm = sc > 0 && sc < SDL_SCANCODE_COUNT ? SDL_GetScancodeName((SDL_Scancode)sc) : "";
    if (!nm || !*nm) { snprintf(out, n, "?"); return 1; }
    if (!strncmp(nm, "Keypad ", 7)) { snprintf(out, n, "Num%s", nm + 7); return 1; }  // ("Keypad 0" -> "Num0")
    snprintf(out, n, "%.5s", nm);
    return 1;
}

// a key cap filling the box: the cap, its top face's edge, the key's name cut out of the face
static void keycap(Canvas *c, float x0, float y0, float x1, float y1, int sc, bool small) {
    float w = x1 - x0, h = y1 - y0, m = fminf(w, h);
    rrect(c, x0, y0, x1, y1, m * 0.22f, FILL);
    float side = m * 0.09f, bottom = m * 0.2f, line = fmaxf(1.4f, m * 0.04f);
    float fx0 = x0 + side, fy0 = y0 + side, fx1 = x1 - side, fy1 = y1 - bottom;
    rrect(c, fx0, fy0, fx1, fy1, m * 0.16f, CUT);  // the face's edge: a ring...
    rrect(c, fx0 + line, fy0 + line, fx1 - line, fy1 - line, m * 0.13f, FILL);  // ...around the face
    char name[16];
    int k = key_name(sc, name, sizeof name);
    float cx = (fx0 + fx1) / 2, cy = (fy0 + fy1) / 2, fw = fx1 - fx0, fh = fy1 - fy0;
    if (k < 0) arrow(c, cx, cy, fminf(fw, fh) * 0.27f, -k - 1, CUT);
    else text(c, name, cx, cy, fw * 0.8f, fh * (small || strlen(name) > 1 ? 0.42f : 0.52f), CUT);
}

// a computer mouse: body, the two buttons' seams and the wheel; lr: arrows left and right of it
static void mouse(Canvas *c, float x0, float y0, float x1, float y1, bool lr) {
    float w = x1 - x0, h = y1 - y0, bh = h * 0.92f, bw = fminf(bh * 0.62f, w * (lr ? 0.5f : 0.9f));
    float cx = (x0 + x1) / 2, top = y0 + (h - bh) / 2;
    rrect(c, cx - bw / 2, top, cx + bw / 2, top + bh, bw * 0.48f, FILL);
    float seam = fmaxf(1.5f, bw * 0.06f), split = top + bh * 0.42f;
    rrect(c, cx - bw / 2, split - seam / 2, cx + bw / 2, split + seam / 2, 0, CUT);  // under the buttons
    rrect(c, cx - seam / 2, top, cx + seam / 2, split, 0, CUT);                      // between them
    float ww = bw * 0.2f;  // the wheel
    rrect(c, cx - ww / 2, top + bh * 0.11f, cx + ww / 2, top + bh * 0.33f, ww / 2, CUT);
    rrect(c, cx - ww / 4, top + bh * 0.14f, cx + ww / 4, top + bh * 0.30f, ww / 4, FILL);
    if (lr) {
        float r = fminf((w - bw) / 2 * 0.4f, h * 0.14f), ay = top + bh * 0.62f;
        arrow(c, x0 + (w - bw) / 4, ay, r, 2, FILL);
        arrow(c, x1 - (w - bw) / 4, ay, r, 3, FILL);
    }
}

// the left stick on keys: W over A S D (whatever is bound); ud: only up over down
static void stick_keys(Canvas *c, float x0, float y0, float x1, float y1, bool ud) {
    float w = x1 - x0, h = y1 - y0, g = 0.06f;
    if (ud) {
        float k = fminf(w * 0.9f, h / (2 + g)), cx = (x0 + x1) / 2, y = y0 + (h - k * (2 + g)) / 2;
        keycap(c, cx - k / 2, y, cx + k / 2, y + k, g_bind_key[BIND_LS_UP], true);
        keycap(c, cx - k / 2, y + k * (1 + g), cx + k / 2, y + k * (2 + g), g_bind_key[BIND_LS_DOWN], true);
        return;
    }
    float k = fminf(w / (3 + 2 * g), h / (2 + g)), cx = (x0 + x1) / 2, top = y0 + (h - k * (2 + g)) / 2;
    float by = top + k * (1 + g), lx = cx - k * (1.5f + g);
    keycap(c, cx - k / 2, top, cx + k / 2, top + k, g_bind_key[BIND_LS_UP], true);
    keycap(c, lx, by, lx + k, by + k, g_bind_key[BIND_LS_LEFT], true);
    keycap(c, cx - k / 2, by, cx + k / 2, by + k, g_bind_key[BIND_LS_DOWN], true);
    keycap(c, cx + k * (0.5f + g), by, cx + k * (1.5f + g), by + k, g_bind_key[BIND_LS_RIGHT], true);
}

// The glyphs (by index in the sheet: six to a row, the Switch's own code points E0AB.., E0E0 = A...) and what each
// shows on the keyboard. Not replaced: SL / SR, Power, Home, Capture and the controller pictures.
enum {
    G_POS_A = 0, G_POS_B, G_POS_X, G_POS_Y, G_STICK_UD, G_LSTICK_UD,  // a button by its place in the diamond
    G_RSTICK_LR, G_POS_YA, G_A, G_B, G_X, G_Y,
    G_L, G_R, G_ZL, G_ZR,
    G_UP = 18, G_DOWN, G_LEFT, G_RIGHT, G_PLUS, G_MINUS,
    G_STICK = 27, G_LSTICK, G_RSTICK, G_STICK_PRESS, G_LSTICK_PRESS, G_RSTICK_PRESS,
    G_COUNT
};
static void draw_glyph(Canvas *c, int g, float x0, float y0, float x1, float y1) {
    static const int8_t key_of[G_COUNT] = {
        [G_UP] = BIND_UP + 1, [G_DOWN] = BIND_DOWN + 1, [G_LEFT] = BIND_LEFT + 1, [G_RIGHT] = BIND_RIGHT + 1,
        [G_PLUS] = BIND_PLUS + 1, [G_MINUS] = BIND_MINUS + 1, [G_STICK_PRESS] = BIND_LSTICK + 1,
        [G_LSTICK_PRESS] = BIND_LSTICK + 1, [G_RSTICK_PRESS] = BIND_RSTICK + 1, [G_POS_A] = BIND_A + 1,
        [G_POS_B] = BIND_B + 1, [G_POS_X] = BIND_X + 1, [G_POS_Y] = BIND_Y + 1, [G_A] = BIND_A + 1,
        [G_B] = BIND_B + 1, [G_X] = BIND_X + 1, [G_Y] = BIND_Y + 1, [G_L] = BIND_L + 1, [G_R] = BIND_R + 1,
        [G_ZL] = BIND_ZL + 1, [G_ZR] = BIND_ZR + 1,
    };
    switch (g) {
    case G_STICK: case G_LSTICK: stick_keys(c, x0, y0, x1, y1, false); return;
    case G_STICK_UD: case G_LSTICK_UD: stick_keys(c, x0, y0, x1, y1, true); return;
    case G_RSTICK: mouse(c, x0, y0, x1, y1, false); return;
    case G_RSTICK_LR: mouse(c, x0, y0, x1, y1, true); return;
    case G_POS_YA: {  // "Y or A": two caps side by side
        float w = x1 - x0, k = fminf(w * 0.48f, y1 - y0), cy = (y0 + y1) / 2;
        keycap(c, x0, cy - k / 2, x0 + k, cy + k / 2, g_bind_key[BIND_Y], true);
        keycap(c, x1 - k, cy - k / 2, x1, cy + k / 2, g_bind_key[BIND_A], true);
        return;
    }
    default: {
        if (g < 0 || g >= G_COUNT || !key_of[g]) return;
        // one key: a square cap (wider for a long name), centered in the box
        char name[16];
        int sc = g_bind_key[key_of[g] - 1], k = key_name(sc, name, sizeof name);
        float h = y1 - y0, w = x1 - x0, cw = fminf(w, k > 0 && strlen(name) > 2 ? w : h), cx = (x0 + x1) / 2;
        keycap(c, cx - cw / 2, y0, cx + cw / 2, y1, sc, false);
    }
    }
}

// ---------------------------------------------------------------- BC4
static void bc4_decode(const uint8_t *b, uint8_t out[16]) {
    uint8_t p[8] = {b[0], b[1]};
    if (b[0] > b[1]) for (int i = 0; i < 6; i++) p[2 + i] = (uint8_t)(((6 - i) * b[0] + (1 + i) * b[1]) / 7);
    else { for (int i = 0; i < 4; i++) p[2 + i] = (uint8_t)(((4 - i) * b[0] + (1 + i) * b[1]) / 5); p[6] = 0, p[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = p[bits >> (3 * i) & 7];
}
static void bc4_encode(const uint8_t v[16], uint8_t *b) {
    uint8_t lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
    memset(b, 0, 8);
    b[0] = hi, b[1] = lo;
    if (hi == lo) return;  // (index 0 everywhere: the one value)
    uint8_t p[8] = {hi, lo};
    for (int i = 0; i < 6; i++) p[2 + i] = (uint8_t)(((6 - i) * hi + (1 + i) * lo) / 7);
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 999;
        for (int k = 0; k < 8; k++) { int d = abs(p[k] - v[i]); if (d < bd) bd = d, best = k; }
        bits |= (uint64_t)best << (3 * i);
    }
    for (int i = 0; i < 6; i++) b[2 + i] = (uint8_t)(bits >> (8 * i));
}

// ---------------------------------------------------------------- the patched sheet
static uint8_t *g_orig;     // the sheet's texels as the game has them (stored orientation: bottom row first)
static uint8_t *g_patched;  // BC4 blocks with the keyboard icons
static uint32_t g_patched_gen;

static void build(uint32_t gen, uint32_t wb, uint32_t hb) {
    uint32_t W = wb * 4, H = hb * 4;
    uint8_t *img = malloc((size_t)W * H);
    memcpy(img, g_orig, (size_t)W * H);
    uint32_t sw = ref.cellw + 1, sh = ref.cellh + 1;  // a cell and the line after it
    Canvas c = {calloc((size_t)sw * sh, sizeof(float)), (int)sw, (int)sh};
    uint8_t *mine = calloc((size_t)W * H, 1);  // (the cells drawn here: ODYSSEY_DUMP_PROMPTS shows only these)
    int drawn = 0;
    for (int g = 0; g < G_COUNT; g++) {
        uint32_t col = g % ref.cols, row = g / ref.cols, x0 = col * sw, y0 = row * sh;  // (y from the top)
        if (x0 + sw > W || y0 + sh > H) continue;
        // the box the game's glyph covers, to draw ours in
        int ix0 = sw, iy0 = sh, ix1 = -1, iy1 = -1;
        for (uint32_t y = 0; y < sh; y++)
            for (uint32_t x = 0; x < sw; x++)
                if (g_orig[(H - 1 - (y0 + y)) * W + x0 + x] > 32) {
                    if ((int)x < ix0) ix0 = x;
                    if ((int)x > ix1) ix1 = x;
                    if ((int)y < iy0) iy0 = y;
                    if ((int)y > iy1) iy1 = y;
                }
        if (ix1 < 0) continue;
        memset(c.a, 0, (size_t)sw * sh * sizeof(float));
        draw_glyph(&c, g, (float)ix0, (float)iy0, (float)ix1 + 1, (float)iy1 + 1);
        bool any = false;
        for (uint32_t i = 0; i < sw * sh && !any; i++) any = c.a[i] > 0;
        if (!any) continue;  // (a glyph with no keyboard meaning keeps the game's)
        for (uint32_t y = 0; y < sh; y++)
            for (uint32_t x = 0; x < sw; x++) {
                size_t a = (size_t)(H - 1 - (y0 + y)) * W + x0 + x;
                img[a] = (uint8_t)lroundf(fminf(c.a[y * sw + x], 1) * 255);
                mine[a] = 1;
            }
        drawn++;
    }
    free(c.a);
    const char *dump = getenv("ODYSSEY_DUMP_PROMPTS");  // (tests) <path>.pgm: the drawn icons only, top row first
    if (dump) {
        FILE *f = fopen(dump, "wb");
        if (f) {
            fprintf(f, "P5 %u %u 255\n", W, H);
            for (uint32_t y = 0; y < H; y++)
                for (uint32_t x = 0; x < W; x++) {
                    uint32_t ty = H - 1 - y, a = ty * W + x;  // (back to the top row first)
                    fputc(mine[a] ? img[a] : 0, f);
                }
            fclose(f);
        }
    }
    free(mine);
    if (!g_patched) g_patched = malloc((size_t)wb * hb * 8);
    for (uint32_t by = 0; by < hb; by++)
        for (uint32_t bx = 0; bx < wb; bx++) {
            uint8_t v[16];
            for (int i = 0; i < 16; i++) v[i] = img[(by * 4 + i / 4) * W + bx * 4 + i % 4];
            bc4_encode(v, g_patched + ((size_t)by * wb + bx) * 8);
        }
    free(img);
    g_patched_gen = gen;
    LOG("[prompts] keyboard icons drawn (%d)\n", drawn);
}

void kbprompts_apply(uint32_t lv, uint8_t *blocks, uint32_t wb, uint32_t hb) {
    if (lv || ref.state != 1 || wb * 4 != ref.w || hb * 4 != ref.h) return;
    if (!g_orig) {  // (the game's own texels: once, from its first upload)
        g_orig = malloc((size_t)ref.w * ref.h);
        for (uint32_t by = 0; by < hb; by++)
            for (uint32_t bx = 0; bx < wb; bx++) {
                uint8_t v[16];
                bc4_decode(blocks + ((size_t)by * wb + bx) * 8, v);
                for (int i = 0; i < 16; i++) g_orig[(by * 4 + i / 4) * ref.w + bx * 4 + i % 4] = v[i];
            }
    }
    uint32_t gen = kbprompts_generation();
    if (!(gen & 1)) return;  // controller: the game's icons, as uploaded
    if (!g_patched || g_patched_gen != gen) build(gen, wb, hb);
    memcpy(blocks, g_patched, (size_t)wb * hb * 8);
}
