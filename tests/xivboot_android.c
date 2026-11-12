/* SDL entry point of the Android app (tools/build_xiv_android.py, docs/android-port.md).
 *
 * SDL's Java (SDLActivity) loads libmain.so and calls SDL_main. This finds the game folder in the app's
 * external files folder (/sdcard/Android/data/<app>/files/FINAL FANTASY XIV, where adb pushes it),
 * reads run.args there (as on iOS: XIV_ lines set the environment, others are xivboot's arguments),
 * sends the log to xivboot.log there, sizes the first config.sys for the screen, then starts the boot
 * test (tests/xivboot.c, built with main renamed to xivboot_main). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

int xivboot_main(int argc, char** argv);

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

int main(int argc, char** argv)
{
    (void)argc;
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
    setenv("XIV_DOCUMENTS", files, 0); /* the game's My Games under the app's files */
    setenv("XIV_DISCORD", "log", 0); /* no Discord app: where the game is goes to the log ([state]) */
    screen_size();
    if (!getenv("XIV_ARGS"))
        fprintf(stderr, "[android] no XIV_ARGS in run.args: the game will not get past its title\n");

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
