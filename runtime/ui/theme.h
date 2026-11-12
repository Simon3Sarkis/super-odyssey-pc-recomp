// Super Odyssey PC Recomp's look (runtime/ui): one palette for the installer, the menu, its option rows, the Controls
// page, the on-screen keyboard - and the icon (tools/make_icon.py): deep sky-blue panels, white text, pipe-green
// accents (buttons, the selection), gold for values that aren't the default.
#pragma once
#include "imgui.h"

#define THEME_RGB(r, g, b, a) ImVec4((r) / 255.0f, (g) / 255.0f, (b) / 255.0f, (a))
// the palette
static const ImVec4 kPanelV = THEME_RGB(30, 58, 110, 0.97f);        // the menu window, the installer's card
static const ImVec4 kSurfaceV = THEME_RGB(42, 77, 140, 1);          // fields, buttons, rows
static const ImVec4 kSurfaceHoverV = THEME_RGB(56, 96, 165, 1);     // ... under the mouse / the cursor
static const ImVec4 kAccentV = THEME_RGB(59, 196, 90, 1);           // the pipe green (the icon's pipe)
static const ImVec4 kAccentDeepV = THEME_RGB(28, 112, 52, 1);       // ... pressed / active fields
static const ImVec4 kAccentSoftV = THEME_RGB(245, 197, 66, 1);      // gold, as text: a value that isn't the default
static const ImVec4 kTextV = THEME_RGB(255, 255, 255, 1);
static const ImVec4 kTextDimV = THEME_RGB(190, 206, 234, 1);        // secondary text, tabs not shown
static const ImVec4 kTextOffV = THEME_RGB(120, 142, 182, 1);        // greyed out
static const ImVec4 kLineV = THEME_RGB(255, 255, 255, 0.14f);       // separators
static const ImVec4 kWarnV = THEME_RGB(224, 154, 43, 1);            // a conflict (one key bound twice)

static inline ImU32 theme_u32(ImVec4 c, float alpha = -1) {
    if (alpha >= 0) c.w = alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}
#define kAccent theme_u32(kAccentV)
#define kAccentDim theme_u32(kAccentV, 0.43f)
#define kText theme_u32(kTextV)
#define kTextDim theme_u32(kTextDimV)
#define kTextOff theme_u32(kTextOffV)

// The icon's pipe (tools/make_icon.py) in a square of `size` at p: a green pipe with its wider lip, a light stripe and
// a dark shade, and a small gold star above it. bg: the colour behind it (unused, kept for the callers).
static inline void theme_logo(ImDrawList *dl, ImVec2 p, float size, ImU32 col, ImU32 bg) {
    (void)bg;
    float u = size / 16;  // the icon's 16-pixel grid
    auto r = [&](float x, float y, float w, float h, ImU32 c) {
        dl->AddRectFilled(ImVec2(p.x + x * u, p.y + y * u), ImVec2(p.x + (x + w) * u, p.y + (y + h) * u), c);
    };
    ImU32 dark = IM_COL32(16, 80, 36, 255), light = IM_COL32(150, 236, 130, 255), edge = IM_COL32(8, 40, 18, 255);
    ImU32 gold = IM_COL32(245, 197, 66, 255);
    r(4, 9, 8, 7, edge), r(5, 9, 6, 7, col), r(6, 9, 1, 7, light), r(9, 9, 2, 7, dark);  // the body
    r(3, 6, 10, 4, edge), r(3.5f, 6.5f, 9, 3, col), r(4.5f, 6.5f, 1, 3, light), r(10, 6.5f, 2.5f, 3, dark);  // the lip
    r(7, 1, 2, 4, gold), r(6, 2, 4, 2, gold), r(5, 2.5f, 6, 1, gold);  // the star
}
