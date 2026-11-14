/* SDL entry point of the Android app (tools/build_xiv_android.py, docs/android-port.md).
 *
 * SDL's Java (SDLActivity) loads libmain.so and calls SDL_main. This finds the game folder in the app's
 * external files folder (/sdcard/Android/data/<app>/files/FINAL FANTASY XIV, where adb pushes it),
 * reads run.args there (as on iOS: XIV_ lines set the environment, others are xivboot's arguments),
 * sends the log to xivboot.log there, sizes the first config.sys for the screen, then starts the boot
 * test (tests/xivboot.c, built with main renamed to xivboot_main), with iOS's touch controls
 * (tests/ios_touch.c) over it. The sign-in screen (android/.../SignInActivity.java) passes the session,
 * the server and the lobby as arguments (--xiv-session, --xiv-server, --xiv-lobby), which become the
 * launch argument (xiv_args.c) unless run.args gives one (XIV_ARGS). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "xiv_args.h"

int xivboot_main(int argc, char** argv);
extern void (*xivboot_main_hook)(void); /* xivboot.c */
void ios_touch_init(void);              /* ios_touch.c: the touch controls, shared with iOS */
void ios_touch_frame(int in_game);

static int g_touch_on;

/* Each turn of the main loop (SDL is up by the first): the touch controls set up once, then laid out
 * at about 60 Hz (XIV_TOUCH=0: none), as on iOS */
static void main_tick(void)
{
    static int up;
    static Uint64 last;
    if (!g_touch_on)
        return;
    Uint64 now = SDL_GetTicks();
    if (!up)
        up = 1, ios_touch_init();
    if (now - last >= 16)
        last = now, ios_touch_frame(1);
}

/* the Mac's Dock icon (app_icon.m): an Android app's icon is its package's */
void app_icon_from_image(uint32_t base) { (void)base; }

#define MAX_ARGS 32

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
            fprintf(stderr, "[android] %s set from run.args\n", line);
        }
        else if (n < room)
            out[n++] = strdup(line);
    }
    fclose(f);
    return n;
}

/* the first config.sys: 720 lines (the game's least, its interface as large as it gets) at the screen's
 * shape, as on iOS (xivboot_ios.c) */
static void screen_size(void)
{
    if (getenv("XIV_CONFIG_SIZE") || !SDL_InitSubSystem(SDL_INIT_VIDEO))
        return;
    const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (m && m->w > 0 && m->h > 0)
    {
        int w = m->w > m->h ? m->w : m->h, h = m->w > m->h ? m->h : m->w;
        int cw = (720 * w / h + 4) & ~7;
        if (cw < 1024)
            cw = 1024;
        char size[32];
        snprintf(size, sizeof size, "%dx%d", cw, 720);
        setenv("XIV_CONFIG_SIZE", size, 0);
        fprintf(stderr, "[android] screen %dx%d: the game at %s\n", w, h, size);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

/* the sign-in screen's session as the game's launch argument, the server and lobby beside it */
static void signed_in(int argc, char** argv)
{
    const char *session = NULL, *server = NULL, *lobby = NULL;
    for (int i = 1; i + 1 < argc; i += 2)
        if (!strcmp(argv[i], "--xiv-session"))
            session = argv[i + 1];
        else if (!strcmp(argv[i], "--xiv-server"))
            server = argv[i + 1];
        else if (!strcmp(argv[i], "--xiv-lobby"))
            lobby = argv[i + 1];
    if (!session || getenv("XIV_ARGS"))
        return;
    char arg[512];
    if (!xiv_launch_arg(session, XIV_ARGS_TICK, arg, sizeof arg))
    {
        fprintf(stderr, "[android] the sign-in's session is not one the game takes\n");
        return;
    }
    setenv("XIV_ARGS", arg, 1);
    setenv("XIV_TICK", "0x10000", 1);
    if (server && *server)
        setenv("XIV_SERVER_HOST", server, 0); /* run.args may name others */
    if (lobby && *lobby)
        setenv("XIV_LOBBY", lobby, 0);
    else if (server && *server)
        setenv("XIV_LOBBY", server, 0);
    fprintf(stderr, "[android] signed in; server %s\n", server ? server : "?");
}

int main(int argc, char** argv)
{
    const char* files = SDL_GetAndroidExternalStoragePath();
    if (!files)
        files = SDL_GetAndroidInternalStoragePath();
    char path[1200];
    mkdir(files, 0755);
    snprintf(path, sizeof path, "%s/xivboot.log", files);
    FILE* empty = fopen(path, "w"); /* emptied once; then both streams append */
    if (empty)
        fclose(empty);
    if (freopen(path, "a", stderr))
        setvbuf(stderr, NULL, _IOLBF, 0);
    if (freopen(path, "a", stdout))
        setvbuf(stdout, NULL, _IOLBF, 0);
    fprintf(stderr, "[android] files at %s\n", files);

    static char* extra[MAX_ARGS];
    snprintf(path, sizeof path, "%s/run.args", files);
    int n_extra = read_args(path, extra, MAX_ARGS);
    signed_in(argc, argv);

    static char game[1100], exe[1200];
    if (getenv("XIV_GAME"))
        snprintf(game, sizeof game, "%s", getenv("XIV_GAME"));
    else
        snprintf(game, sizeof game, "%s/FINAL FANTASY XIV", files);
    snprintf(exe, sizeof exe, "%s/ffxivgame.exe", game);
    if (access(exe, R_OK))
    {
        fprintf(stderr, "[android] no game at %s: push the patched \"FINAL FANTASY XIV\" folder there "
                        "(tools/build_xiv_android.py push-game)\n", game);
        fflush(NULL);
        exit(1);
    }
    /* SDL sets the activity's orientation when it makes the window, over the manifest's: without the
     * hint it chose the phone's natural one (portrait), and the window came out 1080x2400 */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    /* SDL's default on Android is AAudio's low-latency mode with buffers of a few milliseconds, which ran
     * dry and crackled while the game was busy; 2048 frames is about 43 ms at 48 kHz (1024 still clicked now and then). As a default-priority
     * hint, the SDL_AUDIO_DEVICE_SAMPLE_FRAMES environment variable still overrides it. */
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "2048", SDL_HINT_DEFAULT);
    /* and AAudio's ordinary mode, not its low-latency one (SDL's default), whose small device buffers popped
     * even with the larger stream: a game's sound gains nothing from the lower latency */
    SDL_SetHintWithPriority(SDL_HINT_ANDROID_LOW_LATENCY_AUDIO, "0", SDL_HINT_DEFAULT);
    setenv("XIV_DOCUMENTS", files, 0); /* the game's My Games under the app's files */
    setenv("XIV_DISCORD", "log", 0); /* no Discord app: where the game is goes to the log ([state]) */
    screen_size();
    g_touch_on = !getenv("XIV_TOUCH") || strcmp(getenv("XIV_TOUCH"), "0");
    xivboot_main_hook = main_tick;
    if (g_touch_on)
        setenv("XIV_VPAD", "1", 0); /* a pad 0 from the start, which the touch controls' buttons go to (xivboot_ios.c) */
    if (!getenv("XIV_ARGS"))
        fprintf(stderr, "[android] not signed in (no session, no XIV_ARGS in run.args): the game will not get past its title\n");

    static char* args[MAX_ARGS + 3];
    int n = 0;
    args[n++] = argv[0];
    args[n++] = exe;
    if (n_extra == 0)
        args[n++] = game;
    for (int i = 0; i < n_extra; ++i)
        args[n++] = extra[i];
    args[n] = NULL;
    fprintf(stderr, "[android] xivboot %s (%d more arguments)\n", exe, n - 2);
    int ret = xivboot_main(n, args);
    fprintf(stderr, "[android] xivboot exit %d\n", ret);
    fflush(NULL);
    exit(ret);
}
