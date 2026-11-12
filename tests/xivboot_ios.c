/* SDL entry point of the iPad and iPhone app (tools/build_xiv_ios.py, docs/ios-port.md).
 *
 * SDL_main.h turns main() into SDL_main and runs it inside UIApplicationMain. This finds the game
 * folder the player copied into the app's Documents, reads Documents/run.args, sends the log to
 * Documents/xivboot.log, sizes the first config.sys for the screen, then starts the boot test
 * (tests/xivboot.c, built with main renamed to xivboot_main).
 *
 * Documents/run.args: one item per line, no shell expansion. A line NAME=VALUE whose NAME starts
 * with XIV_ sets that environment variable (XIV_ARGS, XIV_TICK, XIV_SERVER_HOST, XIV_DIRECT_LOGIN
 * and the rest the runtime reads); any other line is an argument for xivboot after the exe.
 * Blank lines and lines starting with # are skipped.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

int xivboot_main(int argc, char** argv);
extern void (*xivboot_main_hook)(void); /* xivboot.c */
void xivboot_set_fps_cap(int fps);
void ios_touch_init(void);              /* ios_touch.c */
void ios_touch_frame(int in_game);
int ios_power_fps(const char** why);    /* ios_power.m */

static int g_touch_on, g_power_on;

/* Each turn of the main loop (SDL is up by the first): the touch controls set up once, then laid out
 * and drawn at about 60 Hz (XIV_TOUCH=0: none); the frame cap from the phone's state every 2 s
 * (ios_power.m; not while XIV_FPSCAP fixes it) */
static void main_tick(void)
{
    static int up, cap;
    static Uint64 last_touch, last_power;
    Uint64 now = SDL_GetTicks();
    if (g_touch_on)
    {
        if (!up)
            up = 1, ios_touch_init();
        if (now - last_touch >= 16)
            last_touch = now, ios_touch_frame(1);
    }
    if (g_power_on && (!last_power || now - last_power >= 2000))
    {
        last_power = now;
        const char* why = "";
        int fps = ios_power_fps(&why);
        if (fps != cap)
        {
            cap = fps;
            xivboot_set_fps_cap(fps);
            fprintf(stderr, "[ios] frame cap %d (%s)\n", fps, why);
        }
    }
}

#define MAX_ARGS 32

static int is_dir(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int read_args(const char* path, char** out, int room)
{
    FILE* f = fopen(path, "r");
    if (!f)
        return 0;
    int n = 0;
    char line[4096];
    while (fgets(line, sizeof line, f))
    {
        size_t len = strcspn(line, "\r\n");
        line[len] = 0;
        if (!len || line[0] == '#')
            continue;
        char* eq = strchr(line, '=');
        if (!strncmp(line, "XIV_", 4) && eq)
        {
            *eq = 0;
            setenv(line, eq + 1, 1);
            fprintf(stderr, "[ios] %s set from run.args\n", line);
        }
        else if (n < room)
            out[n++] = strdup(line);
    }
    fclose(f);
    return n;
}

/* The first config.sys's size: 720 lines (the least the game takes, so its interface is as large as
 * it gets; the game draws it at a fixed pixel size) at the screen's shape, scaled up to the screen
 * when presented. A phone's screen at its own resolution makes the text unreadably small.
 * XIV_CONFIG_SIZE=<w>x<h> in run.args chooses another. */
static void screen_size(void)
{
    if (getenv("XIV_CONFIG_SIZE") || !SDL_InitSubSystem(SDL_INIT_VIDEO))
        return;
    const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (m && m->w > 0 && m->h > 0)
    {
        int w = m->w > m->h ? m->w : m->h, h = m->w > m->h ? m->h : m->w; /* landscape */
        int cw = (720 * w / h + 4) & ~7;
        if (cw < 1024)
            cw = 1024;
        char size[32];
        snprintf(size, sizeof size, "%dx%d", cw, 720);
        setenv("XIV_CONFIG_SIZE", size, 0);
        fprintf(stderr, "[ios] screen %dx%d points x%.0f: the game at %s\n", w, h, m->pixel_density, size);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int main(int argc, char** argv)
{
    (void)argc;
    const char* home = getenv("HOME"); /* the app's container */
    char docs[1024], path[1200];
    snprintf(docs, sizeof docs, "%s/Documents", home ? home : ".");
    mkdir(docs, 0755);
    /* from the home screen there is no terminal; from simctl launch --console-pty there is */
    if (!isatty(2))
    {
        snprintf(path, sizeof path, "%s/xivboot.log", docs);
        if (freopen(path, "w", stderr))
            setvbuf(stderr, NULL, _IOLBF, 0);
        if (freopen(path, "a", stdout))
            setvbuf(stdout, NULL, _IOLBF, 0);
    }
    static char* extra[MAX_ARGS];
    snprintf(path, sizeof path, "%s/run.args", docs);
    int n_extra = read_args(path, extra, MAX_ARGS);

    /* the game folder: XIV_GAME (a simulator can use the Mac's own), else Documents/FINAL FANTASY XIV */
    static char game[1100];
    if (getenv("XIV_GAME"))
        snprintf(game, sizeof game, "%s", getenv("XIV_GAME"));
    else
        snprintf(game, sizeof game, "%s/FINAL FANTASY XIV", docs);
    static char exe[1200];
    snprintf(exe, sizeof exe, "%s/ffxivgame.exe", game);
    if (!is_dir(game) || access(exe, R_OK))
    {
        fprintf(stderr, "[ios] no game at %s: copy the patched \"FINAL FANTASY XIV\" folder into the app's "
                        "Documents (Finder or the Files app)\n", game);
        fflush(NULL);
        exit(1);
    }
    /* the game's "My Documents" is the app's Documents: its settings, logs and screenshots land in
     * Documents/My Games/FINAL FANTASY XIV, where the Files app shows them */
    setenv("XIV_DOCUMENTS", docs, 0);
    setenv("XIV_DISCORD", "0", 0); /* no Discord app to talk to */
    screen_size();
    g_power_on = !getenv("XIV_FPSCAP");
    g_touch_on = !getenv("XIV_TOUCH") || strcmp(getenv("XIV_TOUCH"), "0");
    xivboot_main_hook = main_tick;
    if (g_touch_on)
    {
        /* a pad 0 from the start (input.c's, which the touch controls' and a real controller's
         * buttons are added to): the touch controls' own pad comes only once the main loop runs,
         * after the game has looked for its pad, and the game's menus then took no D-pad presses
         * from it (held, its repeats got through) */
        setenv("XIV_VPAD", "1", 0);
    }

    static char* args[MAX_ARGS + 3];
    int n = 0;
    args[n++] = argv[0];
    args[n++] = exe;
    if (n_extra == 0)
        args[n++] = game;
    for (int i = 0; i < n_extra; ++i)
        args[n++] = extra[i];
    args[n] = NULL;
    fprintf(stderr, "[ios] xivboot %s (%d more arguments)\n", exe, n - 2);
    int ret = xivboot_main(n, args);
    fprintf(stderr, "[ios] xivboot exit %d\n", ret);
    /* returning from SDL_main leaves an iOS app running with a black window */
    fflush(NULL);
    exit(ret);
}
