/* iOS's touch controls (tests/ios_touch.c), drawn over every frame by the Metal back end
 * (gfx9_touch.h): each shape in the window's points; copied, so the caller's array may change
 * after. n 0 for none. From xi-on-anything's gfx.h. */
#pragma once

enum
{
    GFX_TOUCH_BACK,   /* round, a chevron pointing left */
    GFX_TOUCH_BUTTON, /* round, a symbol (sym) or a label */
    GFX_TOUCH_PILL,   /* rounded, hw by hh, a label */
    GFX_TOUCH_RING,   /* a stick's base */
    GFX_TOUCH_KNOB,   /* a stick's knob */
};
enum
{
    GFX_SYM_NONE,
    GFX_SYM_CROSS, GFX_SYM_CIRCLE, GFX_SYM_SQUARE, GFX_SYM_TRIANGLE, /* the face buttons */
    GFX_SYM_UP, GFX_SYM_DOWN, GFX_SYM_LEFT, GFX_SYM_RIGHT,           /* the D-pad's */
    GFX_SYM_PAD,                                                     /* a gamepad: the controls' toggle */
    GFX_SYM_LOCK, GFX_SYM_UNLOCK,                                    /* the layout locked, or being moved */
};
typedef struct GfxTouchShape
{
    float x, y, hw, hh; /* the centre, and half the width and height (round: hw) */
    int kind, sym, pressed;
    float alpha;        /* 0..1: the whole shape's */
    char label[8];      /* PILL and BUTTON without a symbol */
} GfxTouchShape;
#define GFX_TOUCH_MAX 24
void gfx_set_touch_shapes(const GfxTouchShape* shapes, int n);
