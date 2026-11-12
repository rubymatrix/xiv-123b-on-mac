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

/* the screen in pixels, landscape, for the first config.sys (the game's window is the screen) */
static void screen_size(void)
{
    if (getenv("XIV_CONFIG_SIZE") || !SDL_InitSubSystem(SDL_INIT_VIDEO))
        return;
    const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (m)
    {
        int w = (int)(m->w * m->pixel_density), h = (int)(m->h * m->pixel_density);
        if (w < h)
        {
            int t = w;
            w = h;
            h = t;
        }
        char size[32];
        snprintf(size, sizeof size, "%dx%d", w, h);
        setenv("XIV_CONFIG_SIZE", size, 0);
        fprintf(stderr, "[ios] screen %s\n", size);
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
