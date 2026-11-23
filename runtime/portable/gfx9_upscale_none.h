/* Upscaling off: part of gfx9_metal.m in place of gfx9_upscale.h where the SDK has no MetalFX (the
 * iOS simulator's). The scene's targets stay at the game's size, so there is nothing to track,
 * jitter or upscale; the shaders' render scale is the draw's own. */

static void up_track(G9Tex* t) { (void)t; }
static void up_forget(G9Tex* t) { (void)t; }
static int up_mode(void) { return 0; }
static void up_apply(G9Tex* t) { (void)t; }
static void up_frame_end(G9Tex* bb) { (void)bb; }

static void up_draw_aux(const G9Draw* d, SmAux* aux, float sx, float sy)
{
    (void)d;
    aux->rscale[0] = 1.0f / sx, aux->rscale[1] = 1.0f / sy;
}
