/* iOS's touch controls (tests/ios_touch.c, runtime/portable/touch.h): part of gfx9_metal.m, drawn
 * over each frame after present. From xi-on-anything's gfx_metal.m; the shapes come in the window's
 * points and are drawn in the drawable's pixels. */
#include <CoreText/CoreText.h>
#include <pthread.h>
#include "touch.h"

static const char TOUCH_MSL[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct TU { float4 rect; float4 cell; uint kind, sym, pressed; float alpha; };\n"
    "struct TO { float4 pos [[position]]; };\n"
    "vertex TO touch_vs(uint vid [[vertex_id]], constant TU& u [[buffer(0)]], constant float2& size [[buffer(1)]]) {\n"
    "  float2 c = u.rect.xy + float2((vid & 1) ? u.rect.z : 0.0, (vid & 2) ? u.rect.w : 0.0);\n"
    "  TO o; o.pos = float4(c.x / size.x * 2.0 - 1.0, 1.0 - c.y / size.y * 2.0, 0, 1); return o;\n"
    "}\n"
    "static float seg(float2 p, float2 a, float2 b) {\n"
    "  float2 pa = p - a, ba = b - a; return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0));\n"
    "}\n"
    "static float tri(float2 p, float r) {\n" /* an equilateral triangle, apex up (y up): inside < 0 */
    "  const float k = sqrt(3.0); p.x = abs(p.x) - r; p.y = p.y + r / k;\n"
    "  if (p.x + k * p.y > 0.0) p = float2(p.x - k * p.y, -k * p.x - p.y) / 2.0;\n"
    "  p.x -= clamp(p.x, -2.0 * r, 0.0); return -length(p) * sign(p.y);\n"
    "}\n"
    "static float body_sd(uint kind, float2 d, float2 hf, float r) {\n" /* pixels from the body's edge */
    "  if (kind == 2u) { float2 q = abs(d) - float2(max(hf.x * 0.86 - r, 0.0), 0.0); return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r; }\n"
    "  return length(d) - r;\n"
    "}\n"
    "fragment float4 touch_fs(TO in [[stage_in]], constant TU& u [[buffer(0)]], texture2d<float> labels [[texture(0)]],\n"
    "                         sampler s [[sampler(0)]]) {\n"
    "  float2 hf = u.rect.zw * 0.5, c = u.rect.xy + hf, d = in.pos.xy - c;\n"
    "  float r = min(hf.x, hf.y) * 0.86, sd = body_sd(u.kind, d, hf, r);\n"
    "  float body = 1.0 - smoothstep(-1.5, 0.0, sd);\n"
    "  float shadow = (1.0 - smoothstep(0.0, r * 0.2, body_sd(u.kind, d - float2(0.0, r * 0.06), hf, r))) * 0.3;\n"
    "  float rim = body * (1.0 - smoothstep(0.5, 2.0, abs(sd + 1.25)));\n"
    "  float2 p = d / r; float aa = 1.5 / r;\n"
    "  float ink = 0.0; float3 inkc = float3(1.0);\n"
    "  if (u.kind == 0u) {\n"
    "    float2 q = float2(p.x + 0.06, abs(p.y));\n"
    "    ink = 1.0 - smoothstep(0.065 - aa, 0.065 + aa, seg(q, float2(-0.2, 0.0), float2(0.14, 0.34)));\n"
    "  } else if (u.sym != 0u) {\n"
    "    float g;\n"
    "    if (u.sym == 1u) { g = min(seg(p, float2(-0.32, -0.32), float2(0.32, 0.32)), seg(p, float2(-0.32, 0.32), float2(0.32, -0.32))) - 0.075; inkc = float3(0.49, 0.70, 0.95); }\n"
    "    else if (u.sym == 2u) { g = abs(length(p) - 0.34) - 0.075; inkc = float3(1.0, 0.45, 0.45); }\n"
    "    else if (u.sym == 3u) { g = abs(max(abs(p.x), abs(p.y)) - 0.29) - 0.07; inkc = float3(0.93, 0.62, 0.86); }\n"
    "    else if (u.sym == 4u) { g = abs(tri(float2(p.x, -p.y - 0.06), 0.36)) - 0.07; inkc = float3(0.30, 0.90, 0.66); }\n"
    "    else if (u.sym == 9u) {\n" /* a gamepad's outline, filled: body and grips, the D-pad and buttons cut out */
    "      float2 b = abs(p - float2(0.0, -0.02)) - float2(0.30, 0.10);\n"
    "      float body = min(length(max(b, 0.0)) + min(max(b.x, b.y), 0.0) - 0.12, length(float2(abs(p.x) - 0.33, p.y - 0.12)) - 0.17);\n"
    "      float2 dp = abs(p - float2(-0.27, -0.02));\n"
    "      float plus = min(max(dp.x - 0.10, dp.y - 0.032), max(dp.x - 0.032, dp.y - 0.10));\n"
    "      float dots = min(length(p - float2(0.24, -0.07)), length(p - float2(0.33, 0.02))) - 0.045;\n"
    "      g = max(body, -min(plus, dots));\n"
    "    }\n"
    "    else if (u.sym == 10u || u.sym == 11u) {\n" /* a padlock: body, shackle (open: lifted, to the right) */
    "      float2 b = abs(p - float2(0.0, 0.14)) - float2(0.28, 0.2);\n"
    "      float body = length(max(b, 0.0)) + min(max(b.x, b.y), 0.0) - 0.05;\n"
    "      float2 q = p - (u.sym == 10u ? float2(0.0, -0.1) : float2(0.12, -0.22));\n"
    "      float arc = q.y < 0.0 ? abs(length(q) - 0.18) - 0.06 : max(abs(abs(q.x) - 0.18) - 0.06, q.y - 0.14);\n"
    "      g = min(body, arc);\n"
    "    }\n"
    "    else {\n"
    "      float2 q = u.sym == 5u ? float2(p.x, -p.y) : u.sym == 6u ? p : u.sym == 7u ? float2(p.y, -p.x) : float2(p.y, p.x);\n"
    "      g = tri(q, 0.3);\n"
    "    }\n"
    "    ink = 1.0 - smoothstep(-aa, aa, g);\n"
    "  } else if (u.cell.z > 0.0) {\n"
    "    float lw = u.kind == 2u ? hf.x * 1.5 : r * 1.7, lh = lw * 0.375;\n" /* the atlas's cells are 8:3 */
    "    float2 uv = (d + float2(lw, lh) * 0.5) / float2(lw, lh);\n"
    "    if (all(uv >= 0.0) && all(uv <= 1.0)) ink = labels.sample(s, u.cell.xy + uv * u.cell.zw).r;\n"
    "  }\n"
    "  float top = saturate(p.y * 0.5 + 0.5);\n"
    "  float3 glass = mix(float3(0.30, 0.30, 0.33), float3(0.11, 0.11, 0.13), top);\n"
    "  float fill = 0.6;\n"
    "  if (u.pressed != 0u) glass = mix(glass, float3(0.75), 0.45), fill = 0.8;\n"
    "  if (u.kind == 3u) { glass = float3(0.1); fill = 0.22; rim *= 1.6; }\n" /* the stick's base: faint, a stronger rim */
    "  if (u.kind == 4u) { glass = mix(float3(0.85), float3(0.6), top); fill = 0.85; }\n"
    "  float3 col = mix(mix(glass, float3(1.0), rim * 0.35), inkc, ink);\n"
    "  float a = max(body * mix(fill, 0.97, ink), shadow * (1.0 - body)) * u.alpha;\n"
    "  return float4(body > 0.0 ? col : float3(0.0), a);\n"
    "}\n"
;

static GfxTouchShape g_touch[GFX_TOUCH_MAX];
static int g_ntouch;
static pthread_mutex_t g_touch_lock = PTHREAD_MUTEX_INITIALIZER; /* set from the main thread, drawn on the render thread */
static id<MTLRenderPipelineState> g_touch_pipe;
static id<MTLSamplerState> g_touch_samp;

/* The touch controls' labels: each text drawn once with the system font (bold), white on black, into a
 * cell of an 8-bit atlas */
enum
{
    LABEL_W = 256,
    LABEL_H = 96,
    LABEL_COLS = 4,
    LABEL_ROWS = 4,
};
static id<MTLTexture> g_labels;
static char g_label_text[LABEL_COLS * LABEL_ROWS][8];
static int g_nlabels;

static int label_cell(const char* text)
{
    for (int i = 0; i < g_nlabels; ++i)
        if (!strcmp(g_label_text[i], text))
            return i;
    if (g_nlabels == LABEL_COLS * LABEL_ROWS || !g_labels)
        return -1;
    uint8_t* px = (uint8_t*)calloc(LABEL_W * LABEL_H, 1);
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(px, LABEL_W, LABEL_H, 8, LABEL_W, gray, (CGBitmapInfo)kCGImageAlphaNone);
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, 64.0, NULL);
    CGFloat white_c[2] = { 1.0, 1.0 };
    CGColorRef white = CGColorCreate(gray, white_c);
    CFStringRef str = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    const void* keys[] = { kCTFontAttributeName, kCTForegroundColorAttributeName };
    const void* vals[] = { font, white };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CGRect b = CTLineGetImageBounds(line, ctx);
    double k = fmin(1.0, fmin((LABEL_W - 16) / fmax(b.size.width, 1), (LABEL_H - 16) / fmax(b.size.height, 1)));
    CGContextTranslateCTM(ctx, LABEL_W * 0.5, LABEL_H * 0.5);
    CGContextScaleCTM(ctx, k, k);
    CGContextSetTextPosition(ctx, -b.origin.x - b.size.width * 0.5, -b.origin.y - b.size.height * 0.5);
    CTLineDraw(line, ctx);
    int i = g_nlabels++;
    snprintf(g_label_text[i], sizeof g_label_text[i], "%s", text);
    [g_labels replaceRegion:MTLRegionMake2D((NSUInteger)(i % LABEL_COLS) * LABEL_W, (NSUInteger)(i / LABEL_COLS) * LABEL_H, LABEL_W, LABEL_H)
                mipmapLevel:0 withBytes:px bytesPerRow:LABEL_W];
    CFRelease(line), CFRelease(as), CFRelease(attrs), CFRelease(str), CGColorRelease(white), CFRelease(font);
    CGContextRelease(ctx), CGColorSpaceRelease(gray);
    free(px);
    return i;
}

static void touch_init(void)
{
    NSError* err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(TOUCH_MSL) options:nil error:&err];
    if (!lib)
    {
        rt_log("[gfx9] touch controls' shaders: %s\n", err.localizedDescription.UTF8String);
        return;
    }
    MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor new] autorelease];
    pd.vertexFunction = [[lib newFunctionWithName:@"touch_vs"] autorelease];
    pd.fragmentFunction = [[lib newFunctionWithName:@"touch_fs"] autorelease];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pd.colorAttachments[0].blendingEnabled = YES;
    pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    g_touch_pipe = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!g_touch_pipe)
        rt_log("[gfx9] touch controls' pipeline: %s\n", err.localizedDescription.UTF8String);
    MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor new] autorelease];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    g_touch_samp = [g_dev newSamplerStateWithDescriptor:sd];
    [lib release];
}

/* the shapes over the drawable (w x h pixels), the window's points scaled by k pixels a point */
static void draw_touch(id<MTLRenderCommandEncoder> e, NSUInteger w, NSUInteger h, float k)
{
    GfxTouchShape shapes[GFX_TOUCH_MAX];
    pthread_mutex_lock(&g_touch_lock);
    int n = g_ntouch;
    memcpy(shapes, g_touch, sizeof *shapes * (size_t)n);
    pthread_mutex_unlock(&g_touch_lock);
    if (!n || !g_touch_pipe)
        return;
    if (!g_labels)
    {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
            width:LABEL_W * LABEL_COLS height:LABEL_H * LABEL_ROWS mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        g_labels = [g_dev newTextureWithDescriptor:d];
    }
    float size[2] = { (float)w, (float)h };
    [e setViewport:(MTLViewport){ 0, 0, (double)w, (double)h, 0, 1 }];
    [e setRenderPipelineState:g_touch_pipe];
    [e setVertexBytes:size length:sizeof size atIndex:1];
    [e setFragmentTexture:g_labels atIndex:0];
    [e setFragmentSamplerState:g_touch_samp atIndex:0];
    for (int i = 0; i < n; ++i)
    {
        const GfxTouchShape* t = &shapes[i];
        struct
        {
            float rect[4], cell[4];
            uint32_t kind, sym, pressed;
            float alpha;
        } u;
        memset(&u, 0, sizeof u);
        float hw = t->hw * k, hh = (t->kind == GFX_TOUCH_PILL ? t->hh : t->hw) * k;
        u.rect[0] = t->x * k - hw / 0.86f, u.rect[1] = t->y * k - hh / 0.86f; /* the body 0.86 of the quad */
        u.rect[2] = 2 * hw / 0.86f, u.rect[3] = 2 * hh / 0.86f;
        u.kind = t->kind == GFX_TOUCH_PILL ? 2u : t->kind == GFX_TOUCH_RING ? 3u : t->kind == GFX_TOUCH_KNOB ? 4u
               : t->kind == GFX_TOUCH_BACK ? 0u : 1u;
        u.sym = (uint32_t)t->sym, u.pressed = t->pressed != 0, u.alpha = t->alpha;
        int cell = t->label[0] && !t->sym ? label_cell(t->label) : -1;
        if (cell >= 0)
        {
            u.cell[0] = (float)(cell % LABEL_COLS) / LABEL_COLS, u.cell[1] = (float)(cell / LABEL_COLS) / LABEL_ROWS;
            u.cell[2] = 1.0f / LABEL_COLS, u.cell[3] = 1.0f / LABEL_ROWS;
        }
        [e setVertexBytes:&u length:sizeof u atIndex:0];
        [e setFragmentBytes:&u length:sizeof u atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
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
