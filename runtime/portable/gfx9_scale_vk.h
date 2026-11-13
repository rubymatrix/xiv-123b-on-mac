/* The render scale for the Vulkan back end: part of gfx9_vulkan.c (included there; it uses that file's
 * textures, passes and frames). The idea is gfx9_upscale.h's on Metal, without MetalFX.
 *
 * The game draws its 3D scene into render targets the size of the back buffer, copies the finished
 * scene into the target the interface is drawn on, then copies that into the back buffer. The scene's
 * targets (every 2D one-level render target and depth buffer the size of the back buffer, but the
 * back buffer and the interface's target) are made smaller than the game thinks: the game keeps
 * seeing its own size (G9Tex w, h), the image is rw x rh, and viewports, scissors, clear and
 * StretchRect rectangles and pixel shaders' vPos are scaled between the two (gfx9_vulkan.c). At the
 * first draw into the interface's target each frame, the scene it samples is scaled up to the game's
 * size with a filtered blit, and that copy stands in for it until the frame ends: the interface is
 * drawn sharp over it.
 *
 * Settings: XIV_RENDER_SCALE, the scene's share of the back buffer's size per axis (0.25 to 1), or
 * auto (the default on Android): from 1, lowered while the GPU takes longer than a frame at
 * XIV_RENDER_FPS (default 30) and raised again while it has room, never under XIV_RENDER_SCALE_MIN
 * (default 0.5). The GPU's time is two timestamps a command buffer. */

#define UP_MAX_RTS 512

static struct
{
    int set;                /* the settings read */
    int autom;              /* XIV_RENDER_SCALE=auto */
    float want;             /* the scale asked for, or auto's current */
    float least, budget_ms; /* auto: the least scale, the GPU's time a frame */
    G9Tex* rts[UP_MAX_RTS]; /* the 2D render targets and depth buffers that could be the scene's */
    uint32_t nrts;
    float scale;            /* the scale the scene's targets are made at */
    G9Tex* bb;              /* ... for this back buffer and interface target */
    G9Tex* ui;
    G9Tex* last_bb;         /* the back buffer last presented: copies into it name the interface's target */
    G9Tex* seen_ui;         /* ... the last so named */
    G9Tex* swapped;         /* the scene target the upscaled copy stands in for this frame */
    G9Tex* out;             /* that copy */
    int done, making, logged;
    double gpu_ms;          /* auto: the GPU's time a frame, averaged */
    uint32_t samples;
    uint64_t checked, changed; /* auto: when it last looked and last changed the scale (ns) */
} g_up;

static void up_settings(void)
{
    if (g_up.set)
        return;
    g_up.set = 1;
    const char* v = getenv("XIV_RENDER_SCALE");
#if defined(__ANDROID__)
    if (!v)
        v = "auto";
#endif
    g_up.want = 1.0f;
    if (v && !strcmp(v, "auto"))
        g_up.autom = 1;
    else if (v)
    {
        float f = (float)atof(v);
        g_up.want = f < 0.25f ? 0.25f : f > 1.0f ? 1.0f : f;
    }
    const char* m = getenv("XIV_RENDER_SCALE_MIN");
    g_up.least = m ? (float)atof(m) : 0.5f;
    g_up.least = g_up.least < 0.25f ? 0.25f : g_up.least > 1.0f ? 1.0f : g_up.least;
    const char* fps = getenv("XIV_RENDER_FPS");
    double f = fps ? atof(fps) : 30.0;
    g_up.budget_ms = 1000.0 / (f >= 5.0 ? f : 30.0);
    g_up.scale = 1.0f;
    if (g_up.autom || g_up.want < 1.0f)
        rt_log("[gfx9] render scale %s%.2f%s\n", g_up.autom ? "auto from " : "", g_up.want,
            g_up.autom ? "" : " of the back buffer");
    if (g_up.autom)
        rt_log("[gfx9] render scale: auto keeps the GPU under %.1f ms a frame, the scale at least %.2f\n", g_up.budget_ms,
            g_up.least);
}

/* GPU timestamps wanted for the automatic scale */
static int up_timed(void)
{
    up_settings();
    return g_up.autom;
}

/* a command buffer's GPU time (about a frame's: the game submits once a frame) */
static void up_gpu_time(double ns)
{
    if (!g_up.autom)
        return;
    double ms = ns / 1e6;
    g_up.gpu_ms = g_up.samples ? g_up.gpu_ms * 0.9 + ms * 0.1 : ms;
    g_up.samples++;
}

/* t at s of its size (its own size again at 1): a new image; the old one goes once the GPU is past it */
static void up_rescale(G9Tex* t, float s)
{
    uint32_t w = s < 1.0f ? (uint32_t)fmaxf(1.0f, roundf(t->w * s)) : t->w;
    uint32_t h = s < 1.0f ? (uint32_t)fmaxf(1.0f, roundf(t->h * s)) : t->h;
    if (w == t->rw && h == t->rh)
        return;
    fb_scrub(t);
    G9Tex* old = (G9Tex*)malloc(sizeof *old);
    *old = *t;
    pending_add((Trash){ 0, old, 0, { 0 } });
    t->img = VK_NULL_HANDLE;
    memset(&t->mem, 0, sizeof t->mem);
    t->view = t->srgb = VK_NULL_HANDLE;
    t->att = NULL;
    t->inited = 0;
    t->rw = w, t->rh = h;
    if (!tex_image(t))
    {
        /* no room: the game's size again (the old image's handles back would be freed under it) */
        t->rw = t->w, t->rh = t->h;
        if (!tex_image(t))
            rt_log("[gfx9] render scale: cannot remake a %ux%u target\n", t->w, t->h);
    }
    g_targets_changed = 1;
}

/* whether a target of w x h is one of the scene's for this back buffer: its size, or half of it either way
 * rounded (the game draws effects at half size with the scene's depth buffer bound, which would otherwise
 * be smaller than them) */
static int up_scene_size(const G9Tex* bb, uint32_t w, uint32_t h)
{
    if (w == bb->w && h == bb->h)
        return 1;
    return (w == bb->w / 2 || w == (bb->w + 1) / 2) && (h == bb->h / 2 || h == (bb->h + 1) / 2);
}

/* a new render target: one the size of the scene's is made at the render scale at once (a frame with
 * its targets at different sizes cannot be drawn) */
static void up_track(G9Tex* t)
{
    up_settings();
    if (g_up.making || t->type != G9_TEX_2D || t->levels != 1 || (t->use != G9_USE_RT && t->use != G9_USE_DEPTH) ||
        g_up.nrts >= UP_MAX_RTS)
        return;
    g_up.rts[g_up.nrts++] = t;
    if (g_up.bb && g_up.ui && g_up.scale < 1.0f && up_scene_size(g_up.bb, t->w, t->h))
    {
        t->scene = 1;
        t->rw = (uint32_t)fmaxf(1.0f, roundf(t->w * g_up.scale));
        t->rh = (uint32_t)fmaxf(1.0f, roundf(t->h * g_up.scale));
    }
}

static void up_forget(G9Tex* t)
{
    if (g_up.swapped == t)
        g_up.swapped = NULL;
    if (g_up.out == t)
        g_up.out = NULL, g_up.swapped = NULL;
    for (uint32_t i = 0; i < g_up.nrts; ++i)
        if (g_up.rts[i] == t)
        {
            g_up.rts[i] = g_up.rts[--g_up.nrts];
            break;
        }
    if (g_up.bb == t)
        g_up.bb = NULL;
    if (g_up.ui == t)
        g_up.ui = NULL;
    if (g_up.seen_ui == t)
        g_up.seen_ui = NULL;
    if (g_up.last_bb == t)
        g_up.last_bb = NULL;
}

/* which targets are the scene's, and at what scale (between frames) */
static void up_choose(G9Tex* bb, G9Tex* ui, float scale)
{
    if (bb == g_up.bb && ui == g_up.ui && scale == g_up.scale)
        return;
    g_up.bb = bb, g_up.ui = ui, g_up.scale = scale;
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_up.nrts; ++i)
    {
        G9Tex* t = g_up.rts[i];
        t->scene = scale < 1.0f && bb && ui && t != bb && t != ui && up_scene_size(bb, t->w, t->h);
        up_rescale(t, t->scene ? scale : 1.0f);
        n += t->scene;
    }
    g_targets_changed = 1;
    if (n || scale < 1.0f)
        rt_log("[gfx9] render scale %.2f: %u scene targets of %ux%u at %ux%u\n", scale, n, bb ? bb->w : 0, bb ? bb->h : 0,
        bb ? (uint32_t)fmaxf(1.0f, roundf(bb->w * scale)) : 0, bb ? (uint32_t)fmaxf(1.0f, roundf(bb->h * scale)) : 0);
}

/* A copy into the back buffer: its source is where the interface is drawn */
static void up_note_stretch(G9Tex* src, G9Tex* dst)
{
    if (dst && dst == g_up.last_bb && src && src != dst && src->type == G9_TEX_2D && !src->scene)
        g_up.seen_ui = src;
}

/* what a draw or copy reads in place of t: the upscaled scene for the rest of its frame */
static G9Tex* up_stand_in(G9Tex* t)
{
    return t && t == g_up.swapped && g_up.out ? g_up.out : t;
}

/* a rectangle in t's game pixels to its image's; whether it was scaled */
static int rect_to_image(const G9Tex* t, int32_t r[4])
{
    if (t->rw == t->w && t->rh == t->h)
        return 0;
    float sx = (float)t->rw / (float)t->w, sy = (float)t->rh / (float)t->h;
    r[0] = (int32_t)floorf(r[0] * sx), r[1] = (int32_t)floorf(r[1] * sy);
    r[2] = (int32_t)ceilf(r[2] * sx), r[3] = (int32_t)ceilf(r[3] * sy);
    return 1;
}

/* a scaled target at the game's size, made for the caller (GetRenderTargetData) */
static G9Tex* up_full_copy(G9Tex* t)
{
    g_up.making = 1;
    G9Tex* full = g9_tex_create(G9_TEX_2D, t->fmt, t->w, t->h, 1, 1, G9_USE_RT);
    g_up.making = 0;
    if (full)
        g9_stretch(t, 0, 0, NULL, full, 0, 0, NULL, 1);
    return full;
}

/* the scene t at the game's size into g_up.out, which stands in for it until the frame ends */
static void up_apply(G9Tex* t)
{
    VkFormatFeatureFlags ff = fmt_features(t->f.vf);
    if (!(ff & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(ff & VK_FORMAT_FEATURE_BLIT_DST_BIT) || t->f.block)
        return; /* sampled at its own size: the composite's filtering scales it */
    G9Tex* o = g_up.out;
    if (!o || o->w != t->w || o->h != t->h || o->fmt != t->fmt)
    {
        if (o)
            g9_tex_destroy(o);
        g_up.making = 1;
        g_up.out = o = g9_tex_create(G9_TEX_2D, t->fmt, t->w, t->h, 1, 1, G9_USE_RT);
        g_up.making = 0;
        if (!o)
            return;
    }
    end_pass();
    cmd();
    tex_prepare(t);
    tex_prepare(o);
    barrier_image(t, t->rest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
    barrier_image(o, o->rest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 0, 1);
    VkImageBlit b = { 0 };
    b.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    b.srcOffsets[1] = (VkOffset3D){ (int32_t)t->rw, (int32_t)t->rh, 1 };
    b.dstSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    b.dstOffsets[1] = (VkOffset3D){ (int32_t)o->w, (int32_t)o->h, 1 };
    vkCmdBlitImage(g_cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
        t->nolinear ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    barrier_image(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->rest, 0, 1, 0, 1);
    barrier_image(o, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, o->rest, 0, 1, 0, 1);
    g_up.swapped = t;
    g_targets_changed = 1;
    if (!g_up.logged)
        g_up.logged = 1, rt_log("[gfx9] render scale: the scene upscaled from %ux%u to %ux%u\n", t->rw, t->rh, t->w, t->h);
}

/* Before a draw: the first into the interface's target this frame upscales the scaled scene target it
 * samples */
static void up_hook(const G9Draw* d)
{
    if (g_up.scale >= 1.0f || g_up.done || !g_up.ui || g_rt[0] != g_up.ui)
        return;
    g_up.done = 1;
    const SmInfo* pi = &d->ps->info;
    for (uint32_t s = 0; s < G9_PS_SAMPLERS; ++s)
    {
        G9Tex* t = d->tex[s];
        if (pi->sampler[s] && t && t->scene && !t->f.depth && t->w == g_up.ui->w && t->h == g_up.ui->h)
        {
            up_apply(t);
            return;
        }
    }
}

/* auto, twice a second: a step down while the GPU is over its budget (two when far over), up while it
 * has a third to spare; two seconds of the new scale's timings before the next change */
static float up_auto_step(void)
{
    float s = g_up.want;
    uint64_t now = now_ns();
    if (now - g_up.checked < 500000000u || now - g_up.changed < 2000000000u || g_up.samples < 8)
        return s;
    g_up.checked = now;
    float next = s;
    if (g_up.gpu_ms > g_up.budget_ms * 1.5 && s > g_up.least)
        next = s - 0.2f;
    else if (g_up.gpu_ms > g_up.budget_ms * 1.05 && s > g_up.least)
        next = s - 0.1f;
    else if (g_up.gpu_ms < g_up.budget_ms * 0.66 && s < 1.0f)
        next = s + 0.05f;
    next = roundf(next * 20.0f) / 20.0f;
    next = next < g_up.least ? g_up.least : next > 1.0f ? 1.0f : next;
    if (next != s)
    {
        rt_log("[gfx9] render scale auto: GPU %.1f ms a frame (budget %.1f): %.2f to %.2f\n", g_up.gpu_ms, g_up.budget_ms, s,
            next);
        g_up.changed = now;
        g_up.samples = 0;
    }
    return next;
}

/* At present: the scene's own textures back, the next frame's targets */
static void up_frame_end(G9Tex* bb)
{
    up_settings();
    g_up.swapped = NULL;
    g_up.done = 0;
    g_up.last_bb = bb;
    if (g_up.autom)
        g_up.want = up_auto_step();
    float s = g_up.want;
    up_choose(bb, g_up.seen_ui, bb && g_up.seen_ui ? s : 1.0f);
}
