/* A back end under d3d9.c that draws nothing (gfx9.h): for bringing the game up on a system before it
 * has a renderer (Android's first builds, docs/android-port.md). Every call succeeds; textures keep
 * their size and format, so a read gives zeros of the right size. The frames are paced to 60 a second
 * in g9_present, as a display would. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gfx9.h"

void rt_log(const char* fmt, ...);

struct G9Tex
{
    int type, use;
    uint32_t fmt, w, h, depth, levels;
};
struct G9Buf
{
    uint32_t size;
};
struct G9Shader
{
    uint32_t ntokens;
};

void (*g9_pace_hook)(uint32_t fps);

/* the navmesh overlay (navdraw.c, compiled into the Metal back end): nothing to draw it on */
volatile int navdraw_on;
int navdraw_key(int sc, int down, int shift)
{
    (void)sc, (void)down, (void)shift;
    return 0;
}

/* the touch controls (touch.h): nothing to draw them on */
#include "touch.h"
void gfx_set_touch_shapes(const GfxTouchShape* shapes, int n) { (void)shapes, (void)n; }

int g9_init(void* sdl_window, int vsync)
{
    (void)sdl_window, (void)vsync;
    rt_log("[gfx9] no renderer: frames are drawn to nothing (gfx9_null.c)\n");
    return 1;
}

void g9_resize(uint32_t w, uint32_t h) { (void)w, (void)h; }

int g9_format_ok(uint32_t d3dfmt, int use)
{
    (void)d3dfmt, (void)use;
    return 1;
}

G9Tex* g9_tex_create(int type, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, int use)
{
    G9Tex* t = calloc(1, sizeof *t);
    if (t)
        t->type = type, t->fmt = d3dfmt, t->w = w, t->h = h, t->depth = depth, t->levels = levels, t->use = use;
    return t;
}

void g9_tex_destroy(G9Tex* t) { free(t); }

void g9_tex_upload(G9Tex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h,
    uint32_t d, const void* src, uint32_t pitch, uint32_t slice)
{
    (void)t, (void)face, (void)level, (void)x, (void)y, (void)z, (void)w, (void)h, (void)d, (void)src, (void)pitch, (void)slice;
}

void g9_tex_read(G9Tex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    (void)face;
    if (!t || !dst)
        return;
    uint32_t h = t->h >> level;
    memset(dst, 0, (size_t)pitch * (h ? h : 1));
}

void g9_tex_gen_mips(G9Tex* t) { (void)t; }

void g9_stretch(G9Tex* src, uint32_t sface, uint32_t slevel, const int32_t srect[4], G9Tex* dst, uint32_t dface,
    uint32_t dlevel, const int32_t drect[4], int linear)
{
    (void)src, (void)sface, (void)slevel, (void)srect, (void)dst, (void)dface, (void)dlevel, (void)drect, (void)linear;
}

void g9_fill(G9Tex* t, uint32_t face, uint32_t level, const int32_t rect[4], uint32_t color)
{
    (void)t, (void)face, (void)level, (void)rect, (void)color;
}

G9Buf* g9_buf_create(uint32_t size)
{
    G9Buf* b = calloc(1, sizeof *b);
    if (b)
        b->size = size;
    return b;
}

void g9_buf_destroy(G9Buf* b) { free(b); }

void g9_buf_upload(G9Buf* b, uint32_t offset, const void* data, uint32_t size) { (void)b, (void)offset, (void)data, (void)size; }

G9Shader* g9_shader_create(const uint32_t* tokens, uint32_t ntokens)
{
    (void)tokens;
    G9Shader* s = calloc(1, sizeof *s);
    if (s)
        s->ntokens = ntokens;
    return s;
}

void g9_shader_destroy(G9Shader* s) { free(s); }

void g9_set_targets(G9Tex* const color[G9_MAX_RT], const uint8_t face[G9_MAX_RT], const uint8_t level[G9_MAX_RT], G9Tex* depth)
{
    (void)color, (void)face, (void)level, (void)depth;
}

void g9_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
    const G9Viewport* vp, const int32_t* scissor)
{
    (void)nrects, (void)rects, (void)flags, (void)color, (void)z, (void)stencil, (void)vp, (void)scissor;
}

void g9_draw(const G9Draw* d) { (void)d; }

void g9_present(G9Tex* backbuffer)
{
    (void)backbuffer;
    /* a 60 Hz display's pace: without it the game's loop spins as fast as it can */
    static struct timespec next;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!next.tv_sec)
        next = now;
    next.tv_nsec += 16666667;
    if (next.tv_nsec >= 1000000000)
        next.tv_sec++, next.tv_nsec -= 1000000000;
    if (now.tv_sec < next.tv_sec || (now.tv_sec == next.tv_sec && now.tv_nsec < next.tv_nsec))
    {
        struct timespec wait = { next.tv_sec - now.tv_sec, next.tv_nsec - now.tv_nsec };
        if (wait.tv_nsec < 0)
            wait.tv_sec--, wait.tv_nsec += 1000000000;
        nanosleep(&wait, NULL); /* macOS has no clock_nanosleep */
    }
    else
        next = now;
}

void g9_overlay(G9Tex* backbuffer) { (void)backbuffer; }

uint64_t g9_pace_refresh_ns(void) { return 0; }

void g9_finish(void) {}
