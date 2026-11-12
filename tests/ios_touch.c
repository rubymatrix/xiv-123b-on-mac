/* Touch controls on iOS (docs/ios-port.md): a gamepad on the screen, for playing without a controller
 * or a keyboard, and Back everywhere. From xi-on-anything's host/ios_touch.c.
 *
 * The gamepad is SDL's virtual one: the game sees an Xbox 360 pad like any other (input.c, dinput.c),
 * the first pad config.pad names. Drawn by the Metal back end (gfx9_touch.h, gfx_set_touch_shapes):
 *   - the left stick floats: it starts where the thumb lands in the lower left, and moves the player
 *   - a drag anywhere else on the game turns the camera (the right stick, from where the finger landed)
 *   - the face buttons (PlayStation's shapes, as FFXI's console pads), the D-pad, L1/R1, L2/R2 (the
 *     macro palette), SELECT and START
 *   - a quick tap that is not on a control is a click where it landed, for the game's menus and targets
 * The controls are there from the start: the sign-in screen takes a pad too. Ours is the game's first
 * pad while they are (input_prefer_pad); a real controller or a hardware keyboard in use hides them (the
 * controller is then the game's), and a
 * touch on the screen brings them back.
 *
 * Back (Escape) is also iOS's back gesture, a swipe in from the left edge. Escape is let go KEY_MS
 * after it is pressed: the game reads the keyboard's state once a frame, and would miss both in one.
 *
 * While the controls are up the mouse SDL makes of touches is dropped (fingers are handled here), and
 * a pointer's (the simulator's from a Mac, an iPad trackpad's) is taken as one more finger. Finger events come on the main thread (an event watch);
 * the controls are laid out and drawn from there and from the game's frame (ios_touch_frame); g_lock
 * keeps the two apart. */
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "input.h"
#include "touch.h"

void ios_touch_init(void);
void ios_touch_frame(int in_game);
static void refresh(SDL_Window* win);
static void draw(void);

#define EDGE 0.12f  /* the back gesture: starts within this of the left edge */
#define SWIPE 0.15f /* and goes this far across */
enum
{
    SWIPE_NS = 800000000,
    TAP_NS = 250000000, /* a tap: up within this */
    TAP_MOVE = 10,      /* points, moved less than this */
    KEY_MS = 80,
    CLICK_MS = 50,
    STICK = 58,         /* the left stick's base, radius in points */
    KNOB = 24,
    LOOK = 60,          /* points of drag for the camera at full speed */
    FACE = 25,          /* a face button's radius */
    FACE_GAP = 47,      /* from the cluster's centre */
    DPAD = 21,
    DPAD_GAP = 36,
    PILL_W = 30, PILL_H = 15,
    TOGGLE = 44,        /* points: Apple's smallest tap target */
    TOGGLE_MARGIN = 16, /* from the safe area's corner where there is no bar (an iPad) */
    BAR_TOP = 44,       /* down a phone's bar: clear of its rounded screen corner (~62 points) */
};
#define EDIT_HOLD_NS 600000000 /* the toggle held this long: the layout edited */
#define TALL_FOOT 0.175f /* a tall screen's stick and face buttons: this much of its height up from its foot */

/* what each control is to the virtual pad */
enum Ctl
{
    C_NONE,
    C_STICK,
    C_LOOK,
    C_BUTTON, /* + the gamepad button: btn */
    C_TRIGGER,
    C_KEY,    /* a keyboard key: btn the scancode */
    C_TOGGLE, /* the controls shown or hidden; held: the layout edited */
    C_LOCK,   /* the layout locked, or being edited */
    C_RESET,  /* editing: every group back in its place */
    C_DONE,   /* editing: the layout kept */
    C_DRAG,   /* editing: a finger moving a group */
};

/* the groups that move together when the layout is edited */
enum Group
{
    G_NONE,
    G_STICK,
    G_DPAD,
    G_FACE, /* and Target */
    G_LEFT, /* L1, L2 */
    G_RIGHT,
    G_MENU, /* SELECT, START */
    G_COUNT,
};

typedef struct Control
{
    enum Ctl ctl;
    int btn;           /* SDL_GamepadButton, or the trigger's axis */
    int kind, sym;     /* how it is drawn */
    const char* label;
    float x, y, hw, hh; /* points */
    int group;
} Control;

static SDL_Mutex* g_lock;
static Control g_ctl[GFX_TOUCH_MAX];
static int g_nctl;
static int g_shown;              /* the controls are up (the game, no real controller) */
static int g_hidden;             /* put away with the toggle */
static int g_bold;               /* high contrast (the toggle's second state; Documents/touch-style.txt) */
static float g_density = 1.0f;   /* the window's pixels a point */
static float g_stick_x, g_stick_y; /* the stick's resting place */
static int g_editing;            /* the layout is being edited (the toggle held) */
static int g_group;              /* the group add() puts controls in */
static float g_off[G_COUNT][2];  /* each group's move from its place, in fractions of the safe area */
static char g_shape[32];         /* the window in points ("874x402"): a layout is kept for each */
static SDL_Window* g_win;        /* the window last laid out for */
static SDL_FRect g_safe;         /* its safe area, in points */

/* the fingers on the screen */
typedef struct Finger
{
    SDL_FingerID id;
    int ctl;            /* index into g_ctl, or -1 for the stick (floating) / the camera */
    enum Ctl what;
    float x0, y0, x, y; /* points */
    Uint64 t0;
    int group;          /* C_DRAG: the group, and its move when the finger landed */
    float ox, oy;
} Finger;
static Finger g_fingers[10];

static SDL_JoystickID g_vid;
static SDL_Joystick* g_pad;
static int g_down[SDL_GAMEPAD_BUTTON_COUNT];

/* The window the game's input goes to. The game makes several windows, and on iOS each is a window of
 * the screen's size: fingers arrive on whichever is on top (the first), the game listens on its main
 * one, which has the keyboard focus. */
static SDL_WindowID game_window(SDL_WindowID w)
{
    SDL_Window* f = SDL_GetKeyboardFocus();
    return f ? SDL_GetWindowID(f) : w;
}

/* --- Escape and clicks ----------------------------------------------------------------------- */
static Uint32 SDLCALL escape_up(void* window_id, SDL_TimerID timer, Uint32 interval)
{
    (void)timer, (void)interval;
    SDL_Event k;
    SDL_zero(k);
    k.type = SDL_EVENT_KEY_UP;
    k.key.windowID = (SDL_WindowID)(uintptr_t)window_id;
    k.key.scancode = SDL_SCANCODE_ESCAPE;
    k.key.key = SDLK_ESCAPE;
    SDL_PushEvent(&k);
    return 0;
}

static void escape(SDL_WindowID window, const char* why)
{
    window = game_window(window);
    SDL_Event k;
    SDL_zero(k);
    k.type = SDL_EVENT_KEY_DOWN;
    k.key.windowID = window;
    k.key.scancode = SDL_SCANCODE_ESCAPE;
    k.key.key = SDLK_ESCAPE;
    k.key.down = true;
    SDL_PushEvent(&k);
    SDL_AddTimer(KEY_MS, escape_up, (void*)(uintptr_t)window);
    fprintf(stderr, "[ios] %s: Escape\n", why);
}

#define CLICK_MOUSE ((SDL_MouseID)0x58490001) /* our clicks' mouse: the game's (keep_event) */
static struct
{
    SDL_WindowID window;
    float x, y;
} g_click;

static Uint32 SDLCALL click_up(void* unused, SDL_TimerID timer, Uint32 interval)
{
    (void)unused, (void)timer, (void)interval;
    SDL_Event m;
    SDL_zero(m);
    m.type = SDL_EVENT_MOUSE_BUTTON_UP;
    m.button.which = CLICK_MOUSE;
    m.button.windowID = g_click.window;
    m.button.button = SDL_BUTTON_LEFT;
    m.button.clicks = 1;
    m.button.x = g_click.x, m.button.y = g_click.y;
    SDL_PushEvent(&m);
    return 0;
}

/* A left click at (x, y), in points, as a mouse's (not a touch's: the filter keeps it) */
static void click(SDL_WindowID window, float x, float y)
{
    window = game_window(window);
    g_click.window = window, g_click.x = x, g_click.y = y;
    SDL_Event m;
    SDL_zero(m);
    m.type = SDL_EVENT_MOUSE_MOTION;
    m.motion.which = CLICK_MOUSE;
    m.motion.windowID = window;
    m.motion.x = x, m.motion.y = y;
    SDL_PushEvent(&m);
    SDL_zero(m);
    m.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    m.button.which = CLICK_MOUSE;
    m.button.windowID = window;
    m.button.button = SDL_BUTTON_LEFT;
    m.button.down = true;
    m.button.clicks = 1;
    m.button.x = x, m.button.y = y;
    SDL_PushEvent(&m);
    SDL_AddTimer(CLICK_MS, click_up, NULL);
}

/* --- the virtual pad -------------------------------------------------------------------------- */
static void pad_attach(void)
{
    if (g_pad)
        return;
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.vendor_id = 0x045E, d.product_id = 0x028E; /* as an Xbox 360 pad: what the game expects of XInput */
    d.naxes = SDL_GAMEPAD_AXIS_COUNT, d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    d.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1; /* every button: the indices are SDL's */
    d.name = "Touch controls";
    g_vid = SDL_AttachVirtualJoystick(&d);
    g_pad = g_vid ? SDL_OpenJoystick(g_vid) : NULL;
    memset(g_down, 0, sizeof g_down);
    if (!g_pad)
        fprintf(stderr, "[ios] touch controls: no virtual pad: %s\n", SDL_GetError());
}

/* every button up, the triggers too (the sticks follow the fingers: sticks()) */
static void pad_release(void)
{
    for (int b = 0; g_pad && b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        SDL_SetJoystickVirtualButton(g_pad, b, false);
    if (g_pad)
        SDL_SetJoystickVirtualAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN),
            SDL_SetJoystickVirtualAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    memset(g_down, 0, sizeof g_down);
}

static Sint16 axis(float v) { return (Sint16)(v < -1.0f ? -32768 : v > 1.0f ? 32767 : v * 32767.0f); }

/* A real controller in use: a button pressed or a stick moved past its dead zone on any pad but ours,
 * until the screen is touched again. Connected and lying idle (one a Mac shares with
 * the simulator) it does not hide the controls. */
static SDL_AtomicInt g_pad_used;

static void controller_event(const SDL_Event* e)
{
    if (e->type == SDL_EVENT_KEY_DOWN)
    {
        /* a hardware keyboard's key (ours, key() and escape(), and the on-screen keyboard's have no
         * keyboard of their own): keyboard and mouse play, the controls put away like a pad's */
        if (!e->key.which || SDL_GetAtomicInt(&g_pad_used))
            return;
        fprintf(stderr, "[ios] keyboard in use: %s\n", SDL_GetKeyboardNameForID(e->key.which));
        SDL_SetAtomicInt(&g_pad_used, 1);
        SDL_LockMutex(g_lock);
        refresh(SDL_GetKeyboardFocus());
        SDL_UnlockMutex(g_lock);
        return;
    }
    SDL_JoystickID id = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e->gbutton.which : e->gaxis.which;
    if (id == g_vid)
        return;
    if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && abs(e->gaxis.value) < 12000)
        return;
    if (SDL_GetAtomicInt(&g_pad_used))
        return;
    fprintf(stderr, "[ios] controller in use: %s\n", SDL_GetGamepadNameForID(id));
    SDL_SetAtomicInt(&g_pad_used, 1);
    SDL_LockMutex(g_lock);
    refresh(SDL_GetKeyboardFocus());
    SDL_UnlockMutex(g_lock);
}

/* --- layout ------------------------------------------------------------------------------------- */
static void add(enum Ctl ctl, int btn, int kind, int sym, const char* label, float x, float y, float hw, float hh)
{
    if (g_nctl < GFX_TOUCH_MAX)
        g_ctl[g_nctl++] = (Control){ ctl, btn, kind, sym, label, x, y, hw, hh, g_group };
}

/* --- the edited layouts: Documents/touch-layout.txt, a line a group moved: "<shape> <group> <dx> <dy>",
 * the move in fractions of the safe area, a layout for each shape of window (a phone's, an iPad's) */
static void layout_path(char* out, size_t n)
{
    const char* home = getenv("HOME");
    snprintf(out, n, "%s/Documents/touch-layout.txt", home ? home : ".");
}

/* the controls' look, kept across runs: Documents/touch-style.txt, "bold" for high contrast */
static void style_path(char* out, size_t n)
{
    const char* home = getenv("HOME");
    snprintf(out, n, "%s/Documents/touch-style.txt", home ? home : ".");
}

static void load_style(void)
{
    char path[1100], word[16] = "";
    style_path(path, sizeof path);
    FILE* f = fopen(path, "r");
    if (f)
    {
        if (fscanf(f, "%15s", word) != 1)
            word[0] = 0;
        fclose(f);
    }
    g_bold = !strcmp(word, "bold");
}

static void save_style(void)
{
    char path[1100];
    style_path(path, sizeof path);
    FILE* f = fopen(path, "w");
    if (f)
    {
        fputs(g_bold ? "bold\n" : "normal\n", f);
        fclose(f);
    }
}

static void load_layout(void)
{
    char path[1100], line[128], shape[32];
    int g;
    float dx, dy;
    memset(g_off, 0, sizeof g_off);
    layout_path(path, sizeof path);
    FILE* f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%31s %d %f %f", shape, &g, &dx, &dy) == 4 && !strcmp(shape, g_shape) && g > 0 && g < G_COUNT)
            g_off[g][0] = dx, g_off[g][1] = dy;
    fclose(f);
}

static void save_layout(void)
{
    char path[1100], line[128], shape[32], kept[4096] = "";
    layout_path(path, sizeof path);
    FILE* f = fopen(path, "r");
    if (f) /* the other shapes' lines, as they were */
    {
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "%31s", shape) == 1 && strcmp(shape, g_shape) && strlen(kept) + strlen(line) < sizeof kept)
                strcat(kept, line);
        fclose(f);
    }
    if (!(f = fopen(path, "w")))
        return;
    fputs(kept, f);
    for (int g = 1; g < G_COUNT; ++g)
        if (g_off[g][0] || g_off[g][1])
            fprintf(f, "%s %d %.4f %.4f\n", g_shape, g, g_off[g][0], g_off[g][1]);
    fclose(f);
}

/* The controls for the window as it is now; the Back button only, off the game */
static void layout(SDL_Window* win, int controls)
{
    int w = 0, h = 0, pw = 0;
    SDL_Rect safe;
    SDL_GetWindowSize(win, &w, &h);
    SDL_GetWindowSizeInPixels(win, &pw, NULL);
    if (w <= 0 || h <= 0 || !SDL_GetWindowSafeArea(win, &safe))
        return;
    g_density = (float)pw / (float)w;
    g_nctl = 0;
    g_win = win;
    float sx = (float)safe.x, sy = (float)safe.y, sw = (float)safe.w, sh = (float)safe.h;
    g_safe = (SDL_FRect){ sx, sy, sw, sh };
    char shape[32];
    snprintf(shape, sizeof shape, "%dx%d", w, h);
    if (strcmp(shape, g_shape)) /* another shape of window (or the first): its layout */
        snprintf(g_shape, sizeof g_shape, "%s", shape), load_layout();
    g_group = G_NONE;
    /* the toggle: in a phone's black bar left of the game, else in the safe area's top-left corner */
    int bar = safe.x >= TOGGLE + 8;
    float tx = bar ? sx * 0.5f : sx + TOGGLE_MARGIN + TOGGLE * 0.5f;
    float ty = sy + (bar ? BAR_TOP : TOGGLE_MARGIN) + TOGGLE * 0.5f;
    add(C_TOGGLE, 0, GFX_TOUCH_BUTTON, GFX_SYM_PAD, NULL, tx, ty, TOGGLE * 0.5f, TOGGLE * 0.5f);
    if (!controls)
        return;
    /* the lock beside it: under it in a phone's bar, right of it on an iPad */
    add(C_LOCK, 0, GFX_TOUCH_BUTTON, g_editing ? GFX_SYM_UNLOCK : GFX_SYM_LOCK, NULL, bar ? tx : tx + TOGGLE + 10,
        bar ? ty + TOGGLE + 10 : ty, TOGGLE * 0.5f, TOGGLE * 0.5f);
    /* where each group goes. A phone: the top row along the top, the D-pad upper left, the stick and
     * the face buttons in the lower corners. A tall screen (an iPad): everything gathered by the thumbs
     * at its foot, the stick and the face buttons TALL_FOOT of its height up, the rest above them */
    float left = sx + (bar ? 0.0f : 2 * TOGGLE + 10 + TOGGLE_MARGIN); /* the top row right of an iPad's toggle and lock */
    float top = sy + PILL_H + 10.0f;
    float fx, fy, dx, dy, ly, l2x, l1x, ry, r2x, r1x, sel_y;
    if (sh > 600)
    {
        float cy = sy + sh - sh * TALL_FOOT;
        fx = sx + sw - FACE_GAP - FACE - 60, fy = cy;
        g_stick_x = sx + STICK + 70, g_stick_y = cy;
        dx = g_stick_x, dy = cy - STICK - 40 - DPAD_GAP - DPAD;
        ly = dy - DPAD_GAP - DPAD - 24 - PILL_H, l2x = dx - PILL_W - 8, l1x = dx + PILL_W + 8;
        ry = fy - FACE_GAP - FACE - 10 - 3 * PILL_H - 16, r1x = fx - PILL_W - 8, r2x = fx + PILL_W + 8;
        sel_y = sy + sh - PILL_H - 24;
    }
    else
    {
        fx = sx + sw - FACE_GAP - FACE - 18, fy = sy + sh - FACE_GAP - FACE - 14;
        g_stick_x = sx + STICK + 26, g_stick_y = sy + sh - STICK - 18;
        dx = sx + DPAD_GAP + DPAD + 18, dy = top + PILL_H + 14 + DPAD_GAP + DPAD;
        ly = ry = sel_y = top;
        l2x = left + 16 + PILL_W, l1x = left + 28 + 3 * PILL_W;
        r2x = sx + sw - 16 - PILL_W, r1x = sx + sw - 28 - 3 * PILL_W;
    }
    g_group = G_LEFT;
    add(C_TRIGGER, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, GFX_TOUCH_PILL, 0, "L2", l2x, ly, PILL_W, PILL_H);
    add(C_BUTTON, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, GFX_TOUCH_PILL, 0, "L1", l1x, ly, PILL_W, PILL_H);
    g_group = G_RIGHT;
    add(C_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, GFX_TOUCH_PILL, 0, "R2", r2x, ry, PILL_W, PILL_H);
    add(C_BUTTON, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, GFX_TOUCH_PILL, 0, "R1", r1x, ry, PILL_W, PILL_H);
    g_group = G_MENU;
    add(C_BUTTON, SDL_GAMEPAD_BUTTON_BACK, GFX_TOUCH_PILL, 0, "SELECT", sx + sw * 0.5f - 44, sel_y, 36, PILL_H);
    add(C_BUTTON, SDL_GAMEPAD_BUTTON_START, GFX_TOUCH_PILL, 0, "START", sx + sw * 0.5f + 44, sel_y, 36, PILL_H);
    /* the face buttons, Target above them */
    g_group = G_FACE;
    add(C_BUTTON, SDL_GAMEPAD_BUTTON_NORTH, GFX_TOUCH_BUTTON, GFX_SYM_TRIANGLE, NULL, fx, fy - FACE_GAP, FACE, FACE);
    add(C_BUTTON, SDL_GAMEPAD_BUTTON_SOUTH, GFX_TOUCH_BUTTON, GFX_SYM_CROSS, NULL, fx, fy + FACE_GAP, FACE, FACE);
    add(C_BUTTON, SDL_GAMEPAD_BUTTON_WEST, GFX_TOUCH_BUTTON, GFX_SYM_SQUARE, NULL, fx - FACE_GAP, fy, FACE, FACE);
    add(C_BUTTON, SDL_GAMEPAD_BUTTON_EAST, GFX_TOUCH_BUTTON, GFX_SYM_CIRCLE, NULL, fx + FACE_GAP, fy, FACE, FACE);
    /* the D-pad, above the stick's place */
    g_group = G_DPAD;
    add(C_BUTTON, SDL_GAMEPAD_BUTTON_DPAD_UP, GFX_TOUCH_BUTTON, GFX_SYM_UP, NULL, dx, dy - DPAD_GAP, DPAD, DPAD);
    add(C_BUTTON, SDL_GAMEPAD_BUTTON_DPAD_DOWN, GFX_TOUCH_BUTTON, GFX_SYM_DOWN, NULL, dx, dy + DPAD_GAP, DPAD, DPAD);
    add(C_BUTTON, SDL_GAMEPAD_BUTTON_DPAD_LEFT, GFX_TOUCH_BUTTON, GFX_SYM_LEFT, NULL, dx - DPAD_GAP, dy, DPAD, DPAD);
    add(C_BUTTON, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, GFX_TOUCH_BUTTON, GFX_SYM_RIGHT, NULL, dx + DPAD_GAP, dy, DPAD, DPAD);
    /* each group where the player moved it, kept on the screen */
    for (int i = 0; i < g_nctl; ++i)
    {
        Control* c = &g_ctl[i];
        if (!c->group)
            continue;
        c->x = SDL_clamp(c->x + g_off[c->group][0] * sw, c->hw, (float)w - c->hw);
        c->y = SDL_clamp(c->y + g_off[c->group][1] * sh, c->hh, (float)h - c->hh);
    }
    g_stick_x = SDL_clamp(g_stick_x + g_off[G_STICK][0] * sw, (float)STICK, (float)w - STICK);
    g_stick_y = SDL_clamp(g_stick_y + g_off[G_STICK][1] * sh, (float)STICK, (float)h - STICK);
    if (g_editing) /* Reset and Done, in the middle */
    {
        g_group = G_NONE;
        add(C_RESET, 0, GFX_TOUCH_PILL, 0, "Reset", sx + sw * 0.5f - 48, sy + sh * 0.5f, 40, 18);
        add(C_DONE, 0, GFX_TOUCH_PILL, 0, "Done", sx + sw * 0.5f + 48, sy + sh * 0.5f, 40, 18);
    }
}

/* the group under a point (editing): anywhere in the box around its controls (the D-pad's middle
 * too), or on the stick's resting place */
static int group_at(float x, float y)
{
    for (int g = G_STICK + 1; g < G_COUNT; ++g)
    {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int i = 0; i < g_nctl; ++i)
            if (g_ctl[i].group == g)
            {
                const Control* c = &g_ctl[i];
                x0 = SDL_min(x0, c->x - c->hw), x1 = SDL_max(x1, c->x + c->hw);
                y0 = SDL_min(y0, c->y - c->hh), y1 = SDL_max(y1, c->y + c->hh);
            }
        if (x >= x0 - 6 && x <= x1 + 6 && y >= y0 - 6 && y <= y1 + 6)
            return g;
    }
    float dx = x - g_stick_x, dy = y - g_stick_y;
    return dx * dx + dy * dy <= STICK * STICK ? G_STICK : G_NONE;
}

static int hit(float x, float y)
{
    for (int i = 0; i < g_nctl; ++i)
    {
        const Control* c = &g_ctl[i];
        float slop = 6.0f; /* a little past the edge still counts */
        if (fabsf(x - c->x) <= c->hw + slop && fabsf(y - c->y) <= c->hh + slop)
            return i;
    }
    return -1;
}

/* where the stick floats from: around its resting place (wherever the player put it) */
static int stick_zone(float x, float y, int w, int h)
{
    (void)w, (void)h;
    float dx = x - g_stick_x, dy = y - g_stick_y;
    return dx * dx + dy * dy <= (STICK * 2.2f) * (STICK * 2.2f);
}

/* --- fingers ------------------------------------------------------------------------------------- */
static Finger* finger(SDL_FingerID id)
{
    for (int i = 0; i < 10; ++i)
        if (g_fingers[i].id == id)
            return &g_fingers[i];
    return NULL;
}

static void key(SDL_WindowID window, SDL_Scancode sc, int down)
{
    window = game_window(window);
    SDL_Event k;
    SDL_zero(k);
    k.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    k.key.windowID = window;
    k.key.scancode = sc;
    k.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
    k.key.down = down != 0;
    SDL_PushEvent(&k);
}

/* A control's press as the game sees it: buttons and triggers on our pad, a key as the keyboard's. Held
 * at least HOLD_MS: the pad and the keyboard are read once a frame, and a quick tap (a click in the
 * simulator) would go down and up between two reads, seen by neither. */
enum
{
    HOLD_MS = 80,
    DPAD_HOLD_MS = 100, /* the D-pad's (XIV_DPAD_HOLD_MS): three of the game's 30 Hz polls */
};
static Uint64 g_pressed_at[GFX_TOUCH_MAX]; /* SDL_GetTicks of each control's press */
static SDL_WindowID g_key_window;

static void apply(const Control* c, int down)
{
    if (c->ctl == C_KEY)
        key(g_key_window, (SDL_Scancode)c->btn, down);
    else if (g_pad && c->ctl == C_BUTTON)
        g_down[c->btn] = down, SDL_SetJoystickVirtualButton(g_pad, c->btn, down);
    else if (g_pad && c->ctl == C_TRIGGER) /* the axis's whole range is the trigger's: up at its minimum */
        SDL_SetJoystickVirtualAxis(g_pad, c->btn, down ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
}

static Uint32 SDLCALL release_later(void* control, SDL_TimerID timer, Uint32 interval)
{
    (void)timer, (void)interval;
    SDL_LockMutex(g_lock);
    Control c = *(const Control*)control; /* as it was pressed: the layout may be made again since */
    SDL_free(control);
    apply(&c, 0);
    SDL_UnlockMutex(g_lock);
    return 0;
}

static void press(int i, int down, SDL_WindowID window)
{
    const Control* c = &g_ctl[i];
    if (c->ctl != C_KEY && c->ctl != C_BUTTON && c->ctl != C_TRIGGER)
        return;
    g_key_window = window;
    Uint64 now = SDL_GetTicks(), held = now - g_pressed_at[i];
    int dpad = c->ctl == C_BUTTON && c->btn >= SDL_GAMEPAD_BUTTON_DPAD_UP && c->btn <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    static int dpad_ms = -1;
    if (dpad_ms < 0)
        dpad_ms = getenv("XIV_DPAD_HOLD_MS") ? atoi(getenv("XIV_DPAD_HOLD_MS")) : DPAD_HOLD_MS;
    Uint64 least = dpad ? (Uint64)dpad_ms : HOLD_MS;
    static int log = -1;
    if (log < 0)
        log = getenv("XIV_TOUCHLOG") && strcmp(getenv("XIV_TOUCHLOG"), "0");
    if (log)
        fprintf(stderr, "[touch] control %d (btn %d) %s at %llu ms%s\n", i, c->btn, down ? "down" : "up",
            (unsigned long long)now, down ? "" : held >= least ? " (released)" : " (held to the least)");
    if (down)
        g_pressed_at[i] = now, apply(c, 1);
    else if (held >= least)
        apply(c, 0);
    else
    {
        Control* later = (Control*)SDL_malloc(sizeof *later);
        *later = *c;
        SDL_AddTimer((Uint32)(least - held), release_later, later);
    }
}

/* the sticks from the fingers on them */
static void sticks(void)
{
    float lx = 0, ly = 0, rx = 0, ry = 0;
    for (int i = 0; i < 10; ++i)
    {
        const Finger* f = &g_fingers[i];
        if (!f->id)
            continue;
        float dx = f->x - f->x0, dy = f->y - f->y0;
        if (f->what == C_STICK)
        {
            float len = sqrtf(dx * dx + dy * dy);
            if (len > STICK)
                dx *= STICK / len, dy *= STICK / len;
            lx = dx / STICK, ly = dy / STICK;
        }
        else if (f->what == C_LOOK)
            rx = dx / LOOK, ry = dy / LOOK;
    }
    if (!g_pad)
        return;
    SDL_SetJoystickVirtualAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX, axis(lx));
    SDL_SetJoystickVirtualAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY, axis(ly)); /* SDL: down is positive */
    SDL_SetJoystickVirtualAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTX, axis(rx));
    SDL_SetJoystickVirtualAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTY, axis(ry));
}

static void finger_down(const SDL_TouchFingerEvent* e, float x, float y, int w, int h)
{
    Finger* f = finger(0);
    if (!f)
        return;
    int i = hit(x, y);
    *f = (Finger){ e->fingerID, i, C_NONE, x, y, x, y, e->timestamp, 0, 0, 0 };
    if (g_editing) /* the toggle, Reset, Done, or a group to move: nothing reaches the game */
    {
        int g = i >= 0 && g_ctl[i].group ? g_ctl[i].group : i < 0 ? group_at(x, y) : G_NONE;
        if (g)
            f->ctl = -1, f->what = C_DRAG, f->group = g, f->ox = g_off[g][0], f->oy = g_off[g][1];
        else if (i >= 0)
            f->what = g_ctl[i].ctl;
        return;
    }
    if (i >= 0)
    {
        f->what = g_ctl[i].ctl;
        if (g_shown)
            press(i, 1, e->windowID);
    }
    else if (g_shown)
        f->what = stick_zone(x, y, w, h) ? C_STICK : C_LOOK;
}

/* a finger moved (g_lock held): a dragged group follows it, a stick finger moves its stick */
static void finger_moved(Finger* f, float x, float y)
{
    f->x = x, f->y = y;
    if (f->what == C_DRAG && g_safe.w > 0 && g_win)
    {
        g_off[f->group][0] = f->ox + (x - f->x0) / g_safe.w;
        g_off[f->group][1] = f->oy + (y - f->y0) / g_safe.h;
        layout(g_win, 1);
    }
    else if (f->what == C_STICK || f->what == C_LOOK)
        sticks();
}

static void finger_up(const SDL_TouchFingerEvent* e, Finger* f, float x, float y)
{
    int over = f->ctl >= 0 && hit(x, y) == f->ctl; /* as iOS's buttons: on release, over it */
    if (f->what == C_TOGGLE && over)
    {
        if (g_editing) /* a tap while editing: done */
            g_editing = 0, save_layout();
        else if (e->timestamp - f->t0 >= EDIT_HOLD_NS) /* held: the layout edited */
            g_editing = 1, g_hidden = 0;
        else /* a tap: shown, then high contrast, then put away, then shown again */
        {
            if (g_hidden)
                g_hidden = 0;
            else if (g_bold)
                g_bold = 0, g_hidden = 1;
            else
                g_bold = 1;
            save_style();
        }
        memset(f, 0, sizeof *f);
        refresh(SDL_GetWindowFromID(e->windowID));
        return;
    }
    if (f->what == C_LOCK && over) /* unlocked: the layout edited; locked again: kept */
    {
        if (g_editing)
            save_layout();
        g_editing = !g_editing;
        memset(f, 0, sizeof *f);
        refresh(SDL_GetWindowFromID(e->windowID));
        return;
    }
    if (f->what == C_DRAG || ((f->what == C_RESET || f->what == C_DONE) && over))
    {
        if (f->what == C_RESET)
            memset(g_off, 0, sizeof g_off);
        if (f->what == C_DONE)
            g_editing = 0;
        save_layout();
        memset(f, 0, sizeof *f);
        refresh(SDL_GetWindowFromID(e->windowID));
        return;
    }
    if (g_editing)
    {
        memset(f, 0, sizeof *f);
        return;
    }
    if (f->ctl >= 0)
        press(f->ctl, 0, e->windowID);
    else if ((f->what == C_STICK || f->what == C_LOOK) && e->timestamp - f->t0 < TAP_NS &&
             fabsf(x - f->x0) < TAP_MOVE && fabsf(y - f->y0) < TAP_MOVE)
        click(e->windowID, x, y); /* a tap on the game: its menus, its targets */
    memset(f, 0, sizeof *f);
    sticks();
}

/* A mouse or a trackpad's pointer (the simulator's from a Mac, an iPad's) on the controls: a finger of
 * its own while its left button is down (g_lock held). 0: the event is ours, not the game's. */
#define POINTER_FINGER ((SDL_FingerID)-2)

static int pointer(const SDL_Event* e)
{
    Finger* f = finger(POINTER_FINGER);
    if (e->type == SDL_EVENT_MOUSE_MOTION)
    {
        if (!f)
            return 1; /* hovering: the game's cursor */
        finger_moved(f, e->motion.x, e->motion.y);
        draw();
        return 0;
    }
    SDL_TouchFingerEvent t;
    SDL_zero(t);
    t.fingerID = POINTER_FINGER, t.timestamp = e->button.timestamp, t.windowID = e->button.windowID;
    int w = 0, h = 0;
    SDL_Window* win = SDL_GetWindowFromID(t.windowID);
    if (win)
        SDL_GetWindowSize(win, &w, &h);
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && !f)
        finger_down(&t, e->button.x, e->button.y, w, h);
    else if (e->type == SDL_EVENT_MOUSE_BUTTON_UP && f)
        finger_up(&t, f, e->button.x, e->button.y);
    draw();
    return 0;
}

/* While the controls are up, the mouse is the fingers' here: SDL's of touches dropped, a pointer's
 * taken as a finger; only our own clicks (click()) are the game's */
static bool SDLCALL keep_event(void* unused, SDL_Event* e)
{
    (void)unused;
    int button = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP;
    if (!button && e->type != SDL_EVENT_MOUSE_MOTION)
        return true;
    SDL_MouseID which = button ? e->button.which : e->motion.which;
    if (which == CLICK_MOUSE)
        return true;
    float x = button ? e->button.x : e->motion.x, y = button ? e->button.y : e->motion.y;
    SDL_LockMutex(g_lock);
    int keep;
    int i = hit(x, y);
    if (!g_shown && !(i >= 0 && g_ctl[i].ctl == C_TOGGLE) && !finger(POINTER_FINGER))
        keep = 1; /* put away: only the toggle is ours */
    else if (which == SDL_TOUCH_MOUSEID)
        keep = 0;
    else if (button && e->button.button != SDL_BUTTON_LEFT)
        keep = 1;
    else
        keep = pointer(e);
    SDL_UnlockMutex(g_lock);
    return keep;
}

static bool SDLCALL touch(void* unused, SDL_Event* e)
{
    static SDL_FingerID swiping; /* the finger that started at the edge, else 0 */
    static float sx0, sy0;
    static Uint64 start;
    (void)unused;
    if (e->type == SDL_EVENT_WINDOW_SHOWN || e->type == SDL_EVENT_WINDOW_SAFE_AREA_CHANGED ||
        e->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
    {
        SDL_LockMutex(g_lock);
        refresh(SDL_GetWindowFromID(e->window.windowID));
        SDL_UnlockMutex(g_lock);
        return true;
    }
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION || e->type == SDL_EVENT_KEY_DOWN)
    {
        controller_event(e);
        return true;
    }
    if (e->type == SDL_EVENT_SCREEN_KEYBOARD_SHOWN || e->type == SDL_EVENT_SCREEN_KEYBOARD_HIDDEN)
    {
        SDL_LockMutex(g_lock);
        draw(); /* dimmed, or back */
        SDL_UnlockMutex(g_lock);
        return true;
    }
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION && e->type != SDL_EVENT_FINGER_UP &&
        e->type != SDL_EVENT_FINGER_CANCELED)
        return true;
    if (e->type == SDL_EVENT_FINGER_DOWN && SDL_GetAtomicInt(&g_pad_used))
    {
        SDL_SetAtomicInt(&g_pad_used, 0); /* back to the touch controls; this touch only brings them */
        SDL_LockMutex(g_lock);
        refresh(SDL_GetWindowFromID(e->tfinger.windowID));
        SDL_UnlockMutex(g_lock);
        return true;
    }
    int w = 0, h = 0;
    SDL_Window* win = SDL_GetWindowFromID(e->tfinger.windowID);
    if (win)
        SDL_GetWindowSize(win, &w, &h);
    float x = e->tfinger.x * (float)w, y = e->tfinger.y * (float)h;
    SDL_LockMutex(g_lock);
    Finger* f = finger(e->tfinger.fingerID);
    if (e->type == SDL_EVENT_FINGER_DOWN)
    {
        if (!swiping && e->tfinger.x < EDGE && hit(x, y) < 0)
            swiping = e->tfinger.fingerID, sx0 = e->tfinger.x, sy0 = e->tfinger.y, start = e->tfinger.timestamp;
        else if (swiping)
            swiping = 0; /* a second finger: not the back gesture */
        finger_down(&e->tfinger, x, y, w, h);
    }
    else if (e->type == SDL_EVENT_FINGER_MOTION)
    {
        if (f)
            finger_moved(f, x, y);
        if (e->tfinger.fingerID == swiping)
        {
            float dx = e->tfinger.x - sx0, dy = fabsf(e->tfinger.y - sy0);
            if (e->tfinger.timestamp - start > SWIPE_NS)
                swiping = 0;
            else if (dx >= SWIPE && dx > dy)
            {
                swiping = 0;
                if (f) /* not a click, not the stick: the gesture's */
                    f->what = C_NONE, f->ctl = -1, sticks();
                escape(e->tfinger.windowID, "swipe from the left edge");
            }
        }
    }
    else if (f)
    {
        if (e->tfinger.fingerID == swiping)
            swiping = 0;
        if (e->type == SDL_EVENT_FINGER_CANCELED)
            f->t0 = 0; /* never a tap */
        finger_up(&e->tfinger, f, x, y);
    }
    draw(); /* pressed, the stick where the thumb is: on the sign-in screen too, which has no frame hook */
    SDL_UnlockMutex(g_lock);
    return true;
}

/* --- drawing ------------------------------------------------------------------------------------ */
static void draw(void)
{
    GfxTouchShape s[GFX_TOUCH_MAX];
    int n = 0;
    float d = g_density;
    int pressed[GFX_TOUCH_MAX] = { 0 }, dragged = G_NONE;
    const Finger* stick = NULL;
    for (int i = 0; i < 10; ++i)
    {
        if (g_fingers[i].id && g_fingers[i].ctl >= 0)
            pressed[g_fingers[i].ctl] = 1;
        if (g_fingers[i].id && g_fingers[i].what == C_DRAG)
            dragged = g_fingers[i].group;
        if (g_fingers[i].id && g_fingers[i].what == C_STICK)
            stick = &g_fingers[i];
    }
    /* dimmed while iOS's keyboard is up: it covers the lower half, and typing is what is wanted */
    SDL_Window* focus = SDL_GetKeyboardFocus();
    float fade = focus && SDL_ScreenKeyboardShown(focus) ? 0.15f : 1.0f;
    for (int i = 0; i < g_nctl && n < GFX_TOUCH_MAX - 2; ++i)
    {
        const Control* c = &g_ctl[i];
        GfxTouchShape* t = &s[n++];
        memset(t, 0, sizeof *t);
        t->x = c->x * d, t->y = c->y * d, t->hw = c->hw * d, t->hh = c->hh * d;
        t->kind = c->kind, t->sym = c->sym, t->pressed = (pressed[i] || (c->group && c->group == dragged)) | (g_bold ? 2 : 0);
        t->alpha = (g_bold ? 0.97f : 0.85f) * fade;
        if (g_editing && c->group) /* editing: the groups that move pulse */
            t->alpha = 0.7f + 0.25f * sinf((float)SDL_GetTicks() / 160.0f);
        if (c->label)
            snprintf(t->label, sizeof t->label, "%s", c->label);
    }
    if (g_shown)
    {
        /* the stick: where the thumb landed while it is down, else at rest, fainter */
        float bx = stick ? stick->x0 : g_stick_x, by = stick ? stick->y0 : g_stick_y, kx = bx, ky = by;
        if (stick)
        {
            float dx = stick->x - bx, dy = stick->y - by, len = sqrtf(dx * dx + dy * dy);
            if (len > STICK)
                dx *= STICK / len, dy *= STICK / len;
            kx += dx, ky += dy;
        }
        float a = (stick ? 0.9f : g_bold ? 0.85f : 0.5f) * fade;
        if (g_editing)
            a = 0.7f + 0.25f * sinf((float)SDL_GetTicks() / 160.0f);
        int bold = g_bold ? 2 : 0;
        s[n++] = (GfxTouchShape){ bx * d, by * d, STICK * d, STICK * d, GFX_TOUCH_RING, 0, bold, a, "" };
        s[n++] = (GfxTouchShape){ kx * d, ky * d, KNOB * d, KNOB * d, GFX_TOUCH_KNOB, 0, (stick != NULL) | bold, a, "" };
    }
    gfx_set_touch_shapes(s, n);
}

/* The controls as things are now (g_lock held): up while no real controller is in use, ours the game's
 * first pad while they are; laid out for win (none: as they were) and drawn. From the start: the
 * sign-in screen takes a pad's D-pad, its confirm and back buttons, and START. */
static void refresh(SDL_Window* win)
{
    int show = (!SDL_GetAtomicInt(&g_pad_used) && !g_hidden) || g_editing;
    if (show != g_shown)
    {
        g_shown = show;
        if (show)
            pad_attach(), pad_release(); /* the triggers up: a new virtual axis is at 0, SDL's half-pressed trigger */
        else
            memset(g_fingers, 0, sizeof g_fingers), sticks(), pad_release();
        input_prefer_pad(show ? (uint32_t)g_vid : 0);
        fprintf(stderr, "[ios] touch controls %s\n", show ? "on" : g_hidden ? "off (put away)" : "off (a controller in use)");
    }
    if (win)
        layout(win, g_shown);
    draw();
}

/* Each frame of the game (host64's present hook): laid out again twice a second (the window may
 * have turned), drawn every frame */
void ios_touch_frame(int in_game)
{
    static int frames;
    (void)in_game;
    if (!g_lock)
        return;
    SDL_LockMutex(g_lock);
    if (frames++ % 30 == 0)
        refresh(SDL_GetKeyboardFocus());
    else
        draw();
    SDL_UnlockMutex(g_lock);
}

void ios_touch_init(void)
{
    load_style();
    g_lock = SDL_CreateMutex();
    SDL_SetEventFilter(keep_event, NULL);
    SDL_AddEventWatch(touch, NULL);
}
