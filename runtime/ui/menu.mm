// In-game overlay menu built with Dear ImGui (Metal renderer on macOS, Vulkan elsewhere; input fed from SDL events).
//
// Threads: SDL events arrive on the main thread and are queued; the menu is laid out and drawn on the
// render thread inside backend_present, after the game image, into the same drawable.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <atomic>
#include <vector>

#include "imgui.h"
#include "option_rows.h"
#include "performance.h"
#include "imgui_internal.h"  // (NavCursorVisible)
#include "osk.h"
#include "theme.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
extern "C" bool vk_imgui_backend_init(void);  // runtime/nvn/vulkan/imgui_glue.cpp
#endif

extern "C" {
#include "../hle.h"
#include "../mods.h"
#include "../settings.h"
#include "../updater.h"
#include "menu.h"
#include "setup.h"
void input_reset(void);
void controls_bindings_draw(float scale);  // controls_ui.mm
#include "../input_bind.h"
#include "../paths.h"
}

// ------------------------------------------------------------------ state shared with the main thread
static std::atomic<bool> g_open{false};
static std::atomic<bool> g_quit_prompt{false};
static std::atomic<bool> g_setup{false};  // the first-start setup page (setup.mm) instead of the menu
static std::atomic<bool> g_text_busy{false};  // a text field / the on-screen keyboard has the keys (render thread: ui_text_busy)
static std::atomic<bool> g_want_window_apply{false};
static std::atomic<bool> g_draw_mouse_cursor{false};
static std::atomic<int> g_last_fullscreen{BORDERLESS};
extern "C" bool menu_stats_visible(void) {
    return g_settings.show_fps || g_settings.performance_overlay;
}

struct QEvent {
    enum { KEY, ANALOG, MOUSE_POS, MOUSE_BUTTON, WHEEL, TEXT, RESET } kind;
    ImGuiKey key;
    bool down;
    float value, x, y;
    int button;
    char text[32];
};
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<QEvent> q_events;
static float g_pixel_density = 1;

static void push(const QEvent &e) {
    pthread_mutex_lock(&q_lock);
    q_events.push_back(e);
    pthread_mutex_unlock(&q_lock);
}

static void reset_menu_events(void) {
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    QEvent reset = {};
    reset.kind = QEvent::RESET;
    q_events.push_back(reset);
    pthread_mutex_unlock(&q_lock);
}

bool menu_is_open(void) { return g_open.load(); }
void menu_setup_mode(bool on) { g_setup.store(on); }
static void set_open(bool open);
extern "C" void menu_test_open(void) {
    set_open(true);
    if (getenv("ODYSSEY_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("ODYSSEY_MENU_TAB")); }
}

void menu_tabs_focus(bool first_tab);
static void set_open(bool open) {
    if (open == g_open.load()) return;
    g_open.store(open);
    reset_menu_events();
    input_reset();  // nothing held while the menu had focus leaks into the game
    bind_capture_cancel();
    if (open) {
        static bool opened;  // the controller's cursor: the first tab (Controls) the first time this session, later the
        menu_tabs_focus(!opened);  // selected one
        opened = true;
    }
    else { settings_save(); osk_cancel(); g_text_busy = false; }
}

static ImGuiKey key_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_A: return ImGuiKey_A;
    case SDL_SCANCODE_C: return ImGuiKey_C;
    case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Z: return ImGuiKey_Z;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;  // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;  // next tab
    default: return ImGuiKey_None;
    }
}

// Nintendo layout: the right face button (A) confirms, the bottom one (B) goes back.
static ImGuiKey key_from_pad(Uint8 b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceDown;   // activate
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceRight; // cancel / back
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

// ---- software keyboard (runtime/hle/swkbd.c: the game's name entry etc.) --------------------------------------
// The game thread waits in menu_keyboard_run (re-presenting the last frame); this dialog takes the text: typed
// directly when the player last used a keyboard, an on-screen keyboard (d-pad + A, B deletes, Start / + OK) when
// they last used a controller.
static std::atomic<int> g_kbd{0};  // 1 open, 2 confirmed, 3 cancelled
static std::atomic<bool> g_last_pad{false};
static char g_kbd_text[96];
static int g_kbd_max = 8;
// A notice the game thread waits on in menu_notice_run, like the name entry (runtime/hle/applets.c: a system applet
// this version has nothing to show for)
static std::atomic<int> g_note{0};  // 1 shown, 2 closed
static char g_note_title[128], g_note_text[800];
static bool capturing() { return g_open.load() || g_kbd.load() == 1 || g_note.load() == 1 || g_setup.load(); }
// Draw the settings pointer in the overlay so it also appears in fullscreen.
static void sync_mouse_cursor(bool ui_active) {
    SDL_Window *mouse_window = SDL_GetMouseFocus();
    bool focused = mouse_window && mouse_window == SDL_GetKeyboardFocus();
    bool draw_cursor = focused && g_open.load() && !g_setup.load();
    if (draw_cursor && !g_draw_mouse_cursor.load()) {
        float x, y;
        SDL_GetMouseState(&x, &y);
        QEvent q = {};
        q.kind = QEvent::MOUSE_POS;
        q.x = x * g_pixel_density;
        q.y = y * g_pixel_density;
        push(q);
    }
    g_draw_mouse_cursor.store(draw_cursor);
    bool show = !focused || (ui_active && !draw_cursor);
    if (show == SDL_CursorVisible()) return;
    if (show) SDL_ShowCursor();
    else SDL_HideCursor();
}
// (frame interpolation's presenter, runtime/nvn/vulkan/device.c, draws the overlay without the render lock) nothing open
// that takes input or runs the menu's actions, and the overlay drawn once (ImGui uploads its fonts on the first frame)
static std::atomic<bool> g_drawn;
extern "C" bool menu_overlay_lockfree(void) { return !capturing() && (!menu_stats_visible() || g_drawn.load()); }
// The game gets no input while this is true (runtime/hle/input.c): menu or name entry up.
extern "C" bool menu_input_captured(void) { return capturing(); }

bool menu_handle_event(const SDL_Event *e) {
    static SDL_JoystickID back_pad, start_pad;
    static bool stick_down[4];
    if (e->type == SDL_EVENT_WINDOW_FOCUS_GAINED) g_app_unfocused = false;
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        g_app_unfocused = true;  // (the audio's Mute in background)
        back_pad = start_pad = 0;
        for (bool &held : stick_down) held = false;
        if (capturing()) reset_menu_events();
        return false;  // also clear the game's input
    }
    if (e->type == SDL_EVENT_GAMEPAD_REMOVED) {
        if (back_pad == e->gdevice.which) back_pad = 0;
        if (start_pad == e->gdevice.which) start_pad = 0;
        if (capturing()) reset_menu_events();
        return false;
    }
    if (g_open.load() && bind_capture_event(e)) return true;  // Controls tab waiting for a new binding: it takes the press
    if (e->type == SDL_EVENT_KEY_DOWN) g_last_pad = false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // Enter confirms, Esc cancels
        if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) { g_kbd = 2; return true; }
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { g_kbd = 3; return true; }
    }
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        g_kbd = 2;
        return true;
    }
    if (g_note.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat &&  // Enter / Space / Esc: OK
        (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER ||
         e->key.scancode == SDL_SCANCODE_SPACE || e->key.scancode == SDL_SCANCODE_ESCAPE)) {
        g_note = 2;
        return true;
    }
    // typing (a text field, the on-screen keyboard, the name entry): keys are text, not menu shortcuts
    bool typing = g_kbd.load() == 1 || (g_open.load() && g_text_busy.load());
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat && !g_setup.load() && (e->key.scancode == SDL_SCANCODE_F1 ||
                                                  (e->key.scancode == SDL_SCANCODE_ESCAPE && !g_open.load()))) {
            if (!typing && !g_quit_prompt.load()) set_open(!g_open.load());
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||
                               (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) ||
                               (e->key.scancode == SDL_SCANCODE_F && (e->key.mod & SDL_KMOD_GUI) && (e->key.mod & SDL_KMOD_CTRL)))) {
            g_settings.window_mode = g_settings.window_mode == WINDOWED ? g_last_fullscreen.load() : WINDOWED;
            g_want_window_apply.store(true);
            settings_save();
            return true;
        }
        /* fallthrough */
    case SDL_EVENT_KEY_UP: {
        if (!capturing()) return false;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && e->key.repeat) return true;  // ImGui owns key repeat and text has its own events.
        ImGuiKey k = key_from_scancode(e->key.scancode);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::TEXT; strlcpy(q.text, e->text.text, sizeof q.text); push(q); }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        bool chord = back_pad && back_pad == start_pad;
        SDL_JoystickID pad = e->gbutton.which;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK && (down || back_pad == pad)) back_pad = down ? pad : 0;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START && (down || start_pad == pad)) start_pad = down ? pad : 0;
        if (down && !typing && !g_setup.load() &&
            ((!chord && back_pad && back_pad == start_pad) || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
            if (!g_quit_prompt.load()) set_open(!g_open.load());
            return true;
        }
        if (!capturing()) return false;
        ImGuiKey k = key_from_pad(e->gbutton.button);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        float v = e->gaxis.value / 32767.0f;
        QEvent q = {};
        q.kind = QEvent::ANALOG;
        bool active = capturing();
        auto direction = [&](int i, ImGuiKey key, float amount) {
            stick_down[i] = amount > (stick_down[i] ? 0.35f : 0.55f);
            if (active) { q.key = key; q.value = stick_down[i] ? amount : 0; push(q); }
        };
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            direction(0, ImGuiKey_GamepadLStickLeft, -v);
            direction(1, ImGuiKey_GamepadLStickRight, v);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            direction(2, ImGuiKey_GamepadLStickUp, -v);
            direction(3, ImGuiKey_GamepadLStickDown, v);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: if (active) { q.key = ImGuiKey_GamepadL2; q.value = v; push(q); } break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: if (active) { q.key = ImGuiKey_GamepadR2; q.value = v; push(q); } break;
        default: break;
        }
        return active;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        bool ui_active = capturing();
        sync_mouse_cursor(ui_active);
        if (!ui_active) return false;
        { QEvent q = {}; q.kind = QEvent::MOUSE_POS; q.x = e->motion.x * g_pixel_density; q.y = e->motion.y * g_pixel_density; push(q); }
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!capturing()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_BUTTON;
            q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::WHEEL; q.x = e->wheel.x; q.y = e->wheel.y; push(q); }
        return true;
    default:
        return false;
    }
}

void menu_request_window_apply(void) { g_want_window_apply.store(true); }

void menu_apply_window_requests(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    g_pixel_density = SDL_GetWindowPixelDensity(w);
    static bool text_input;
    bool want_text = capturing();
    sync_mouse_cursor(want_text);
    if (want_text != text_input) {
        if (want_text) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
        text_input = want_text;
    }
    static bool first = true;
    bool apply = g_want_window_apply.exchange(false) ||
                 (first && !getenv("ODYSSEY_WINDOW") && !getenv("ODYSSEY_HIDDEN"));
    first = false;
    if (!apply) return;

    int mode = g_settings.window_mode;
    int height = settings_display_height(g_settings.display_resolution);
    SDL_DisplayID display = SDL_GetDisplayForWindow(w);
    const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display);
    bool applied;
    if (mode == WINDOWED) {
        applied = SDL_SetWindowFullscreen(w, false) && SDL_SetWindowFullscreenMode(w, NULL);
        if (applied && height) applied = SDL_SetWindowSize(w, height * 16 / 9, height);
    } else {
        g_last_fullscreen.store(mode);
        SDL_DisplayMode selected;
        const SDL_DisplayMode *exclusive = NULL;
        if (mode == EXCLUSIVE_FULLSCREEN && desktop) {
            int width = height ? height * 16 / 9 : desktop->w;
            int lines = height ? height : desktop->h;
            if (SDL_GetClosestFullscreenDisplayMode(display, width, lines, 0, true, &selected)) exclusive = &selected;
        }
        applied = (mode != EXCLUSIVE_FULLSCREEN || exclusive) &&
                  SDL_SetWindowFullscreenMode(w, exclusive) && SDL_SetWindowFullscreen(w, true);
        if (!applied && mode == EXCLUSIVE_FULLSCREEN) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Exclusive fullscreen unavailable: %s. Using borderless.", SDL_GetError());
            applied = SDL_SetWindowFullscreenMode(w, NULL) && SDL_SetWindowFullscreen(w, true);
            if (applied) {
                g_last_fullscreen.store(BORDERLESS);
                g_settings.window_mode = BORDERLESS;
                settings_save();
            }
        }
    }
    if (!applied) SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Could not apply display settings: %s", SDL_GetError());
}

// ------------------------------------------------------------------ render thread
static bool g_inited;
static ImGuiContext *g_ctx;  // ours (another UI would have its own context)
static ImFont *g_font, *g_font_big;
static ImFont *g_menu_font, *g_menu_font_big;  // the menu window (Roboto Medium); everything else keeps g_font
static int g_tab, g_tab_request = -2;  // (-2: no request; -1 is the Prelaunch tab)
static int g_focus_tab = -99;  // put the nav cursor on this tab next frame (-100: the selected one; -99: none)
static int g_focus_frames;

// The menu opening: the controller's cursor starts on the first tab the first time in a session,
// later on the tab that's selected
void menu_tabs_focus(bool first_tab) { g_focus_tab = first_tab ? -1 : -100, g_focus_frames = 0; }
int g_tab_request_ext = -1;

// The look (theme.h): deep sky blue, white text, pipe green as the accent, gold for changed values; rounded corners as
// the installer's card; tabs are plain words with a green line under the selected one (tabs_body draws it).
static void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 14; s.FrameRounding = 7; s.GrabRounding = 7; s.TabRounding = 0; s.ChildRounding = 8;
    s.PopupRounding = 10; s.ScrollbarRounding = 7;
    s.WindowPadding = ImVec2(32, 26); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 14; s.TabBarBorderSize = 0;
    ImVec4 *c = s.Colors, none(0, 0, 0, 0);
    c[ImGuiCol_WindowBg] = kPanelV;
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = ImVec4(kPanelV.x, kPanelV.y, kPanelV.z, 0.99f);
    c[ImGuiCol_Border] = kLineV;
    c[ImGuiCol_Text] = kTextV;
    c[ImGuiCol_TextDisabled] = kTextDimV;
    c[ImGuiCol_FrameBg] = c[ImGuiCol_Button] = c[ImGuiCol_Header] = c[ImGuiCol_TableHeaderBg] = kSurfaceV;
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_ButtonHovered] = c[ImGuiCol_HeaderHovered] = kSurfaceHoverV;
    c[ImGuiCol_FrameBgActive] = c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderActive] = kAccentDeepV;
    c[ImGuiCol_Tab] = c[ImGuiCol_TabSelected] = c[ImGuiCol_TabSelectedOverline] = none;
    c[ImGuiCol_TabDimmed] = c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabDimmedSelectedOverline] = none;
    c[ImGuiCol_TabHovered] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrabActive] = c[ImGuiCol_NavCursor] = c[ImGuiCol_ScrollbarGrabActive] = kAccentV;
    c[ImGuiCol_SliderGrab] = ImVec4(kAccentV.x, kAccentV.y, kAccentV.z, 0.75f);
    c[ImGuiCol_Separator] = kLineV;
    c[ImGuiCol_TableRowBg] = none;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.14f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.22f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);
}


#ifdef __APPLE__
static void init(id<MTLDevice> dev) {
#else
static void init(void) {
#endif
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.KeyRepeatDelay = 0.45f;
    io.KeyRepeatRate = 0.10f;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    const char *paths[] = {"/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                           "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"};
    for (const char *p : paths) {
        if (FILE *f = fopen(p, "rb")) fclose(f);  // a missing file is an assertion in ImGui, not a NULL return
        else continue;
        if (!g_font) g_font = io.Fonts->AddFontFromFileTTF(p, 34, &cfg);
        if (g_font && !g_font_big) { ImFontConfig b = cfg; b.FontNo = 0; g_font_big = io.Fonts->AddFontFromFileTTF(p, 56, &b); }
    }
    // the menu window itself: Roboto Medium (third_party/imgui/misc/fonts, Apache 2.0); the FPS counter, overlays and
    // on-screen keyboard keep the fonts above
    char menu_font[1024];
    if (const char *f = getenv("ODYSSEY_MENU_FONT")) strlcpy(menu_font, f, sizeof menu_font);  // (test) another face
    if (getenv("ODYSSEY_MENU_FONT") ||
        rt_resource("Roboto-Medium.ttf", "third_party/imgui/misc/fonts/Roboto-Medium.ttf", menu_font, sizeof menu_font)) {
        g_menu_font = io.Fonts->AddFontFromFileTTF(menu_font, 36, &cfg);
        g_menu_font_big = io.Fonts->AddFontFromFileTTF(menu_font, 58, &cfg);
    }
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    if (!g_font_big) g_font_big = g_font;
    if (!g_menu_font || !g_menu_font_big) g_menu_font = g_font, g_menu_font_big = g_font_big;
    style_init();
#ifdef __APPLE__
    ImGui_ImplMetal_Init(dev);
#else
    vk_imgui_backend_init();
#endif
    g_inited = true;
}

static void drain_events(ImGuiIO &io) {
    pthread_mutex_lock(&q_lock);
    std::vector<QEvent> ev;
    ev.swap(q_events);
    pthread_mutex_unlock(&q_lock);
    for (const QEvent &q : ev) {
        switch (q.kind) {
        case QEvent::KEY: io.AddKeyEvent(q.key, q.down); break;
        case QEvent::ANALOG: io.AddKeyAnalogEvent(q.key, q.value > 0.1f, q.value); break;
        case QEvent::MOUSE_POS: io.AddMousePosEvent(q.x, q.y); break;
        case QEvent::MOUSE_BUTTON: io.AddMouseButtonEvent(q.button, q.down); break;
        case QEvent::WHEEL: io.AddMouseWheelEvent(q.x, q.y); break;
        case QEvent::TEXT: io.AddInputCharactersUTF8(q.text); break;
        case QEvent::RESET: io.ClearEventsQueue(); io.ClearInputKeys(); io.ClearInputMouse(); break;
        }
    }
}

// Section header: in the accent, capitals, as the option rows' sections (option_rows.mm opt_section).
static void header(const char *t) {
    char up[96];
    int i = 0;
    for (; t[i] && i < (int)sizeof up - 1; i++) up[i] = (char)toupper((unsigned char)t[i]);
    up[i] = 0;
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextColored(kAccentV, "%s", up);
    ImGui::Separator();
    ImGui::Spacing();
}

static void hint(const char *t) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", t);
    ImGui::PopTextWrapPos();
}

// A row with a label on the left and a choice of options on the right (dpad left/right cycles).
// reserve: room kept free right of the combo (a button on the same line)
static bool choice(const char *label, int *value, const char *const *names, int n, float reserve = 0) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
    bool changed = false;
    ImGui::SetNextItemWidth(reserve > 0 ? -reserve : -1);
    if (ImGui::BeginCombo("##c", names[*value])) {
        for (int i = 0; i < n; i++) {
            if (ImGui::Selectable(names[i], i == *value)) { *value = i; changed = true; }
            if (i == *value) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemFocused() && !ImGui::IsPopupOpen("##c")) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            *value = (*value + 1) % n; changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            *value = (*value + n - 1) % n; changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

static bool toggle(const char *label, bool *v) {
    const char *names[] = {"Off", "On"};
    int i = *v;
    bool ch = choice(label, &i, names, 2);
    *v = i;
    return ch;
}

extern "C" void renderer_get_scale(float *sx, float *sy);
extern "C" int input_pad_family(void);  // runtime/hle/input.c
extern "C" double output_scale_dynamic_factor(void);
extern "C" bool vk_prepare_requested(void);       // runtime/nvn/vulkan/device.c (the menu's Compile shaders)
extern "C" void vk_prepare_request(bool on);
#ifdef _WIN32
extern "C" bool vk_dlss_enabled(void);
#endif

static void tab_display(void) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##video", scale);
    opt_section("Display");
    static const char *window_modes[] = {"Windowed", "Borderless", "Exclusive fullscreen"};
    if (opt_choice("Display mode", &g_settings.window_mode, window_modes, WINDOW_MODE_COUNT, WINDOWED,
                   "Borderless uses the desktop's resolution. Exclusive fullscreen requests a monitor display mode. "
                   "F11 and Alt+Enter toggle between the window and your last fullscreen mode."))
        g_want_window_apply.store(true);
    static const char *display_modes[] = {"Auto", "1080p", "1440p", "4K"};
    bool borderless = g_settings.window_mode == BORDERLESS;
    if (borderless) opt_disable_next();
    if (opt_choice("Display resolution", &g_settings.display_resolution, display_modes, DISPLAY_MODE_COUNT, DISPLAY_AUTO,
                   "Changes the window size or selects the closest exclusive fullscreen mode your monitor supports. "
                   "Auto keeps the current window size or uses the desktop resolution in exclusive fullscreen. "
                   "Borderless always uses the desktop resolution."))
        g_want_window_apply.store(true);
#ifndef __APPLE__  // (Vulkan: runtime/hle/odyssey_wide.c)
    opt_section("Screen shape");
    {
        static const char *asp[] = {"16:9", "Fill window"};
        opt_choice("Aspect ratio", &g_settings.aspect, asp, 2, ASPECT_16_9,
                   "For windows wider than 16:9 (ultrawide monitors). 16:9: the Switch's picture, with bars at the sides. "
                   "Fill window: the game shows more of the world at the sides; the HUD keeps its shape in the middle.");
    }
#endif
    opt_end();
}

// Graphics: how sharp and clean the 3D image is (resolution, upscaling, anti-aliasing, DLSS, textures)
static void tab_graphics(void) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##graphics", scale);
    opt_section("Resolution");
    // display order -> RES_* (the ini / enum order predates the menu order)
    // (up to 4x: 7680 x 4320, 8K - more only ran out of GPU memory, and sat next to 75% in the list)
    // (Match display in the middle: left from it 75% / 50% / 25%, right Dynamic then 1x-4x; user 2026-10-11)
    static const char *res[] = {"25%", "50%", "75%", "Match display", "Dynamic", "1080p (1x)", "1440p", "4K (2x)", "3x", "4x"};
    static const int res_mode[] = {RES_Q25, RES_Q50, RES_Q75, RES_MATCH, RES_DYNAMIC, 1, RES_1440, 2, 3, 4};
    enum { RES_MATCH_AT = 3 };
    enum { NRES = sizeof res_mode / sizeof res_mode[0] };
    {
        int ri = RES_MATCH_AT;
        for (int i = 0; i < NRES; i++) if (res_mode[i] == g_settings.res_mode) ri = i;
        float sx, sy;
        renderer_get_scale(&sx, &sy);
        char d[400];
        int n = snprintf(d, sizeof d, "How sharp the 3D image is. Match display: your screen's own resolution. Dynamic: "
                                      "as sharp as your GPU keeps at full speed. 1x-4x: 1080p times that (2x: 4K; the "
                                      "Switch renders 1600 x 900). 75%% / 50%% / 25%%: a share of the display, for "
                                      "slower machines.\n\nRendering at %.0f x %.0f.", 1600.0 * sx, 900.0 * sy);
        if (g_settings.res_mode == RES_DYNAMIC)
            snprintf(d + n, sizeof d - n, " Dynamic: %.0f%% of the display right now.", output_scale_dynamic_factor() * 100);
#ifdef _WIN32
        if (vk_dlss_enabled()) {
            snprintf(d, sizeof d, "Match display lets DLSS choose its usual input size. A fixed resolution overrides that "
                     "size. At or above the display resolution, DLSS upscaling is bypassed. DLAA applies at equal sizes.\n\n"
                     "Rendering at %.0f x %.0f.", 1600.0 * sx, 900.0 * sy);
        }
#endif
        if (opt_choice("Internal resolution", &ri, res, NRES, RES_MATCH_AT, d, false)) g_settings.res_mode = res_mode[ri];
    }
    {   // each OS's own upscaler next to bilinear (UPSCALE_SHARP: MetalFX on macOS, AMD FSR 1 on Windows)
#ifdef __APPLE__
        static const char *ups[] = {"Bilinear", "MetalFX"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. MetalFX: Apple's "
                         "upscaler, sharper. Bilinear: plain and soft.";
#else
        static const char *ups[] = {"Bilinear", "AMD FSR 1"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. AMD FSR 1: "
                         "edge-aware upscaling and sharpening, sharper. Bilinear: plain and soft.";
#endif
        int ui = g_settings.upscaler == UPSCALE_BILINEAR ? 0 : 1;
        char udm[512];
        if (g_settings.res_mode == RES_MATCH
#ifdef _WIN32
            && !vk_dlss_enabled()
#endif
        ) {  // (renders at the window's size: nothing to scale up)
            snprintf(udm, sizeof udm, "%s\n\nNot used at Match display: the game already renders at your display's resolution.", ud);
            ud = udm;
            opt_disable_next();
        }
#ifdef __APPLE__
        constexpr int default_upscaler = 1;
#else
        constexpr int default_upscaler = 0;
#endif
        if (opt_choice("Upscaler", &ui, ups, 2, default_upscaler, ud)) g_settings.upscaler = ui ? UPSCALE_SHARP : UPSCALE_BILINEAR;
    }
    opt_section("Anti-aliasing");
    static const char *aa[] = {"Off", "SMAA"};
    opt_choice("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,
               "Smooths jagged edges of the game image before it is scaled to the window. SMAA: finds the edges and "
               "blends only along them, so textures stay sharp. Most useful at a lower internal resolution.");
#ifdef _WIN32
    opt_section("NVIDIA DLSS");
    {
        static const char *modes[] = {"Off", "DLAA", "Quality", "Balanced", "Performance"};
        static const char *presets[] = {"Default", "K", "L", "M"};
        opt_choice("DLSS mode", &g_settings.dlss_mode, modes, DLSS_MODE_COUNT, DLSS_OFF,
                   "DLAA anti-aliases at display resolution. Quality, Balanced and Performance render at progressively "
                   "lower resolutions. Off takes effect immediately. Restart after selecting another mode.");
        opt_choice("DLSS preset", &g_settings.dlss_preset, presets, DLSS_PRESET_COUNT, DLSS_PRESET_DEFAULT,
                   "Model hint for NVIDIA DLSS. Default lets the installed model choose. Applies on the next launch.");
        opt_toggle("CAS sharpening", &g_settings.dlss_cas, false,
                   "Applies AMD FidelityFX Contrast Adaptive Sharpening after DLSS or DLAA. Changes at once.");
        if (!g_settings.dlss_cas) opt_disable_next();
        float sharpness = (float)g_settings.dlss_sharpness;
        if (opt_slider("CAS strength", &sharpness, 10, 100, 10, 20, "%g%%",
                       "Adjusts the amount of sharpening. Changes at once."))
            g_settings.dlss_sharpness = (int)sharpness;
    }
#endif
#ifndef __APPLE__  // (Vulkan: nvn/vulkan/textures.c vk_sampler, "Banding")
    opt_section("Image");
    {
        static const char *an[] = {"Game", "2x", "4x", "8x", "16x"};
        static const int an_v[] = {ANISO_GAME, 2, 4, 8, 16};
        int ai = 0;
        for (int i = 0; i < 5; i++) if (an_v[i] == g_settings.aniso) ai = i;
        char ad[400];
        snprintf(ad, sizeof ad, "Keeps ground, roads and walls seen at a shallow angle sharp into the distance. Only surface "
                                "textures are raised; shadows and screen effects keep the game's setting. Game: the level "
                                "the game asks for, as on the Switch.%s",
                 settings_aniso_at_start() != g_settings.aniso ? "\n\nApplies when the game starts again." : "");
        if (opt_choice("Anisotropic filtering", &ai, an, 5, 4, ad, false)) g_settings.aniso = an_v[ai];
    }
    opt_toggle("Smooth gradients", &g_settings.deband, true,
               "Removes the visible color steps (banding) in skies, fog and dark areas: the game's image keeps more "
               "precision until it reaches the screen and gets a fine dither there. Off: as on the Switch.");
#endif
    opt_end();
}

// Performance: frame rate, the overlays, and the shaders (stutter)
static void tab_performance(void) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##performance", scale);
#ifndef __APPLE__  // (Vulkan: runtime/hle/odyssey_120.c, nvn/interp.c)
    opt_section("Frame rate");
    {
        static const char *fps[] = {"60 FPS", "120 FPS (Experimental)"};
        opt_choice("Frame rate", &g_settings.frame_rate, fps, 2, FPS_60,
                   "Experimental 120 FPS draws up to 120 frames per second while gameplay runs at its original 60 Hz. "
                   "Use a 120 Hz or faster display. 60 FPS matches the Switch. Changes at once.", false);
    }
    if (g_settings.frame_rate == FPS_120) opt_disable_next();
#ifdef _WIN32
    if (vk_dlss_enabled()) opt_disable_next();
#endif
    opt_toggle("120 FPS interpolation", &g_settings.frame_interp, false,
               "Available with Frame rate set to 60 FPS and DLSS off. Adds an image between game frames, targeting 120 FPS "
               "on a 120 Hz or faster display. Busy scenes can temporarily fall back to 60 FPS.");
#endif
    opt_section("Overlay");
    opt_toggle("Show FPS", &g_settings.show_fps, false, "Shows the frame rate in the top right corner.");
    opt_toggle("Advanced performance overlay", &g_settings.performance_overlay, false,
               "Shows FPS, frame time, and available hardware information in the top right.");
#ifndef __APPLE__  // (Vulkan: nvn/vulkan/device.c vk_prepare_before_start)
    opt_section("Shaders");
    opt_toggle("Preload area shaders", &g_settings.shader_preload, true,
               "Warms shader libraries during area loading to reduce stalls and objects popping in. Uses more memory. "
               "Applies to areas loaded from now on.");
    {
        static const char *wn[] = {"Skip", "Wait"};
        int wi = g_settings.shader_wait;
        if (opt_choice("While a shader loads", &wi, wn, 2, 0,
                       "What happens when something is drawn before its shader is ready. Skip: the game keeps running; "
                       "the object appears a moment later (or the picture is held for a frame). Wait: nothing is ever "
                       "missing, but the game pauses for the load (a short hitch).", false)) g_settings.shader_wait = wi != 0;
    }
    {
        static const char *wn[WARMUP_COUNT] = {"Auto", "Normal", "Long"};
        opt_choice("Warm-up at start", &g_settings.warmup, wn, WARMUP_COUNT, WARMUP_AUTO,
                   "How long the game spends at start getting the pipelines it has seen ready, so effects don't stutter "
                   "the first time they appear. Normal: up to 15 s and 3 GB of memory. Long: up to 30 s and 12 GB "
                   "(at most half of this PC's memory). Auto: Long with 16 GB of memory or more. Applies at the next "
                   "start.", false);
    }
    {
        bool on = vk_prepare_requested();
        if (opt_action("Compile shaders", on ? "At next start" : "", on
                       ? "The shader preparation pass will run the next time the game starts (a window shows the "
                         "progress; a few minutes). Quit and start the game again to do it now. Select again to cancel."
                       : "Compiles the game's shader archives and pipeline states recorded during play at the next "
                         "start. A progress window shows both passes. Use after a driver update or after visiting new areas."))
            vk_prepare_request(!on);
    }
#endif
    opt_end();
}

static void tab_audio(void) {
    opt_begin("##audio", ImGui::GetFontSize() / 34);
    opt_section("Sound");
    float pct = roundf(g_settings.volume * 100);
    if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                   "The game's volume. 100% is the game's own level; above it, louder."))
        g_settings.volume = pct / 100.0f;
    opt_toggle("Reverb", &g_settings.reverb, true,
               "The game's echo effect in caves and big rooms.");
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the game's videos (e.g. the control tips). Applies the next time a video starts.");
    opt_toggle("Mute in background", &g_settings.mute_unfocused, false,
               "No sound while the game's window isn't in front (another window is active). The sound comes back as "
               "soon as you switch back.");
    opt_end();
}

// Mods (runtime/mods.h): the folders in mods/ with game files, each on or off (applies the next time the game starts).
static void tab_mods(float scale) {
    const char *how = "Put each mod in its own folder in here, with its romfs folder inside - as mods for the Switch "
                      "emulators come: mods/<Mod name>/romfs/...\n\nMods made for version 1.0.0 of the game work best. "
                      "If two mods change the same file, the one higher in the list wins.\n\nCode mods (exefs: IPS / "
                      "pchtxt patches, Super Mario Odyssey: Online) can't be used: the game code is recompiled from the "
                      "original game on your PC.";
    opt_begin("##mods", scale);
    int n = mods_count();
    opt_section(mods_changed() ? "Mods - restart the game to apply" : "Mods");
    if (!n) {
        opt_disable_next();
        opt_action("No mods found", "", how);
    }
    for (int i = 0; i < n; i++) {
        const ModInfo *m = mods_get(i);
        char d[1600];
        int k = snprintf(d, sizeof d, "mods/%s\n\n", m->id);
        if (!m->romfs) {
            snprintf(d + k, sizeof d - k, "A code mod: it changes the game code, which can't be used here - the game "
                                          "code is recompiled from the original game on your PC.");
        } else {
            k += snprintf(d + k, sizeof d - k, "%d file%s", m->files, m->files == 1 ? "" : "s");
            if (m->applied)
                k += snprintf(d + k, sizeof d - k, ": %d replace%s the game's, %d %s new.", m->replaced,
                              m->replaced == 1 ? "s" : "", m->added, m->added == 1 ? "is" : "are");
            else
                k += snprintf(d + k, sizeof d - k, ".");
            if (m->applied && m->shadowed)
                k += snprintf(d + k, sizeof d - k, " %d %s also in a mod higher in this list, which wins.", m->shadowed,
                              m->shadowed == 1 ? "is" : "are");
            if (m->applied && m->skipped)
                k += snprintf(d + k, sizeof d - k, " %d %s in %s the game doesn't have, left out.", m->skipped,
                              m->skipped == 1 ? "is" : "are", m->skipped == 1 ? "a folder" : "folders");
            if (m->code) k += snprintf(d + k, sizeof d - k, " Its code patches (exefs) can't be used here.");
            snprintf(d + k, sizeof d - k, "\n\nSwitching it on or off applies the next time the game starts.");
        }
        bool on = m->romfs && m->on;
        ImGui::PushID(i);  // (two mods may have the same name)
        if (!m->romfs) opt_disable_next();
        if (opt_toggle(m->name, &on, true, d)) mods_set_on(i, on);
        ImGui::PopID();
    }
    opt_section("Folder");
    char url[1600];
    if (opt_action("Open mods folder", "", how) && mods_dir_url(url, sizeof url)) SDL_OpenURL(url);
    opt_end();
}

static void tab_updates(float scale) {
    UpdStatus s;
    updater_status(&s);
    opt_begin("##updates", scale);
    char value[96];
    opt_section("Updates");
    snprintf(value, sizeof value, "%s", s.current);
    opt_disable_next();
    opt_action("This version", value, "The version of " RT_APP_NAME " you're running.");
    const char *what = "";
    char desc[640];
    snprintf(desc, sizeof desc, "Looks for a newer release on the project's GitHub page.");
    switch (s.state) {
    case UPD_UNCONFIGURED: what = "Not set up"; snprintf(desc, sizeof desc, "This build has no update source."); break;
    case UPD_IDLE: what = "Check now"; break;
    case UPD_CHECKING: what = "Checking..."; break;
    case UPD_LATEST: what = "Up to date"; break;
    case UPD_AVAILABLE: snprintf(value, sizeof value, "%s available", s.latest); what = value; break;
    case UPD_DOWNLOADING: snprintf(value, sizeof value, "Downloading %.0f%%", s.progress * 100); what = value; break;
    case UPD_READY: snprintf(value, sizeof value, "%s installed", s.latest); what = value; break;
    case UPD_FAILED: what = "Didn't work"; snprintf(desc, sizeof desc, "%s", s.error); break;
    }
    if (s.state == UPD_UNCONFIGURED || s.state == UPD_CHECKING || s.state == UPD_DOWNLOADING) opt_disable_next();
    if (opt_action("Check for updates", what, desc) && (s.state == UPD_IDLE || s.state == UPD_LATEST || s.state == UPD_FAILED))
        updater_check();
    if (s.can_install && (s.state == UPD_AVAILABLE || s.state == UPD_FAILED)) {
        snprintf(desc, sizeof desc, "Downloads %s from GitHub and puts it over this install's program files. Your game "
                                    "files, saves, settings and mods stay. After the restart the game code is rebuilt "
                                    "for the new version if it needs to be (a few minutes, once).", s.latest);
        if (opt_action("Download and install", "", desc)) updater_install();
    }
    if (s.state == UPD_READY && opt_action("Restart now", "", "Starts the new version. Progress since your last save is lost."))
        updater_restart();
    if (s.page[0] && opt_action("What's new", "", "Opens the release's page on GitHub in your browser.")) SDL_OpenURL(s.page);
    if (s.state != UPD_UNCONFIGURED) {
        char repo[600];
        snprintf(repo, sizeof repo, "https://github.com/%s", updater_repo());
        if (opt_action("Project page", "", "Opens the project's GitHub page in your browser.")) SDL_OpenURL(repo);
        if (opt_toggle("Check at start", &g_settings.check_updates, true,
                       "Looks for a new release each time the game starts (in the background; only GitHub is contacted, "
                       "and nothing about you or your game is sent)."))
            settings_save();
    }
    opt_end();
}

static void tab_quit(float scale) {
    opt_begin("##quit", scale);
    opt_section("Quit");
    if (opt_action("Quit to Desktop", "", "Closes " RT_APP_NAME " after confirmation.")) g_quit_prompt = true;
    opt_end();
}

static void quit_dialog(float scale) {
    if (!g_quit_prompt.load()) return;
    if (!ImGui::IsPopupOpen("Quit game?")) ImGui::OpenPopup("Quit game?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Quit game?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Progress since your last save will be lost.");
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(136 * scale, 0)) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            g_quit_prompt = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Quit to Desktop", ImVec2(242 * scale, 0)) && g_quit_prompt.load()) {
            settings_save();
            SDL_Event q = {};
            q.type = SDL_EVENT_QUIT;
            if (SDL_PushEvent(&q)) {
                g_quit_prompt = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

static void tab_about(void) {
    header("System language");
    {
        // what the game is told the console's language is: its text follows this (runtime/settings.c)
        char autol[64];
        snprintf(autol, sizeof autol, "Auto (%s)", settings_language_name());
        const char *names[N_LANGUAGES];
        names[0] = autol;
        for (int i = 1; i < N_LANGUAGES; i++) names[i] = g_language_names[i];
        if (choice("Language", &g_settings.language, names, N_LANGUAGES)) settings_save();
    }
    hint("The game's text language, as its own system setting. Applies the next time the game starts.");
    ImGui::Spacing();
    header("Controller");
    toggle("Vibration", &g_settings.rumble);
    hint("The controller's rumble, as the game sends it (the Switch's HD rumble on the controller's motors).");
    {
        static const int pct[] = {0, 5, 10, 15, 20, 25, 30};
        static const char *const names[] = {"Off", "5%", "10%", "15%", "20%", "25%", "30%"};
        int i = 0;
        for (int k = 0; k < 7; k++) if (pct[k] <= g_settings.stick_deadzone) i = k;
        if (choice("Stick deadzone", &i, names, 7)) g_settings.stick_deadzone = pct[i];
    }
    hint("How far a controller stick moves before it counts. Raise it if Mario or the camera drifts on their own.");
    ImGui::Spacing();
    header("Mouse");
    toggle("Mouse camera", &g_settings.mouse_camera);
    hint("Moving the mouse turns the camera. While you play, the pointer stays in the window; it is free again in this "
         "menu (Esc) or when you switch to another window.");
    {
        static const int pct[] = {25, 50, 75, 100, 150, 200, 300, 400};
        static const char *const names[] = {"25%", "50%", "75%", "100%", "150%", "200%", "300%", "400%"};
        int i = 0;
        for (int k = 0; k < 8; k++) if (pct[k] <= g_settings.mouse_sensitivity) i = k;
        if (choice("Mouse sensitivity", &i, names, 8)) g_settings.mouse_sensitivity = pct[i];
    }
    toggle("Invert mouse up / down", &g_settings.mouse_invert_y);
    hint("Moving the mouse up turns the camera down instead.");
    {
        static const char *const names[PROMPTS_COUNT] = {"Auto", "Controller", "Keyboard and mouse"};
        int i = g_settings.prompts >= 0 && g_settings.prompts < PROMPTS_COUNT ? g_settings.prompts : PROMPTS_AUTO;
        if (choice("Button prompts", &i, names, PROMPTS_COUNT)) g_settings.prompts = i;
    }
    hint("The game's button icons: Auto shows the keys you bound (and the mouse for the camera) after you use the "
         "keyboard or mouse, and the controller's buttons after you use a controller.");
    toggle("Skip controller tips", &g_settings.skip_intro);
    hint("Leaves out the Joy-Con instructions screen's wait before the title screen. Applies at the next start.");
    ImGui::Spacing();
    header("Controls");
    controls_bindings_draw(ImGui::GetFontSize() / 34);
    ImGui::Spacing();
    header("Menu");
    ImGui::TextUnformatted("Open / close this menu:  F1, Esc, the controller's Home / Xbox / PlayStation button, or Back + Start");
    ImGui::TextUnformatted("Switch tabs:  L / R  (Q / E)        Back:  B  (Esc)");
    ImGui::TextUnformatted("Fullscreen:  F11 / Alt+Enter");
}

// Reboot (runtime/main.c): the game is gone - menu and name dialog close; the ImGui context stays.
RT_ON_RESET(menu_reset) {
    set_open(false);
    g_quit_prompt = false;
    g_kbd = 0;
    osk_cancel();
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    pthread_mutex_unlock(&q_lock);
}

static void keyboard_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_font_big);
    ImGui::TextUnformatted("Enter your name");
    ImGui::PopFont();
    static bool was_open, was_pad;
    bool pad = g_last_pad.load(), appearing = !was_open || pad != was_pad;  // (switching device: focus the new one)
    was_open = true, was_pad = pad;
    if (!pad) {  // keyboard: type straight into the field
        if (appearing) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushFont(g_font_big);
        ImGui::InputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                         ImGuiInputTextFlags_CallbackCharFilter,
                         [](ImGuiInputTextCallbackData *d) -> int { return d->EventChar < 0x20 ? 1 : 0; });
        ImGui::PopFont();
        if ((int)strlen(g_kbd_text) > g_kbd_max) g_kbd_text[g_kbd_max] = 0;
        ImGui::TextDisabled("Enter  confirm      Esc  cancel      (%d / %d)", (int)strlen(g_kbd_text), g_kbd_max);
    } else {  // controller: on-screen keyboard (osk.mm, shared with our menus' text fields)
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big);
        if (r) g_kbd = r > 0 ? 2 : 3;  // (Start / + confirms in menu_handle_event too)
    }
    ImGui::End();
    if (g_kbd.load() != 1) was_open = false;
}

static void notice_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 140));
    float w = fminf(io.DisplaySize.x * 0.9f, 1000 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##note", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_menu_font_big);
    ImGui::TextUnformatted(g_note_title);
    ImGui::PopFont();
    ImGui::PushFont(g_menu_font);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_note_text);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    static bool was_open;
    if (!was_open) ImGui::SetKeyboardFocusHere(), ImGui::SetNavCursorVisible(true);  // (the controller's cursor on OK)
    was_open = true;
    // OK: clicked, A on it, or any face button / + (only one choice: whichever the player's layout calls A)
    if (ImGui::Button("OK", ImVec2(200 * scale, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))
        g_note = 2;
    ImGui::PopFont();
    ImGui::End();
    if (g_note.load() != 1) was_open = false;
}

// Game thread (runtime/hle/applets.c): the notice over the last frame until the player closes it.
extern "C" void menu_notice_run(const char *title, const char *text) {
    extern void backend_represent(void);
    snprintf(g_note_title, sizeof g_note_title, "%s", title);
    snprintf(g_note_text, sizeof g_note_text, "%s", text);
    input_reset();
    g_note = 1;
    while (g_note.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot closes it)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    g_note = 0;
    input_reset();  // (the button that closed it doesn't reach the game)
}

// Game thread (runtime/hle/swkbd.c): show the dialog, re-present the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    extern void backend_represent(void);
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", initial ? initial : "");
    g_kbd_max = max_len > 0 && max_len < (int)sizeof g_kbd_text / 4 ? max_len : 8;
    input_reset();
    g_kbd = 1;
    // (test) ODYSSEY_KBD_AUTO=<text>: answer at once
    if (getenv("ODYSSEY_KBD_AUTO")) { snprintf(g_kbd_text, sizeof g_kbd_text, "%s", getenv("ODYSSEY_KBD_AUTO")); g_kbd = 2; }
    while (g_kbd.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    bool ok = g_kbd.load() == 2 && g_kbd_text[0];
    if (ok) snprintf(out, cap, "%s", g_kbd_text);
    g_kbd = 0;
    input_reset();
    return ok;
}

// Draw the settings tabs in the current window, with a Prelaunch tab when the launcher supplies one.
static void tabs_body(float scale, void (*prelaunch)(float)) {
    // (id: the page; -1 = Prelaunch)
    struct Tab { const char *name; int id; bool on; };
    Tab tabs[11];
    int ntabs = 0;
    if (prelaunch) tabs[ntabs++] = {"Prelaunch", -1, true};
    // (ids: the pages below; 5-7 were Link's Awakening's and stay unused, so ODYSSEY_MENU_TAB numbers keep meaning;
    // the old Video page is Display (0), Graphics (3) and Performance (4) now)
    static const char *names[] = {"Display", "Audio", "Mods", "Graphics", "Performance", NULL, NULL, NULL, "Controls", "Quit",
                                  "Updates"};
    static const int order[] = {0, 3, 4, 1, 8, 2, 10, 9};  // Display, Graphics, Performance, Audio, Controls, Mods, Updates, Quit
    for (int i : order)
        if (i != 9 || !prelaunch) tabs[ntabs++] = {names[i], i, true};
    int cur = 0;  // g_tab / g_tab_request hold ids
    for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab) cur = i;
    if (!tabs[cur].on) cur = 0;
    // L/R (or Q/E) switch tabs, skipping greyed-out ones (not while typing: Q / E are letters then, and the on-screen
    // keyboard has the controller)
    if (g_tab_request_ext >= 0) { g_tab_request = g_tab_request_ext; g_tab_request_ext = -1; }
    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
    static int focused_tab = -99;  // the tab the controller's cursor is on (from the frame before; -99: none)
    if (!any_popup && !typing) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        if (step) {
            int i = cur;
            do i = (i + step + ntabs) % ntabs; while (!tabs[i].on);
            g_tab_request = tabs[i].id;
            // the cursor on the tab bar goes along (left behind, A on it switched straight back; user 2026-10-11)
            if (focused_tab != -99) g_focus_tab = tabs[i].id, g_focus_frames = 0;
        }
    }
    // Down with the cursor on the tab bar: the page's first row (option rows; other pages keep ImGui's own pick)
    bool down_from_tab = focused_tab != -99 && ImGui::GetCurrentContext()->NavCursorVisible && !any_popup &&
                         (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_DownArrow, false));
    opt_focus_first(down_from_tab);
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < ntabs; k++) {
            const Tab &t = tabs[k];
            ImGuiTabItemFlags f = g_tab_request == t.id && t.on ? ImGuiTabItemFlags_SetSelected : 0;
            char lbl[64];
            snprintf(lbl, sizeof lbl, "  %s  ", t.name);
            ImGui::BeginDisabled(!t.on);
            // The tabs are their names (the shown one white, the others dim) with a red line under the shown one.
            // Focus (controller / keyboard): the tab lights up a little and gets a dim line, instead of ImGui's
            // outline (it sits 4 px outside the tab and the tab bar's clipping could cut its top off on macOS)
            // (focused_tab: from the frame before - colours go in before the tab is drawn)
            bool lit = focused_tab == t.id && ImGui::GetCurrentContext()->NavCursorVisible;
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, k == cur || lit ? kTextV : kTextDimV);
            int pushed = 2;
            if (lit) {
                for (ImGuiCol c : {ImGuiCol_Tab, ImGuiCol_TabHovered, ImGuiCol_TabSelected}) ImGui::PushStyleColor(c, ImVec4(1, 1, 1, 0.07f));
                pushed += 3;
            }
            if (g_focus_tab == -1 && !prelaunch) g_focus_tab = 0;  // (no Prelaunch in game: Display, the first tab)
            bool focus_here = g_focus_tab == t.id || (g_focus_tab == -100 && t.id == g_tab);
            bool open = ImGui::BeginTabItem(lbl, NULL, f);
            ImGui::PopStyleColor(pushed);
            if (focus_here) {  // the controller's cursor starts on this tab (SetKeyboardFocusHere skips tabs: no tab stop)
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                // a few frames: the page's child window appearing makes ImGui pick its first widget once it's laid out
                if (++g_focus_frames >= 4) g_focus_tab = -99, g_focus_frames = 0;
                else g_focus_tab = t.id;
            }
            if (open || lit) {
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                float h = 3 * scale > 2 ? 3 * scale : 2, m = 10 * scale;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + m, b.y - h), ImVec2(b.x - m, b.y), open ? kAccent : kAccentDim);
            }
            if (ImGui::IsItemFocused()) focused_tab = t.id;
            else if (focused_tab == t.id) focused_tab = -99;
            ImGui::EndDisabled();
            if (open) {
                g_tab = t.id;
                ImGui::Spacing();
                ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None,  // (the rest of the window: no hint line)
                                  ImGuiWindowFlags_NavFlattened);
                // Down from the tab bar, pages that aren't option rows (Controls): their first control
                // (option-row pages: opt_focus_first)
                if (down_from_tab && (t.id == -1 || t.id == 8)) {
                    opt_focus_first(false);
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                switch (t.id) {
                case -1: prelaunch(scale); break;
                case 0: tab_display(); break;
                case 3: tab_graphics(); break;
                case 4: tab_performance(); break;
                case 1: tab_audio(); break;
                case 2: tab_mods(scale); break;
                case 8: tab_about(); break;
                case 9: tab_quit(scale); break;
                case 10: tab_updates(scale); break;
                }
                static const char *scroll_test = getenv("ODYSSEY_MENU_SCROLL");  // test hook: page scroll, 0..1
                if (scroll_test) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(scroll_test));
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        g_tab_request = -2;
        opt_focus_first(false);  // (a page without option rows: nothing took it)
        ImGui::EndTabBar();
    }
}

void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool, void (*prelaunch)(float)) {
    ImFont *f0 = g_font, *f1 = g_font_big;
    g_font = font, g_font_big = font_big;
    ImGuiStyle saved = ImGui::GetStyle();  // the menu's look (another UI may have its own style)
    style_init();
    ImGui::GetStyle().ScaleAllSizes(scale);
    tabs_body(scale, prelaunch);
    ImGui::GetStyle() = saved;
    g_font = f0, g_font_big = f1;
}

static void draw_performance(void) {
    float fps = ImGui::GetIO().Framerate;
    if (fps > 0) ImGui::Text("%.0f FPS  |  %.2f ms/frame", fps, 1000.0f / fps);
    else ImGui::TextUnformatted("Measuring frame time");
#ifndef __APPLE__
    VulkanOverlayStats stats = vk_overlay_stats();
    ImGui::Separator();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 22);
    if (stats.gpu_name && *stats.gpu_name) ImGui::TextUnformatted(stats.gpu_name);
    if (stats.gpu_usage >= 0) ImGui::Text("GPU %d%%", stats.gpu_usage);
    else ImGui::TextUnformatted("GPU usage unavailable");
    if (stats.vram_budget_mib) {
        ImGui::SameLine();
        ImGui::Text("VRAM %.1f / %.1f GiB", stats.vram_mib / 1024.0, stats.vram_budget_mib / 1024.0);
    }
    if (stats.cpu_name && *stats.cpu_name) ImGui::TextUnformatted(stats.cpu_name);
    if (stats.cpu_usage >= 0) ImGui::Text("CPU %d%%", stats.cpu_usage);
    ImGui::PopTextWrapPos();
#endif
}
#ifdef __APPLE__
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) {
    const double tw = target.width, th = target.height;
#else
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    const double tw = target_w, th = target_h;
#endif
    bool open = g_open.load(), setup = g_setup.load();
    bool kbd = g_kbd.load() == 1, note = g_note.load() == 1;
    if (!open && !kbd && !note && !setup && !menu_stats_visible()) {
        if (g_inited) { pthread_mutex_lock(&q_lock); q_events.clear(); pthread_mutex_unlock(&q_lock); }
        return;
    }
#ifdef __APPLE__
    if (!g_inited) init(dev);
#else
    if (!g_inited) init();
#endif
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)tw, (float)th);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    io.MouseDrawCursor = open && !setup && g_draw_mouse_cursor.load();
    static double last;
    double now = SDL_GetTicksNS() / 1e9;
    io.DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    drain_events(io);
    // fonts are built at 34/56 px for a ~1080 px tall output; scale to the window. The menu and overlays at 68% of
    // that (a menu over about 40% of the game's picture); the installer page at full size.
    float scale = (float)(th / 1080.0) * (setup ? 1.0f : 0.68f);
    if (scale < 0.45f) scale = 0.45f;
    io.FontGlobalScale = scale;

#ifdef __APPLE__
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
#endif
    // spacing/rounding follow the output size too (style tuned for 1080 p)
    static ImGuiStyle base;
    static float style_scale = -1;
    if (style_scale < 0) base = ImGui::GetStyle();
    if (fabsf(style_scale - scale) > 0.01f) {
        ImGui::GetStyle() = base;
        ImGui::GetStyle().ScaleAllSizes(scale);
        style_scale = scale;
    }
#ifdef __APPLE__
    ImGui_ImplMetal_NewFrame(rp);
#else
    ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();

    if (setup) {
        setup_draw(scale, g_menu_font, g_menu_font_big);
        open = false;
    } else if (menu_stats_visible() && (!open || !g_settings.performance_overlay)) {
        bool advanced = g_settings.performance_overlay;
        float margin = 16 * scale;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - margin, margin),
                                ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(advanced ? 0.8f : 0.5f);
        if (advanced) ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 3 * scale));
        ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::SetWindowFontScale(advanced ? 0.64f : 1.0f);
        if (advanced) draw_performance();
        else ImGui::Text("%.0f fps", io.Framerate);
        ImGui::End();
        if (advanced) ImGui::PopStyleVar();
    }

    if (!setup) {  // a new release found at start: a note in the corner for a few seconds (the menu's Updates page has it)
        static double shown_at = -1;
        UpdStatus us;
        updater_status(&us);
        if (us.state == UPD_AVAILABLE && shown_at < 0) shown_at = now;
        if (shown_at >= 0 && now - shown_at < 12 && !open) {
            float margin = 16 * scale;
            ImGui::SetNextWindowPos(ImVec2(margin, io.DisplaySize.y - margin), ImGuiCond_Always, ImVec2(0, 1));
            ImGui::SetNextWindowBgAlpha(0.85f);
            ImGui::Begin("##update_note", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                                    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::PushStyleColor(ImGuiCol_Text, kAccentSoftV);
            ImGui::Text("Update %s available", us.latest);
            ImGui::PopStyleColor();
            ImGui::TextUnformatted("F1 > Updates to install it");
            ImGui::End();
        }
    }

    if (open) {
        // dim the game behind the menu
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 130));
        // a window in the middle with the game around it: about 55% x 70% of the picture (40% of it)
        float w = fminf(io.DisplaySize.x * 0.55f, 1560 * scale), h = fminf(io.DisplaySize.y * 0.72f, 1100 * scale);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImFont *keep_big = g_font_big;  // (the window in the menu's font; header() uses g_font_big)
        g_font_big = g_menu_font_big;
        ImGui::PushFont(g_menu_font);
        // header: the crescent and the name, a close X in the corner (B / Esc too), a hairline under them
        ImGui::PushFont(g_font_big);
        {
            float lh = ImGui::GetFontSize(), sz = lh * 1.35f;
            ImVec2 lp = ImGui::GetCursorScreenPos();
            // (the icon's pipe, centred on the line)
            theme_logo(ImGui::GetWindowDrawList(), ImVec2(lp.x, lp.y + lh * 0.5f - sz * 0.5f), sz, kAccent, theme_u32(kPanelV, 1));
            ImGui::Dummy(ImVec2(sz, lh));
            ImGui::SameLine(0, lh * 0.4f);
            ImGui::TextUnformatted(RT_APP_NAME);
        }
        ImGui::PopFont();
        {
            float bx = 44 * scale;
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            bool close = ImGui::InvisibleButton("##close", ImVec2(bx, bx));
            ImGui::PopStyleColor();
            bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImU32 col = hot ? kAccent : theme_u32(kTextV, 0.85f);
            float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * scale * 1.5f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), theme_u32(kSurfaceHoverV), 4 * scale);
            dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
            dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
            if (close) set_open(false);
        }
        ImGui::Separator();

        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
        tabs_body(scale, NULL);
        quit_dialog(scale);
        // B with nothing focused-deeper closes the menu (not the Esc / B that ends typing: typing is from the frame
        // before, so it's still true the frame a field lets go)
        if (!any_popup && !typing && !ui_text_busy() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            set_open(false);
        ImGui::PopFont();
        g_font_big = keep_big;
        ImGui::End();
        osk_draw(g_font_big, scale);  // (a text field activated with the controller)
    }

    if (kbd) keyboard_draw(io, scale);
    if (note && !open) notice_draw(io, scale);  // (the menu opened over it: the notice waits behind it)
    ImGui::Render();
    g_text_busy = open && ui_text_busy();
#ifdef __APPLE__
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    [enc endEncoding];
#else
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);  // inside the window image's rendering
#endif
    g_drawn = true;
}
