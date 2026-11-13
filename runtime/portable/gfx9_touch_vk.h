/* The touch controls (tests/ios_touch.c, touch.h) for the Vulkan back end: part of gfx9_vulkan.c,
 * drawn over the frame in the present pass, as gfx9_touch.h does for Metal. The shaders are
 * gfx9_touch.h's in GLSL. Android has no CoreText, so the labels (L1, SELECT, Done...) are drawn
 * with a small stroke font into an atlas once: each letter a few line segments, inked by the
 * distance to them. The shapes come in the window's pixels (ios_touch.c's points times its density). */
#include <strings.h>
#include "touch.h"

static const char TOUCH_VS[] =
    "#version 450\n"
    "layout(push_constant) uniform U { vec4 rect; vec4 cell; uint kind, sym, pressed; float alpha; vec2 size; } u;\n"
    "void main() {\n"
    "    vec2 c = u.rect.xy + vec2((gl_VertexIndex & 1) != 0 ? u.rect.z : 0.0, (gl_VertexIndex & 2) != 0 ? u.rect.w : 0.0);\n"
    "    gl_Position = vec4(c.x / u.size.x * 2.0 - 1.0, c.y / u.size.y * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char TOUCH_FS[] =
    "#version 450\n"
    "layout(push_constant) uniform U { vec4 rect; vec4 cell; uint kind, sym, pressed; float alpha; vec2 size; } u;\n"
    "layout(set = 0, binding = 0) uniform sampler2D labels;\n"
    "layout(location = 0) out vec4 o;\n"
    "float clamp01(float x) { return clamp(x, 0.0, 1.0); }\n"
    "float seg(vec2 p, vec2 a, vec2 b) {\n"
    "  vec2 pa = p - a, ba = b - a; return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0));\n"
    "}\n"
    "float tri(vec2 p, float r) {\n"
    "  const float k = 1.7320508; p.x = abs(p.x) - r; p.y = p.y + r / k;\n"
    "  if (p.x + k * p.y > 0.0) p = vec2(p.x - k * p.y, -k * p.x - p.y) / 2.0;\n"
    "  p.x -= clamp(p.x, -2.0 * r, 0.0); return -length(p) * sign(p.y);\n"
    "}\n"
    "float body_sd(uint kind, vec2 d, vec2 hf, float r) {\n"
    "  if (kind == 2u) { vec2 q = abs(d) - vec2(max(hf.x * 0.86 - r, 0.0), 0.0); return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r; }\n"
    "  return length(d) - r;\n"
    "}\n"
    "void main() {\n"
    "  vec2 hf = u.rect.zw * 0.5, c = u.rect.xy + hf, d = gl_FragCoord.xy - c;\n"
    "  float r = min(hf.x, hf.y) * 0.86, sd = body_sd(u.kind, d, hf, r);\n"
    "  float body = 1.0 - smoothstep(-1.5, 0.0, sd);\n"
    "  float shadow = (1.0 - smoothstep(0.0, r * 0.2, body_sd(u.kind, d - vec2(0.0, r * 0.06), hf, r))) * 0.3;\n"
    "  float rim = body * (1.0 - smoothstep(0.5, 2.0, abs(sd + 1.25)));\n"
    "  vec2 p = d / r; float aa = 1.5 / r;\n"
    "  float ink = 0.0; vec3 inkc = vec3(1.0);\n"
    "  if (u.kind == 0u) {\n"
    "    vec2 q = vec2(p.x + 0.06, abs(p.y));\n"
    "    ink = 1.0 - smoothstep(0.065 - aa, 0.065 + aa, seg(q, vec2(-0.2, 0.0), vec2(0.14, 0.34)));\n"
    "  } else if (u.sym != 0u) {\n"
    "    float g;\n"
    "    if (u.sym == 1u) { g = min(seg(p, vec2(-0.32, -0.32), vec2(0.32, 0.32)), seg(p, vec2(-0.32, 0.32), vec2(0.32, -0.32))) - 0.075; inkc = vec3(0.49, 0.70, 0.95); }\n"
    "    else if (u.sym == 2u) { g = abs(length(p) - 0.34) - 0.075; inkc = vec3(1.0, 0.45, 0.45); }\n"
    "    else if (u.sym == 3u) { g = abs(max(abs(p.x), abs(p.y)) - 0.29) - 0.07; inkc = vec3(0.93, 0.62, 0.86); }\n"
    "    else if (u.sym == 4u) { g = abs(tri(vec2(p.x, -p.y - 0.06), 0.36)) - 0.07; inkc = vec3(0.30, 0.90, 0.66); }\n"
    "    else if (u.sym == 9u) {\n"
    "      vec2 b = abs(p - vec2(0.0, -0.02)) - vec2(0.30, 0.10);\n"
    "      float body = min(length(max(b, 0.0)) + min(max(b.x, b.y), 0.0) - 0.12, length(vec2(abs(p.x) - 0.33, p.y - 0.12)) - 0.17);\n"
    "      vec2 dp = abs(p - vec2(-0.27, -0.02));\n"
    "      float plus = min(max(dp.x - 0.10, dp.y - 0.032), max(dp.x - 0.032, dp.y - 0.10));\n"
    "      float dots = min(length(p - vec2(0.24, -0.07)), length(p - vec2(0.33, 0.02))) - 0.045;\n"
    "      g = max(body, -min(plus, dots));\n"
    "    }\n"
    "    else if (u.sym == 10u || u.sym == 11u) {\n"
    "      vec2 b = abs(p - vec2(0.0, 0.14)) - vec2(0.28, 0.2);\n"
    "      float body = length(max(b, 0.0)) + min(max(b.x, b.y), 0.0) - 0.05;\n"
    "      vec2 q = p - (u.sym == 10u ? vec2(0.0, -0.1) : vec2(0.12, -0.22));\n"
    "      float arc = q.y < 0.0 ? abs(length(q) - 0.18) - 0.06 : max(abs(abs(q.x) - 0.18) - 0.06, q.y - 0.14);\n"
    "      g = min(body, arc);\n"
    "    }\n"
    "    else {\n"
    "      vec2 q = u.sym == 5u ? vec2(p.x, -p.y) : u.sym == 6u ? p : u.sym == 7u ? vec2(p.y, -p.x) : vec2(p.y, p.x);\n"
    "      g = tri(q, 0.3);\n"
    "    }\n"
    "    ink = 1.0 - smoothstep(-aa, aa, g);\n"
    "  } else if (u.cell.z > 0.0) {\n"
    "    float lw = u.kind == 2u ? hf.x * 1.5 : r * 1.7, lh = lw * 0.375;\n"
    "    vec2 uv = (d + vec2(lw, lh) * 0.5) / vec2(lw, lh);\n"
    "    if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) ink = texture(labels, u.cell.xy + uv * u.cell.zw).r;\n"
    "  }\n"
    "  float top = clamp01(p.y * 0.5 + 0.5);\n"
    "  vec3 glass = mix(vec3(0.30, 0.30, 0.33), vec3(0.11, 0.11, 0.13), top);\n"
    "  float fill = 0.6;\n"
    "  bool bold = (u.pressed & 2u) != 0u;\n"
    "  vec3 rimc = vec3(1.0); float rimk = 0.35;\n"
    "  if (bold) { glass = mix(vec3(0.98), vec3(0.80), top); fill = 0.93; rimc = vec3(0.0); rimk = 0.9;\n"
    "              inkc = u.sym != 0u ? inkc * 0.55 : vec3(0.04); }\n"
    "  if ((u.pressed & 1u) != 0u) glass = bold ? mix(glass, vec3(0.45), 0.5) : mix(glass, vec3(0.75), 0.45), fill = bold ? 0.97 : 0.8;\n"
    "  if (u.kind == 3u) { glass = bold ? vec3(0.95) : vec3(0.1); fill = bold ? 0.45 : 0.22; rim *= 1.6; }\n"
    "  if (u.kind == 4u) { glass = mix(vec3(0.85), vec3(0.6), top); fill = bold ? 0.97 : 0.85; }\n"
    "  vec3 col = mix(mix(glass, rimc, clamp01(rim * rimk)), inkc, ink);\n"
    "  float a = max(body * mix(fill, 0.97, ink), shadow * (1.0 - body)) * u.alpha;\n"
    "  o = vec4(body > 0.0 ? col : vec3(0.0), a);\n"
    "}\n";

static GfxTouchShape g_touch[GFX_TOUCH_MAX];
static int g_ntouch;
static pthread_mutex_t g_touch_lock = PTHREAD_MUTEX_INITIALIZER; /* set from the main thread, drawn on the render thread */
static VkShaderModule g_touch_vs, g_touch_fs;
static VkPipelineLayout g_touch_playout;
static VkPipeline g_touch_pipe;
static VkRenderPass g_touch_rp; /* the present pass g_touch_pipe was made for */
static G9Tex* g_labels;
static int g_touch_failed;

/* the push constants: the shaders' block U */
typedef struct
{
    float rect[4], cell[4];
    uint32_t kind, sym, pressed;
    float alpha;
    float size[2];
} TouchU;

enum
{
    LABEL_W = 256,
    LABEL_H = 96,
    LABEL_COLS = 4,
    LABEL_ROWS = 4,
};
static const char* const LABELS[] = { "L1", "L2", "R1", "R2", "SELECT", "START", "RESET", "DONE" };

/* The stroke font: each letter on a 4 x 6 grid (y down), polylines of points, a pen lift as -1 and the
 * end as -2. Only
 * the letters the labels use; lower case is drawn as upper. */
typedef struct
{
    char ch;
    float pts[40];
} Glyph;
#define UP -1, -1
#define END -2, -2
static const Glyph GLYPHS[] = {
    { 'A', { 0, 6, 2, 0, 4, 6, UP, 0.7f, 4, 3.3f, 4, END } },
    { 'C', { 4, 1, 3, 0, 1, 0, 0, 1, 0, 5, 1, 6, 3, 6, 4, 5, END } },
    { 'D', { 0, 0, 2, 0, 3.4f, 0.6f, 4, 2, 4, 4, 3.4f, 5.4f, 2, 6, 0, 6, 0, 0, END } },
    { 'E', { 4, 0, 0, 0, 0, 6, 4, 6, UP, 0, 3, 3, 3, END } },
    { 'L', { 0, 0, 0, 6, 4, 6, END } },
    { 'N', { 0, 6, 0, 0, 4, 6, 4, 0, END } },
    { 'O', { 1, 0, 3, 0, 4, 1, 4, 5, 3, 6, 1, 6, 0, 5, 0, 1, 1, 0, END } },
    { 'R', { 0, 6, 0, 0, 3, 0, 4, 0.8f, 4, 2.2f, 3, 3, 0, 3, UP, 2, 3, 4, 6, END } },
    { 'S', { 4, 0.8f, 3, 0, 1, 0, 0, 0.8f, 0, 2.2f, 1, 3, 3, 3, 4, 3.8f, 4, 5.2f, 3, 6, 1, 6, 0, 5.2f, END } },
    { 'T', { 0, 0, 4, 0, UP, 2, 0, 2, 6, END } },
    { '1', { 1, 1, 2, 0, 2, 6, UP, 1, 6, 3, 6, END } },
    { '2', { 0, 1, 1, 0, 3, 0, 4, 1, 4, 2.3f, 0, 6, 4, 6, END } },
};
#undef UP
#undef END

static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float l = vx * vx + vy * vy, t = l > 0 ? (wx * vx + wy * vy) / l : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float dx = wx - vx * t, dy = wy - vy * t;
    return sqrtf(dx * dx + dy * dy);
}

/* one label, white on black, centred in its cell of the atlas (BGRA8, every channel the ink) */
static void label_draw(uint8_t* atlas, int cell, const char* text)
{
    const float ADV = 5.6f, HALF = 0.62f; /* a letter's advance and the stroke's half width, in grid units */
    int n = (int)strlen(text);
    float tw = n * ADV - (ADV - 4), th = 6;
    float k = fminf((LABEL_W - 16) / tw, (LABEL_H - 20) / th); /* pixels a grid unit */
    float ox = (LABEL_W - tw * k) * 0.5f, oy = (LABEL_H - th * k) * 0.5f;
    int cx = (cell % LABEL_COLS) * LABEL_W, cy = (cell / LABEL_COLS) * LABEL_H, pitch = LABEL_W * LABEL_COLS * 4;
    for (int y = 0; y < LABEL_H; ++y)
        for (int x = 0; x < LABEL_W; ++x)
        {
            float gx = (x + 0.5f - ox) / k, gy = (y + 0.5f - oy) / k, best = 1e9f;
            for (int i = 0; i < n; ++i)
            {
                char ch = text[i] >= 'a' && text[i] <= 'z' ? (char)(text[i] - 32) : text[i];
                const Glyph* g = NULL;
                for (size_t j = 0; j < sizeof GLYPHS / sizeof *GLYPHS; ++j)
                    if (GLYPHS[j].ch == ch)
                        g = &GLYPHS[j];
                if (!g)
                    continue;
                float lx = gx - i * ADV;
                if (lx < -2 || lx > 6)
                    continue;
                float ax = 0, ay = 0;
                int pen = 0; /* a previous point to draw from */
                for (int p = 0; p + 1 < 40 && g->pts[p] != -2; p += 2)
                {
                    float bx = g->pts[p], by = g->pts[p + 1];
                    if (bx < 0)
                    {
                        pen = 0;
                        continue;
                    }
                    if (pen)
                    {
                        float d = seg_dist(lx, gy, ax, ay, bx, by);
                        if (d < best)
                            best = d;
                    }
                    ax = bx, ay = by, pen = 1;
                }
            }
            float ink = (HALF - best) * k + 0.5f;
            uint8_t v = (uint8_t)(ink <= 0 ? 0 : ink >= 1 ? 255 : ink * 255);
            uint8_t* px = atlas + (size_t)(cy + y) * pitch + (size_t)(cx + x) * 4;
            px[0] = px[1] = px[2] = px[3] = v;
        }
}

static int label_cell(const char* text)
{
    for (size_t i = 0; i < sizeof LABELS / sizeof *LABELS; ++i)
        if (!strcasecmp(LABELS[i], text))
            return (int)i;
    return -1;
}

/* the shaders, the layout and the atlas, once; the pipeline for the present pass it is drawn in.
 * Outside a render pass (the atlas is uploaded). */
static int touch_ready(VkRenderPass rp)
{
    if (g_touch_failed)
        return 0;
    if (!g_touch_vs)
    {
        g_touch_vs = util_module(TOUCH_VS, 0);
        g_touch_fs = util_module(TOUCH_FS, 1);
        VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TouchU) };
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &g_util_layout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(g_dev, &pli, NULL, &g_touch_playout);
        uint32_t aw = LABEL_W * LABEL_COLS, ah = LABEL_H * LABEL_ROWS;
        uint8_t* atlas = (uint8_t*)calloc((size_t)aw * ah, 4);
        g_labels = g9_tex_create(G9_TEX_2D, 21 /* D3DFMT_A8R8G8B8 */, aw, ah, 1, 1, G9_USE_SAMPLE);
        if (!g_touch_vs || !g_touch_fs || !g_touch_playout || !g_labels || !atlas)
        {
            rt_log("[gfx9] the touch controls could not be set up\n");
            free(atlas);
            g_touch_failed = 1;
            return 0;
        }
        for (size_t i = 0; i < sizeof LABELS / sizeof *LABELS; ++i)
            label_draw(atlas, (int)i, LABELS[i]);
        g9_tex_upload(g_labels, 0, 0, 0, 0, 0, aw, ah, 1, atlas, aw * 4, 0);
        free(atlas);
    }
    if (g_touch_rp != rp || !g_touch_pipe)
    {
        if (g_touch_pipe)
            vkDestroyPipeline(g_dev, g_touch_pipe, NULL);
        VkPipelineColorBlendAttachmentState ca = { 0 };
        ca.blendEnable = VK_TRUE;
        ca.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, ca.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ca.colorBlendOp = VK_BLEND_OP_ADD;
        ca.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, ca.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ca.alphaBlendOp = VK_BLEND_OP_ADD;
        ca.colorWriteMask = 15;
        g_touch_pipe = util_pipeline_ex(rp, g_touch_vs, g_touch_fs, g_touch_playout, &ca);
        g_touch_rp = rp;
        if (!g_touch_pipe)
        {
            rt_log("[gfx9] the touch controls' pipeline could not be made\n");
            g_touch_failed = 1;
            return 0;
        }
    }
    tex_prepare(g_labels);
    return 1;
}

static int touch_count(void)
{
    pthread_mutex_lock(&g_touch_lock);
    int n = g_ntouch;
    pthread_mutex_unlock(&g_touch_lock);
    return n;
}

/* the shapes over the swapchain image (w x h pixels), the window's points scaled by k pixels a point;
 * in the present pass, after the frame */
static void draw_touch(VkCommandBuffer cb, uint32_t w, uint32_t h, float k)
{
    GfxTouchShape shapes[GFX_TOUCH_MAX];
    pthread_mutex_lock(&g_touch_lock);
    int n = g_ntouch;
    memcpy(shapes, g_touch, sizeof *shapes * (size_t)n);
    pthread_mutex_unlock(&g_touch_lock);
    if (!n || !g_touch_pipe || !g_labels)
        return;
    VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_touch_pipe);
    VkDescriptorSet ds = dset_alloc(g_util_layout);
    VkDescriptorImageInfo ii = { g_linear, g_labels->view, g_labels->rest };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = ds;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(g_dev, 1, &wr, 0, NULL);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_touch_playout, 0, 1, &ds, 0, NULL);
    for (int i = 0; i < n; ++i)
    {
        const GfxTouchShape* t = &shapes[i];
        TouchU u;
        memset(&u, 0, sizeof u);
        float hw = t->hw * k, hh = (t->kind == GFX_TOUCH_PILL ? t->hh : t->hw) * k;
        u.rect[0] = t->x * k - hw / 0.86f, u.rect[1] = t->y * k - hh / 0.86f; /* the body 0.86 of the quad */
        u.rect[2] = 2 * hw / 0.86f, u.rect[3] = 2 * hh / 0.86f;
        u.kind = t->kind == GFX_TOUCH_PILL ? 2u : t->kind == GFX_TOUCH_RING ? 3u : t->kind == GFX_TOUCH_KNOB ? 4u
               : t->kind == GFX_TOUCH_BACK ? 0u : 1u;
        u.sym = (uint32_t)t->sym, u.pressed = (uint32_t)t->pressed, u.alpha = t->alpha; /* bit 0 held, bit 1 high contrast */
        int cell = t->label[0] && !t->sym ? label_cell(t->label) : -1;
        if (cell >= 0)
        {
            u.cell[0] = (float)(cell % LABEL_COLS) / LABEL_COLS, u.cell[1] = (float)(cell / LABEL_COLS) / LABEL_ROWS;
            u.cell[2] = 1.0f / LABEL_COLS, u.cell[3] = 1.0f / LABEL_ROWS;
        }
        u.size[0] = (float)w, u.size[1] = (float)h;
        vkCmdPushConstants(cb, g_touch_playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof u, &u);
        vkCmdDraw(cb, 4, 1, 0, 0);
    }
}

void gfx_set_touch_shapes(const GfxTouchShape* shapes, int n)
{
    if (n > GFX_TOUCH_MAX)
        n = GFX_TOUCH_MAX;
    pthread_mutex_lock(&g_touch_lock);
    memcpy(g_touch, shapes, sizeof *shapes * (size_t)(n > 0 ? n : 0));
    g_ntouch = n > 0 ? n : 0;
    pthread_mutex_unlock(&g_touch_lock);
}
