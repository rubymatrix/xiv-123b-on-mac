/* The Direct3D 9 back end on Vulkan 1.1 (gfx9.h): MoltenVK on the Mac, the system driver on Android.
 * It does what gfx9_metal.m does, which is the reference for every behaviour here; the shaders come
 * from sm_glsl.c (sm_glsl.h has the interface) and are compiled to SPIR-V with glslang at run time.
 *
 * Frames: up to FRAMES command buffers in flight, each with a fence, an upload ring (vertex and index
 * data the draws copy, the constants, texture uploads' staging), descriptor pools that are reset when
 * the frame comes round again, and a list of objects to destroy once its fence has passed. Everything
 * a frame does goes into its one command buffer in call order: uploads and copies end the render pass,
 * so the GPU sees them where the game made them. Images rest between uses in one layout (colour
 * images SHADER_READ_ONLY_OPTIMAL, depth DEPTH_STENCIL_READ_ONLY_OPTIMAL), and every transfer and
 * render pass moves them out of it and back with a full barrier: simple, and the order D3D9 promises.
 *
 * Render passes begin lazily at the first draw or clear after the targets change; a clear at the
 * start of a pass that covers the whole target becomes the pass's load operation, any other is
 * vkCmdClearAttachments over its rectangles.
 *
 * Pipelines are cached by everything Vulkan bakes into one: the two shaders (each translated per
 * variant, as on Metal), the vertex layout, the target formats, blending, the depth and stencil
 * tests, culling and the fill mode. A draw of the 3D scene compiles what it lacks on worker threads
 * and is skipped until that is ready, every other draw waits for its compile, as on Metal. Compiled
 * SPIR-V is kept on disk by the hash of its GLSL, and the driver's pipeline cache too (XIV_VK_CACHE,
 * by default ~/Library/Caches/FFXIV/vulkan on the Mac).
 *
 * The surface: SDL_Vulkan_CreateSurface where SDL is built with Vulkan (Android); on Apple, where the
 * vendored SDL is built without it, MoltenVK's surface on the layer of SDL's Metal view, with the
 * loader linked in (tools/build_xiv.py XIV_GFX=vulkan).
 *
 * Devices without BC sampling (Mali) get DXT1/3/5 and ATI1/2 textures decoded to BGRA8, R8 and RG8
 * when they are uploaded (XIV_VK_NOBC=1 forces that anywhere, to try it). XIV_VK_VALIDATE=1 turns
 * the validation layer on. The Metal back end's extras (the scene effects, upscaling, the navmesh
 * overlay, the FPS counter and frame pacing) are not here.
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#if defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#include <dlfcn.h>
#endif
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include "sdl_main.h"
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gfx9.h"
#include "sm_glsl.h"

void rt_log(const char* fmt, ...);

/* XIV_VK_PROF=1: where the render thread waits, every 600 presents */
static int g_prof;
static uint64_t g_t_gpu, g_n_gpu, g_t_fence, g_t_acquire, g_t_submit, g_t_present, g_t_compile, g_n_submits, g_n_passes, g_n_draws, g_n_barriers;
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define FRAMES 3
#ifdef __ANDROID__
#define RING_SIZE (32u << 20)
#else
#define RING_SIZE (64u << 20)
#endif
#define BLOCK_SIZE (64ull << 20)
#define DESC_SETS_PER_POOL 1024
#define PROF_PASSES 256

/* --- Vulkan entry points ------------------------------------------------------------------------ */

#define VKF_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)
#define VKF_INSTANCE(X)                                                                                          \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                          \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties)                                \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)                             \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                         \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VKF_DEVICE(X)                                                                                            \
    X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR)                     \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit)                    \
    X(vkCreateCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)             \
    X(vkEndCommandBuffer) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore)              \
    X(vkDestroySemaphore) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkCreateBuffer)                   \
    X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView)              \
    X(vkCreateSampler) X(vkCreateShaderModule) X(vkCreatePipelineCache) X(vkGetPipelineCacheData)                \
    X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateDescriptorSetLayout)   \
    X(vkCreateDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)     \
    X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCmdBeginRenderPass)                 \
    X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers)              \
    X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdSetViewport) X(vkCmdSetScissor)              \
    X(vkCmdSetDepthBias) X(vkCmdSetBlendConstants) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask)     \
    X(vkCmdSetStencilReference) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer)      \
    X(vkCmdCopyBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdClearAttachments) X(vkCmdClearColorImage)      \
    X(vkCmdClearDepthStencilImage) X(vkCmdPushConstants) X(vkCreateQueryPool) X(vkCmdResetQueryPool)        \
    X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)

#define VKF_DECL(f) static PFN_##f f;
VKF_GLOBAL(VKF_DECL)
VKF_INSTANCE(VKF_DECL)
VKF_DEVICE(VKF_DECL)
static PFN_vkGetInstanceProcAddr g_gipa;
static PFN_vkCreateDebugUtilsMessengerEXT p_vkCreateDebugUtilsMessengerEXT;

/* --- D3DFORMAT ---------------------------------------------------------------------------------- */

enum
{
    F_A8R8G8B8 = 21, F_X8R8G8B8 = 22, F_R5G6B5 = 23, F_X1R5G5B5 = 24, F_A1R5G5B5 = 25, F_A4R4G4B4 = 26, F_A8 = 28,
    F_X4R4G4B4 = 30, F_A2B10G10R10 = 31, F_A8B8G8R8 = 32, F_X8B8G8R8 = 33, F_G16R16 = 34, F_A2R10G10B10 = 35,
    F_A16B16G16R16 = 36, F_L8 = 50, F_A8L8 = 51, F_A4L4 = 52, F_V8U8 = 60, F_Q8W8V8U8 = 63, F_V16U16 = 64,
    F_D16_LOCKABLE = 70, F_D32 = 71, F_D15S1 = 73, F_D24S8 = 75, F_D24X8 = 77, F_D24X4S4 = 79, F_D16 = 80, F_L16 = 81,
    F_D32F_LOCKABLE = 82, F_D24FS8 = 83, F_R16F = 111, F_G16R16F = 112, F_A16B16G16R16F = 113, F_R32F = 114,
    F_G32R32F = 115, F_A32B32G32R32F = 116,
};
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define F_DXT1 FOURCC('D', 'X', 'T', '1')
#define F_DXT2 FOURCC('D', 'X', 'T', '2')
#define F_DXT3 FOURCC('D', 'X', 'T', '3')
#define F_DXT4 FOURCC('D', 'X', 'T', '4')
#define F_DXT5 FOURCC('D', 'X', 'T', '5')
#define F_ATI1 FOURCC('A', 'T', 'I', '1')
#define F_ATI2 FOURCC('A', 'T', 'I', '2')
#define F_INTZ FOURCC('I', 'N', 'T', 'Z')
#define F_DF24 FOURCC('D', 'F', '2', '4')
#define F_DF16 FOURCC('D', 'F', '1', '6')
#define F_NULL FOURCC('N', 'U', 'L', 'L')

enum { CONV_NONE, CONV_565, CONV_X555, CONV_1555, CONV_4444, CONV_X444, CONV_A4L4, CONV_X8 };
enum { DEC_NONE, DEC_BC1, DEC_BC2, DEC_BC3, DEC_BC4, DEC_BC5 };

typedef struct Fmt
{
    VkFormat vf;
    int conv;        /* the bytes D3D writes are converted on upload (16-bit formats to BGRA8) */
    uint32_t block;  /* bytes per 4x4 block for compressed formats as D3D lays them out, else 0 */
    uint32_t texel;  /* bytes per texel as D3D lays them out */
    uint32_t vtexel; /* ... and as the image holds them */
    int swizzle;     /* 0 none, 1 L (rrr1), 2 AL (rrrg), 3 A (000r) */
    int depth;       /* 1 depth(-stencil) */
    int depth_read;  /* sampled for its values (INTZ, DF24), not compared */
    int stencil;     /* the image has a stencil aspect */
    int decode;      /* DEC_*: a block format decoded on upload (no BC sampling on the device) */
} Fmt;

static int g_bc = 1; /* the device samples BC formats */
static VkFormat g_ds_format = VK_FORMAT_D32_SFLOAT_S8_UINT, g_d_format = VK_FORMAT_D32_SFLOAT;

static Fmt fmt_of(uint32_t f)
{
    Fmt r = { VK_FORMAT_B8G8R8A8_UNORM, CONV_NONE, 0, 4, 4, 0, 0, 0, 0, 0 };
    switch (f)
    {
    case F_A8R8G8B8: case F_X8R8G8B8: break;
    case F_A8B8G8R8: case F_X8B8G8R8: r.vf = VK_FORMAT_R8G8B8A8_UNORM; break;
    case F_R5G6B5: r.conv = CONV_565, r.texel = 2; break;
    case F_X1R5G5B5: r.conv = CONV_X555, r.texel = 2; break;
    case F_A1R5G5B5: r.conv = CONV_1555, r.texel = 2; break;
    case F_A4R4G4B4: r.conv = CONV_4444, r.texel = 2; break;
    case F_X4R4G4B4: r.conv = CONV_X444, r.texel = 2; break;
    case F_A8: r.vf = VK_FORMAT_R8_UNORM, r.texel = r.vtexel = 1, r.swizzle = 3; break;
    case F_L8: r.vf = VK_FORMAT_R8_UNORM, r.texel = r.vtexel = 1, r.swizzle = 1; break;
    case F_A8L8: r.vf = VK_FORMAT_R8G8_UNORM, r.texel = r.vtexel = 2, r.swizzle = 2; break;
    case F_A4L4: r.conv = CONV_A4L4, r.vf = VK_FORMAT_R8G8_UNORM, r.texel = 1, r.vtexel = 2, r.swizzle = 2; break;
    case F_L16: r.vf = VK_FORMAT_R16_UNORM, r.texel = r.vtexel = 2, r.swizzle = 1; break;
    case F_V8U8: r.vf = VK_FORMAT_R8G8_SNORM, r.texel = r.vtexel = 2; break;
    case F_Q8W8V8U8: r.vf = VK_FORMAT_R8G8B8A8_SNORM; break;
    case F_V16U16: r.vf = VK_FORMAT_R16G16_SNORM; break;
    case F_G16R16: r.vf = VK_FORMAT_R16G16_UNORM; break;
    case F_A16B16G16R16: r.vf = VK_FORMAT_R16G16B16A16_UNORM, r.texel = r.vtexel = 8; break;
    case F_A2B10G10R10: r.vf = VK_FORMAT_A2B10G10R10_UNORM_PACK32; break;
    case F_A2R10G10B10: r.vf = VK_FORMAT_A2R10G10B10_UNORM_PACK32; break;
    case F_R16F: r.vf = VK_FORMAT_R16_SFLOAT, r.texel = r.vtexel = 2; break;
    case F_G16R16F: r.vf = VK_FORMAT_R16G16_SFLOAT; break;
    case F_A16B16G16R16F: r.vf = VK_FORMAT_R16G16B16A16_SFLOAT, r.texel = r.vtexel = 8; break;
    case F_R32F: r.vf = VK_FORMAT_R32_SFLOAT; break;
    case F_G32R32F: r.vf = VK_FORMAT_R32G32_SFLOAT, r.texel = r.vtexel = 8; break;
    case F_A32B32G32R32F: r.vf = VK_FORMAT_R32G32B32A32_SFLOAT, r.texel = r.vtexel = 16; break;
    case F_DXT1: r.vf = VK_FORMAT_BC1_RGBA_UNORM_BLOCK, r.block = 8, r.decode = DEC_BC1; break;
    case F_DXT2: case F_DXT3: r.vf = VK_FORMAT_BC2_UNORM_BLOCK, r.block = 16, r.decode = DEC_BC2; break;
    case F_DXT4: case F_DXT5: r.vf = VK_FORMAT_BC3_UNORM_BLOCK, r.block = 16, r.decode = DEC_BC3; break;
    case F_ATI1: r.vf = VK_FORMAT_BC4_UNORM_BLOCK, r.block = 8, r.decode = DEC_BC4; break;
    case F_ATI2: r.vf = VK_FORMAT_BC5_UNORM_BLOCK, r.block = 16, r.decode = DEC_BC5; break;
    case F_D16: case F_D16_LOCKABLE: case F_D32: case F_D32F_LOCKABLE: case F_D24X8: r.vf = g_d_format, r.depth = 1; break;
    case F_D24S8: case F_D24FS8: case F_D24X4S4: case F_D15S1: r.vf = g_ds_format, r.depth = r.stencil = 1; break;
    case F_INTZ: r.vf = g_ds_format, r.depth = r.stencil = 1, r.depth_read = 1; break;
    case F_DF24: case F_DF16: r.vf = g_d_format, r.depth = 1, r.depth_read = 1; break;
    case F_NULL: break;
    default: break;
    }
    if (r.block && g_bc)
        r.decode = DEC_NONE;
    if (r.decode)
    {
        /* no BC sampling: decoded when uploaded */
        r.vf = r.decode == DEC_BC4 ? VK_FORMAT_R8_UNORM : r.decode == DEC_BC5 ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
        r.vtexel = r.decode == DEC_BC4 ? 1 : r.decode == DEC_BC5 ? 2 : 4;
    }
    return r;
}

static int format_listed(uint32_t f, int use);
static VkFormatFeatureFlags fmt_features(VkFormat f);
static VkPhysicalDevice g_phys;

/* D3D9's answer as on Metal, then, once there is a device, whether it can render to or sample the
 * format as the back end holds it (a Mali may lack some 16-bit render targets) */
int g9_format_ok(uint32_t f, int use)
{
    if (!format_listed(f, use))
        return 0;
    if (!g_phys || f == F_NULL)
        return 1;
    Fmt m = fmt_of(f);
    VkFormatFeatureFlags ff = fmt_features(m.vf);
    if (use == G9_USE_RT)
        return (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    if (use == G9_USE_DEPTH)
        return (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    return (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

static int format_listed(uint32_t f, int use)
{
    switch (f)
    {
    case F_A8R8G8B8: case F_X8R8G8B8: case F_A8B8G8R8: case F_X8B8G8R8: case F_R5G6B5: case F_X1R5G5B5:
    case F_A1R5G5B5: case F_A4R4G4B4: case F_A2B10G10R10: case F_A2R10G10B10: case F_G16R16: case F_A16B16G16R16:
    case F_R16F: case F_G16R16F: case F_A16B16G16R16F: case F_R32F: case F_G32R32F: case F_A32B32G32R32F:
        return use != G9_USE_DEPTH;
    case F_A8: case F_L8: case F_A8L8: case F_A4L4: case F_L16: case F_V8U8: case F_Q8W8V8U8: case F_V16U16:
    case F_DXT1: case F_DXT2: case F_DXT3: case F_DXT4: case F_DXT5: case F_ATI1: case F_ATI2: case F_X4R4G4B4:
        return use == G9_USE_SAMPLE;
    case F_D16: case F_D16_LOCKABLE: case F_D32: case F_D24X8: case F_D24S8: case F_D24FS8: case F_D32F_LOCKABLE:
        return use == G9_USE_DEPTH || use == G9_USE_SAMPLE;
    case F_INTZ: case F_DF24: case F_DF16:
        return use == G9_USE_DEPTH || use == G9_USE_SAMPLE;
    case F_NULL:
        return use == G9_USE_RT;
    default:
        return 0;
    }
}

/* --- the device --------------------------------------------------------------------------------- */

static VkInstance g_inst;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qfam;
static VkSurfaceKHR g_surface;
static VkPhysicalDeviceProperties g_props;
static VkPhysicalDeviceFeatures g_feat; /* what was enabled */
static VkPhysicalDeviceMemoryProperties g_memprops;
static int g_mirror_once, g_float_controls, g_format_list, g_portability;
static uint32_t g_ubo_align = 256, g_stride_align = 1;
static void* g_sdl_window;
static int g_vsync;

static VkFormatFeatureFlags fmt_features(VkFormat f)
{
    static VkFormatFeatureFlags cache[256];
    static uint8_t known[256];
    if (f < 256 && known[f])
        return cache[f];
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_phys, f, &p);
    if (f < 256)
        cache[f] = p.optimalTilingFeatures, known[f] = 1;
    return p.optimalTilingFeatures;
}

static VkFormatFeatureFlags buf_features(VkFormat f)
{
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_phys, f, &p);
    return p.bufferFeatures;
}

/* --- memory: blocks per memory type, carved first fit ------------------------------------------- */

typedef struct Range
{
    VkDeviceSize off, size;
} Range;

typedef struct Block
{
    VkDeviceMemory mem;
    VkDeviceSize size;
    uint8_t* map;
    uint32_t type;
    int kind; /* 0 images (optimal tiling), 1 buffers: kept apart, so bufferImageGranularity never applies */
    int dedicated;
    Range* fr; /* free ranges, by offset */
    uint32_t nfr, capfr;
    struct Block* next;
} Block;

typedef struct Alloc
{
    Block* b;
    VkDeviceSize off, size;
} Alloc;

static Block* g_blocks;
static pthread_mutex_t g_mem_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_mem_used;

static int mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < g_memprops.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g_memprops.memoryTypes[i].propertyFlags & want) == want &&
            !(g_memprops.memoryTypes[i].propertyFlags & avoid))
            return (int)i;
    for (uint32_t i = 0; i < g_memprops.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g_memprops.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static void range_insert(Block* b, VkDeviceSize off, VkDeviceSize size)
{
    uint32_t i = 0;
    while (i < b->nfr && b->fr[i].off < off)
        i++;
    /* merge with the neighbours */
    int prev = i > 0 && b->fr[i - 1].off + b->fr[i - 1].size == off;
    int next = i < b->nfr && off + size == b->fr[i].off;
    if (prev && next)
    {
        b->fr[i - 1].size += size + b->fr[i].size;
        memmove(&b->fr[i], &b->fr[i + 1], (b->nfr - i - 1) * sizeof *b->fr);
        b->nfr--;
        return;
    }
    if (prev)
    {
        b->fr[i - 1].size += size;
        return;
    }
    if (next)
    {
        b->fr[i].off = off, b->fr[i].size += size;
        return;
    }
    if (b->nfr == b->capfr)
    {
        b->capfr = b->capfr ? b->capfr * 2 : 16;
        b->fr = (Range*)realloc(b->fr, b->capfr * sizeof *b->fr);
    }
    memmove(&b->fr[i + 1], &b->fr[i], (b->nfr - i) * sizeof *b->fr);
    b->fr[i] = (Range){ off, size };
    b->nfr++;
}

static int block_take(Block* b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize* out)
{
    for (uint32_t i = 0; i < b->nfr; ++i)
    {
        Range r = b->fr[i];
        VkDeviceSize at = (r.off + align - 1) / align * align;
        if (at + size > r.off + r.size)
            continue;
        /* split: [r.off, at) and [at + size, end) stay free */
        memmove(&b->fr[i], &b->fr[i + 1], (b->nfr - i - 1) * sizeof *b->fr);
        b->nfr--;
        if (at > r.off)
            range_insert(b, r.off, at - r.off);
        if (at + size < r.off + r.size)
            range_insert(b, at + size, r.off + r.size - (at + size));
        *out = at;
        return 1;
    }
    return 0;
}

static int mem_alloc(const VkMemoryRequirements* req, VkMemoryPropertyFlags want, int kind, Alloc* out)
{
    int type = mem_type(req->memoryTypeBits, want, kind == 0 ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : 0);
    if (type < 0 && (want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        type = mem_type(req->memoryTypeBits, want & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type < 0)
        return 0;
    int host = (g_memprops.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    pthread_mutex_lock(&g_mem_lock);
    Block* b = NULL;
    VkDeviceSize off = 0;
    if (req->size < BLOCK_SIZE / 4)
        for (b = g_blocks; b; b = b->next)
            if (!b->dedicated && b->type == (uint32_t)type && b->kind == kind && block_take(b, req->size, req->alignment, &off))
                break;
    if (!b)
    {
        int ded = req->size >= BLOCK_SIZE / 4;
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = ded ? req->size : BLOCK_SIZE;
        ai.memoryTypeIndex = (uint32_t)type;
        VkDeviceMemory mem;
        if (vkAllocateMemory(g_dev, &ai, NULL, &mem) != VK_SUCCESS)
        {
            pthread_mutex_unlock(&g_mem_lock);
            rt_log("[gfx9] out of GPU memory (%llu bytes, %llu MB in use)\n", (unsigned long long)ai.allocationSize,
                (unsigned long long)(g_mem_used >> 20));
            return 0;
        }
        g_mem_used += ai.allocationSize;
        b = (Block*)calloc(1, sizeof *b);
        b->mem = mem, b->size = ai.allocationSize, b->type = (uint32_t)type, b->kind = kind, b->dedicated = ded;
        if (host)
            vkMapMemory(g_dev, mem, 0, VK_WHOLE_SIZE, 0, (void**)&b->map);
        b->next = g_blocks;
        g_blocks = b;
        if (!ded)
        {
            range_insert(b, 0, BLOCK_SIZE);
            block_take(b, req->size, req->alignment, &off);
        }
    }
    pthread_mutex_unlock(&g_mem_lock);
    out->b = b, out->off = off, out->size = req->size;
    return 1;
}

static void mem_free(Alloc* a)
{
    if (!a->b)
        return;
    pthread_mutex_lock(&g_mem_lock);
    Block* b = a->b;
    if (b->dedicated)
    {
        for (Block** p = &g_blocks; *p; p = &(*p)->next)
            if (*p == b)
            {
                *p = b->next;
                break;
            }
        vkFreeMemory(g_dev, b->mem, NULL);
        g_mem_used -= b->size;
        free(b->fr);
        free(b);
    }
    else
        range_insert(b, a->off, a->size);
    pthread_mutex_unlock(&g_mem_lock);
    a->b = NULL;
}

static VkBuffer buffer_new(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, Alloc* a)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    VkBuffer b;
    if (vkCreateBuffer(g_dev, &bi, NULL, &b) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, b, &req);
    if (!mem_alloc(&req, want, 1, a))
    {
        vkDestroyBuffer(g_dev, b, NULL);
        return VK_NULL_HANDLE;
    }
    vkBindBufferMemory(g_dev, b, a->b->mem, a->off);
    return b;
}

/* --- resources ---------------------------------------------------------------------------------- */

struct G9Tex
{
    VkImage img;
    Alloc mem;
    VkImageView view; /* sampled: every level and layer, the format's swizzle; depth: its depth aspect */
    VkImageView srgb; /* a BGRA8 image's sRGB view, made on first use */
    VkImageView* att; /* render target views by level * layers + layer, made on first use */
    int type, use, inited, nolinear;
    uint32_t fmt, w, h, d, levels, layers;
    Fmt f;
    VkImageAspectFlags aspect;
    VkImageLayout rest;
};

struct G9Buf
{
    VkBuffer buf;
    Alloc mem;
    uint32_t size;
};

/* A translated function or a pipeline, ready or still compiling: state 0 compiling, 1 done (the
 * handle VK_NULL_HANDLE if it failed). */
typedef struct Compiled
{
    volatile int state;
    VkShaderModule mod;
    VkPipeline pipe;
} Compiled;

typedef struct Variant
{
    uint64_t key;
    Compiled* fn;
} Variant;

struct G9Shader
{
    uint32_t* tokens;
    uint32_t ntokens;
    SmInfo info;
    Variant* var;
    uint32_t nvar, capvar;
    uint64_t content;
};

/* --- small caches: key bytes -> object ---------------------------------------------------------- */

typedef struct CacheEnt
{
    uint64_t h;
    void* key;
    uint32_t klen;
    void* obj;
} CacheEnt;
typedef struct Cache
{
    CacheEnt* e;
    uint32_t n, cap;
} Cache;

static uint64_t fnv64(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static void* cache_get(Cache* c, const void* key, uint32_t klen, uint64_t* hout)
{
    uint64_t h = fnv64(key, klen);
    *hout = h;
    if (!c->cap)
        return NULL;
    for (uint32_t i = h % c->cap, k = 0; k < c->cap; ++k, i = (i + 1) % c->cap)
    {
        CacheEnt* e = &c->e[i];
        if (!e->key)
            return NULL;
        if (e->h == h && e->klen == klen && !memcmp(e->key, key, klen))
            return e->obj;
    }
    return NULL;
}

static void cache_put(Cache* c, const void* key, uint32_t klen, uint64_t h, void* obj)
{
    if ((c->n + 1) * 2 > c->cap)
    {
        Cache old = *c;
        c->cap = c->cap ? c->cap * 2 : 256;
        c->e = (CacheEnt*)calloc(c->cap, sizeof *c->e);
        c->n = 0;
        for (uint32_t i = 0; i < old.cap; ++i)
            if (old.e[i].key)
            {
                for (uint32_t j = old.e[i].h % c->cap;; j = (j + 1) % c->cap)
                    if (!c->e[j].key)
                    {
                        c->e[j] = old.e[i];
                        c->n++;
                        break;
                    }
            }
        free(old.e);
    }
    for (uint32_t i = h % c->cap;; i = (i + 1) % c->cap)
        if (!c->e[i].key)
        {
            c->e[i].h = h;
            c->e[i].key = malloc(klen);
            memcpy(c->e[i].key, key, klen);
            c->e[i].klen = klen;
            c->e[i].obj = obj;
            c->n++;
            return;
        }
}

static Cache g_pipes, g_samplers, g_rps, g_fns;

/* --- frames, the upload ring and deferred destruction -------------------------------------------- */

typedef struct Trash
{
    int kind; /* 0 texture, 1 buffer, 2 framebuffer, 3 staging buffer */
    void* p;
    uint64_t h;
    Alloc a;
} Trash;

typedef struct Frame
{
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkSemaphore acquired;
    VkBuffer ring;
    Alloc ring_mem;
    uint8_t* ring_map;
    VkDescriptorSet ubo_set;
    VkDescriptorPool* dpools;
    uint32_t ndpools, cur_dpool;
    Trash* trash;
    uint32_t ntrash, captrash;
    VkQueryPool qp; /* XIV_VK_PROF: the command buffer's start and end on the GPU, then (XIV_VK_PROF=2)
                     * each render pass's */
    int qp_used;
    uint32_t npass;
    uint32_t pass_key[PROF_PASSES]; /* the pass's width, height and draws, to group by */
    uint32_t pass_draws[PROF_PASSES];
} Frame;

static Frame g_fr[FRAMES];
static uint32_t g_frame, g_ring_used;
static int g_cmd_open;
static VkCommandBuffer g_cb;

/* destroyed from any thread, handed to the frame being submitted */
static pthread_mutex_t g_trash_lock = PTHREAD_MUTEX_INITIALIZER;
static Trash* g_pending;
static uint32_t g_npending, g_cappending;

static void trash_add(Frame* f, Trash t)
{
    if (f->ntrash == f->captrash)
    {
        f->captrash = f->captrash ? f->captrash * 2 : 64;
        f->trash = (Trash*)realloc(f->trash, f->captrash * sizeof *f->trash);
    }
    f->trash[f->ntrash++] = t;
}

static void pending_add(Trash t)
{
    pthread_mutex_lock(&g_trash_lock);
    if (g_npending == g_cappending)
    {
        g_cappending = g_cappending ? g_cappending * 2 : 64;
        g_pending = (Trash*)realloc(g_pending, g_cappending * sizeof *g_pending);
    }
    g_pending[g_npending++] = t;
    pthread_mutex_unlock(&g_trash_lock);
}

static void tex_free(G9Tex* t);
static void fb_scrub(G9Tex* t);

static void trash_empty(Frame* f)
{
    for (uint32_t i = 0; i < f->ntrash; ++i)
    {
        Trash* t = &f->trash[i];
        switch (t->kind)
        {
        case 0:
            tex_free((G9Tex*)t->p);
            break;
        case 1:
        {
            G9Buf* b = (G9Buf*)t->p;
            vkDestroyBuffer(g_dev, b->buf, NULL);
            mem_free(&b->mem);
            free(b);
            break;
        }
        case 2:
            vkDestroyFramebuffer(g_dev, (VkFramebuffer)(uintptr_t)t->h, NULL);
            break;
        case 3:
            vkDestroyBuffer(g_dev, (VkBuffer)(uintptr_t)t->h, NULL);
            mem_free(&t->a);
            break;
        }
    }
    f->ntrash = 0;
}

/* the textures and buffers destroyed since the last submission go with this one */
static void pending_take(Frame* f)
{
    pthread_mutex_lock(&g_trash_lock);
    Trash* p = g_pending;
    uint32_t n = g_npending;
    g_pending = NULL, g_npending = g_cappending = 0;
    pthread_mutex_unlock(&g_trash_lock);
    for (uint32_t i = 0; i < n; ++i)
    {
        if (p[i].kind == 0)
            fb_scrub((G9Tex*)p[i].p);
        trash_add(f, p[i]);
    }
    free(p);
}

/* XIV_VK_PROF=2: GPU time by render pass size, summed over the report's frames */
typedef struct PassStat
{
    uint32_t key;
    double ns;
    uint64_t n, draws;
} PassStat;
static PassStat g_pstat[64];

static void prof_pass(uint32_t key, uint32_t draws, double ns)
{
    for (int i = 0; i < 64; ++i)
        if (g_pstat[i].key == key || !g_pstat[i].n)
        {
            g_pstat[i].key = key, g_pstat[i].ns += ns, g_pstat[i].n++, g_pstat[i].draws += draws;
            return;
        }
}

static int pstat_cmp(const void* a, const void* b)
{
    double x = ((const PassStat*)a)->ns, y = ((const PassStat*)b)->ns;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void prof_pass_report(uint32_t frames)
{
    qsort(g_pstat, 64, sizeof *g_pstat, pstat_cmp);
    for (int i = 0; i < 10 && g_pstat[i].n; ++i)
        rt_log("[gfx9]   passes %4ux%-4u%s: %.2f ms a frame in %.1f passes, %.0f draws\n", g_pstat[i].key >> 17,
            (g_pstat[i].key >> 1) & 0xFFFF, g_pstat[i].key & 1 ? " +depth" : "", g_pstat[i].ns / 1e6 / frames,
            (double)g_pstat[i].n / frames, (double)g_pstat[i].draws / frames);
    memset(g_pstat, 0, sizeof g_pstat);
}

static void reset_bound_state(void);
static void ubo_reset(void);
static void dset_reset(void);

static VkCommandBuffer cmd(void)
{
    if (!g_cmd_open)
    {
        Frame* f = &g_fr[g_frame];
        uint64_t t0 = g_prof ? now_ns() : 0;
        vkWaitForFences(g_dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
        if (g_prof)
        {
            g_t_fence += now_ns() - t0;
            static uint64_t ts[2 + 2 * PROF_PASSES];
            uint32_t nq = 2 + 2 * f->npass;
            if (f->qp_used && vkGetQueryPoolResults(g_dev, f->qp, 0, nq, nq * 8, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                double per = g_props.limits.timestampPeriod;
                g_t_gpu += (uint64_t)((ts[1] - ts[0]) * per), g_n_gpu++;
                for (uint32_t i = 0; i < f->npass; ++i)
                    prof_pass(f->pass_key[i], f->pass_draws[i], (ts[3 + 2 * i] - ts[2 + 2 * i]) * per);
            }
            f->qp_used = 0;
            f->npass = 0;
        }
        vkResetFences(g_dev, 1, &f->fence);
        trash_empty(f);
        vkResetCommandPool(g_dev, f->pool, 0);
        for (uint32_t i = 0; i < f->ndpools; ++i)
            vkResetDescriptorPool(g_dev, f->dpools[i], 0);
        f->cur_dpool = 0;
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f->cb, &bi);
        g_cb = f->cb;
        if (g_prof && f->qp)
        {
            vkCmdResetQueryPool(f->cb, f->qp, 0, 2 + 2 * PROF_PASSES);
            vkCmdWriteTimestamp(f->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f->qp, 0);
            f->qp_used = 1;
        }
        g_cmd_open = 1;
        g_ring_used = 0;
        reset_bound_state();
        ubo_reset();
        dset_reset();
    }
    return g_cb;
}

static int g_in_pass, g_targets_changed = 1;

static int g_pass_timed;

static void end_pass(void)
{
    if (g_in_pass)
    {
        vkCmdEndRenderPass(g_cb);
        g_in_pass = 0;
        if (g_pass_timed)
        {
            Frame* f = &g_fr[g_frame];
            vkCmdWriteTimestamp(g_cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f->qp, 3 + 2 * f->npass);
            f->npass++;
            g_pass_timed = 0;
        }
    }
}

/* Hands the frame's work to the GPU (with a swapchain image's semaphores when presenting) and moves
 * on to the next frame's resources; wait: until the GPU is done. */
static void submit(int wait, VkSemaphore wait_sem, VkSemaphore signal_sem)
{
    end_pass();
    if (!g_cmd_open)
        return;
    Frame* f = &g_fr[g_frame];
    pending_take(f);
    if (f->qp_used)
        vkCmdWriteTimestamp(f->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f->qp, 1);
    vkEndCommandBuffer(f->cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f->cb;
    if (wait_sem)
        si.waitSemaphoreCount = 1, si.pWaitSemaphores = &wait_sem, si.pWaitDstStageMask = &stage;
    if (signal_sem)
        si.signalSemaphoreCount = 1, si.pSignalSemaphores = &signal_sem;
    uint64_t t0 = g_prof ? now_ns() : 0;
    VkResult r = vkQueueSubmit(g_queue, 1, &si, f->fence);
    if (r != VK_SUCCESS)
        rt_log("[gfx9] vkQueueSubmit: %d\n", r);
    if (wait)
        vkWaitForFences(g_dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
    if (g_prof)
        g_t_submit += now_ns() - t0, g_n_submits++;
    g_cmd_open = 0;
    g_frame = (g_frame + 1) % FRAMES;
    g_targets_changed = 1;
}

/* ring space for n bytes this frame, all at once: a draw reserves what it copies before copying any
 * of it, so a full ring never splits one draw's data between two frames */
static void ring_ensure(uint32_t n)
{
    cmd();
    if (g_ring_used + n > RING_SIZE)
    {
        submit(0, VK_NULL_HANDLE, VK_NULL_HANDLE);
        cmd();
    }
}

static void* ring(uint32_t n, uint32_t align, VkBuffer* buf, uint32_t* off)
{
    cmd();
    uint32_t at = (g_ring_used + align - 1) / align * align;
    if (at + n > RING_SIZE)
    {
        submit(0, VK_NULL_HANDLE, VK_NULL_HANDLE);
        cmd();
        at = 0;
        if (n > RING_SIZE)
            return NULL;
    }
    g_ring_used = at + n;
    *buf = g_fr[g_frame].ring;
    *off = at;
    return g_fr[g_frame].ring_map + at;
}

/* upload space: the ring, or a buffer of its own for something larger than a quarter of it */
static void* staging(uint32_t n, VkBuffer* buf, uint32_t* off)
{
    if (n <= RING_SIZE / 4)
        return ring(n, 16, buf, off);
    cmd();
    Alloc a;
    VkBuffer b = buffer_new(n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &a);
    if (!b)
        return NULL;
    trash_add(&g_fr[g_frame], (Trash){ 3, NULL, (uint64_t)(uintptr_t)b, a });
    *buf = b;
    *off = 0;
    return a.b->map + a.off;
}

/* --- barriers ----------------------------------------------------------------------------------- */

static void barrier_full(void)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    g_n_barriers++;
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL,
        0, NULL);
}

static void barrier_image(G9Tex* t, VkImageLayout from, VkImageLayout to, uint32_t level, uint32_t nlevels, uint32_t layer,
    uint32_t nlayers)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->img;
    b.subresourceRange = (VkImageSubresourceRange){ t->aspect, level, nlevels, layer, nlayers };
    g_n_barriers++;
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
        1, &b);
}

/* A new image into its resting layout, outside a render pass: render targets and depth buffers
 * cleared (transparent black, depth 1), textures as they are until uploaded. */
static void tex_prepare(G9Tex* t)
{
    if (!t || t->inited)
        return;
    t->inited = 1;
    cmd();
    if (t->use == G9_USE_RT || t->use == G9_USE_DEPTH)
    {
        barrier_image(t, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, t->levels, 0, t->layers);
        VkImageSubresourceRange r = { t->aspect, 0, t->levels, 0, t->layers };
        if (t->f.depth)
        {
            VkClearDepthStencilValue v = { 1.0f, 0 };
            vkCmdClearDepthStencilImage(g_cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
        }
        else
        {
            VkClearColorValue v = { { 0, 0, 0, 0 } };
            vkCmdClearColorImage(g_cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
        }
        barrier_image(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->rest, 0, t->levels, 0, t->layers);
    }
    else
        barrier_image(t, VK_IMAGE_LAYOUT_UNDEFINED, t->rest, 0, t->levels, 0, t->layers);
}

/* --- textures ----------------------------------------------------------------------------------- */

static void level_dims(const G9Tex* t, uint32_t level, uint32_t* w, uint32_t* h, uint32_t* d)
{
    *w = t->w >> level ? t->w >> level : 1;
    *h = (t->type == G9_TEX_CUBE ? t->w : t->h) >> level;
    if (!*h)
        *h = 1;
    *d = t->type == G9_TEX_3D ? (t->d >> level ? t->d >> level : 1) : 1;
}

static VkComponentMapping tex_swizzle(const G9Tex* t)
{
    VkComponentMapping m = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                             VK_COMPONENT_SWIZZLE_IDENTITY };
    if (t->use != G9_USE_SAMPLE) /* as on Metal: no swizzle on render targets */
        return m;
    const VkComponentSwizzle R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G, O = VK_COMPONENT_SWIZZLE_ONE,
                             Z = VK_COMPONENT_SWIZZLE_ZERO;
    switch (t->f.swizzle)
    {
    case 1: m = (VkComponentMapping){ R, R, R, O }; break;
    case 2: m = (VkComponentMapping){ R, R, R, G }; break;
    case 3: m = (VkComponentMapping){ Z, Z, Z, R }; break;
    default: break;
    }
    if (t->fmt == F_X8R8G8B8 || t->fmt == F_X8B8G8R8 || t->fmt == F_X1R5G5B5 || t->fmt == F_X4R4G4B4)
        m.a = O;
    return m;
}

static VkImageView view_new(G9Tex* t, VkFormat f, VkImageViewType vt, VkImageAspectFlags aspect, uint32_t level,
    uint32_t nlevels, uint32_t layer, uint32_t nlayers, int swz)
{
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t->img;
    vi.viewType = vt;
    vi.format = f;
    if (swz)
        vi.components = tex_swizzle(t);
    vi.subresourceRange = (VkImageSubresourceRange){ aspect, level, nlevels, layer, nlayers };
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(g_dev, &vi, NULL, &v) != VK_SUCCESS)
        rt_log("[gfx9] cannot make a view of a %ux%u texture of format %08x\n", t->w, t->h, t->fmt);
    return v;
}

G9Tex* g9_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, int use)
{
    if (!g_dev)
        return NULL;
    G9Tex* t = (G9Tex*)calloc(1, sizeof *t);
    t->type = type, t->use = use, t->fmt = fmt, t->w = w ? w : 1, t->h = h ? h : 1, t->d = d ? d : 1;
    t->f = fmt_of(fmt);
    uint32_t maxl = 1;
    for (uint32_t s = t->w > t->h ? t->w : t->h; s > 1; s >>= 1)
        maxl++;
    t->levels = levels && levels <= maxl ? levels : maxl;
    t->layers = type == G9_TEX_CUBE ? 6 : 1;
    VkFormatFeatureFlags ff = fmt_features(t->f.vf);
    t->aspect = t->f.depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (t->f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
    t->rest = t->f.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    t->nolinear = !(ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = type == G9_TEX_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ii.format = t->f.vf;
    ii.extent = (VkExtent3D){ t->w, type == G9_TEX_CUBE ? t->w : t->h, type == G9_TEX_3D ? t->d : 1 };
    ii.mipLevels = t->levels;
    ii.arrayLayers = t->layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
        ii.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (use == G9_USE_RT && !t->f.depth && fmt != F_NULL)
        ii.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (t->f.depth)
        ii.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (type == G9_TEX_CUBE)
        ii.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkFormat list[2] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB };
    VkImageFormatListCreateInfoKHR fl = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR };
    if (t->f.vf == VK_FORMAT_B8G8R8A8_UNORM && !t->f.decode)
    {
        /* sRGB reads (D3DSAMP_SRGBTEXTURE) through a second view; the format list keeps the image
         * compressible where the driver can do that */
        ii.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (g_format_list)
        {
            fl.viewFormatCount = 2;
            fl.pViewFormats = list;
            ii.pNext = &fl;
        }
    }
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_dev, &ii, NULL, &t->img) != VK_SUCCESS)
    {
        rt_log("[gfx9] cannot make a %ux%ux%u texture of format %08x\n", t->w, t->h, t->d, fmt);
        free(t);
        return NULL;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_dev, t->img, &req);
    if (!mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &t->mem))
    {
        vkDestroyImage(g_dev, t->img, NULL);
        free(t);
        return NULL;
    }
    vkBindImageMemory(g_dev, t->img, t->mem.b->mem, t->mem.off);
    if (ii.usage & VK_IMAGE_USAGE_SAMPLED_BIT)
        t->view = view_new(t, t->f.vf,
            type == G9_TEX_CUBE ? VK_IMAGE_VIEW_TYPE_CUBE : type == G9_TEX_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D,
            t->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, t->levels, 0, t->layers, !t->f.depth);
    return t;
}

static void tex_free(G9Tex* t)
{
    if (t->att)
        for (uint32_t i = 0; i < t->levels * t->layers; ++i)
            if (t->att[i])
                vkDestroyImageView(g_dev, t->att[i], NULL);
    free(t->att);
    if (t->view)
        vkDestroyImageView(g_dev, t->view, NULL);
    if (t->srgb)
        vkDestroyImageView(g_dev, t->srgb, NULL);
    vkDestroyImage(g_dev, t->img, NULL);
    mem_free(&t->mem);
    free(t);
}

static G9Tex* g_rt[G9_MAX_RT];
static uint8_t g_rt_face[G9_MAX_RT], g_rt_level[G9_MAX_RT];
static G9Tex* g_ds;

void g9_tex_destroy(G9Tex* t)
{
    if (!t)
        return;
    for (int i = 0; i < G9_MAX_RT; ++i)
        if (g_rt[i] == t)
            g_rt[i] = NULL, g_targets_changed = 1;
    if (g_ds == t)
        g_ds = NULL, g_targets_changed = 1;
    pending_add((Trash){ 0, t, 0, { 0 } });
}

/* a render target view of one level and layer */
static VkImageView att_view(G9Tex* t, uint32_t level, uint32_t layer)
{
    if (level >= t->levels || layer >= t->layers)
        return VK_NULL_HANDLE;
    if (!t->att)
        t->att = (VkImageView*)calloc(t->levels * t->layers, sizeof *t->att);
    VkImageView* v = &t->att[level * t->layers + layer];
    if (!*v)
        *v = view_new(t, t->f.vf, VK_IMAGE_VIEW_TYPE_2D, t->aspect, level, 1, layer, 1, 0);
    return *v;
}

static uint32_t expand(uint32_t v, int bits) { return bits >= 8 ? v : (v << (8 - bits)) | (v >> (2 * bits - 8 > 0 ? 2 * bits - 8 : 0)); }

static void convert_row(int conv, const uint8_t* s, uint8_t* d, uint32_t w)
{
    for (uint32_t x = 0; x < w; ++x)
    {
        uint32_t v = conv == CONV_A4L4 ? s[x] : (uint32_t)(s[2 * x] | (s[2 * x + 1] << 8));
        uint32_t r = 0, g = 0, b = 0, a = 255;
        switch (conv)
        {
        case CONV_565: r = expand(v >> 11, 5), g = expand((v >> 5) & 63, 6), b = expand(v & 31, 5); break;
        case CONV_X555: r = expand((v >> 10) & 31, 5), g = expand((v >> 5) & 31, 5), b = expand(v & 31, 5); break;
        case CONV_1555: r = expand((v >> 10) & 31, 5), g = expand((v >> 5) & 31, 5), b = expand(v & 31, 5), a = (v >> 15) ? 255 : 0; break;
        case CONV_4444: a = ((v >> 12) & 15) * 17, r = ((v >> 8) & 15) * 17, g = ((v >> 4) & 15) * 17, b = (v & 15) * 17; break;
        case CONV_X444: r = ((v >> 8) & 15) * 17, g = ((v >> 4) & 15) * 17, b = (v & 15) * 17; break;
        case CONV_A4L4: d[2 * x] = (uint8_t)((v & 15) * 17), d[2 * x + 1] = (uint8_t)((v >> 4) * 17); continue;
        default: break;
        }
        d[4 * x] = (uint8_t)b, d[4 * x + 1] = (uint8_t)g, d[4 * x + 2] = (uint8_t)r, d[4 * x + 3] = (uint8_t)a;
    }
}

/* --- BC decoding (no BC sampling on the device) --------------------------------------------------- */

/* a BC1 colour block's four colours, BGRA; three and transparent black when c0 <= c1 in BC1 */
static void bc_colors(const uint8_t* b, uint8_t pal[4][4], int bc1)
{
    uint32_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    uint32_t r0 = expand(c0 >> 11, 5), g0 = expand((c0 >> 5) & 63, 6), b0 = expand(c0 & 31, 5);
    uint32_t r1 = expand(c1 >> 11, 5), g1 = expand((c1 >> 5) & 63, 6), b1 = expand(c1 & 31, 5);
    pal[0][0] = (uint8_t)b0, pal[0][1] = (uint8_t)g0, pal[0][2] = (uint8_t)r0, pal[0][3] = 255;
    pal[1][0] = (uint8_t)b1, pal[1][1] = (uint8_t)g1, pal[1][2] = (uint8_t)r1, pal[1][3] = 255;
    if (!bc1 || c0 > c1)
    {
        pal[2][0] = (uint8_t)((2 * b0 + b1) / 3), pal[2][1] = (uint8_t)((2 * g0 + g1) / 3), pal[2][2] = (uint8_t)((2 * r0 + r1) / 3);
        pal[3][0] = (uint8_t)((b0 + 2 * b1) / 3), pal[3][1] = (uint8_t)((g0 + 2 * g1) / 3), pal[3][2] = (uint8_t)((r0 + 2 * r1) / 3);
        pal[2][3] = pal[3][3] = 255;
    }
    else
    {
        pal[2][0] = (uint8_t)((b0 + b1) / 2), pal[2][1] = (uint8_t)((g0 + g1) / 2), pal[2][2] = (uint8_t)((r0 + r1) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
}

/* a BC3 alpha (or BC4 channel) block: eight bytes, sixteen values */
static void bc_alpha(const uint8_t* b, uint8_t out[16])
{
    uint32_t a0 = b[0], a1 = b[1], t[8] = { a0, a1 };
    if (a0 > a1)
        for (int i = 1; i < 7; ++i)
            t[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    else
    {
        for (int i = 1; i < 5; ++i)
            t[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        t[6] = 0, t[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; ++i)
        out[i] = (uint8_t)t[(bits >> (3 * i)) & 7];
}

/* one block into a decoded image of w x h at (bx, by) blocks, vt bytes per texel */
static void bc_block(int kind, const uint8_t* b, uint8_t* img, uint32_t w, uint32_t h, uint32_t bx, uint32_t by)
{
    uint8_t px[16][4];
    if (kind == DEC_BC4 || kind == DEC_BC5)
    {
        uint8_t r[16], g[16];
        bc_alpha(b, r);
        if (kind == DEC_BC5)
            bc_alpha(b + 8, g);
        for (int i = 0; i < 16; ++i)
            px[i][0] = r[i], px[i][1] = kind == DEC_BC5 ? g[i] : 0;
    }
    else
    {
        uint8_t pal[4][4], a[16];
        const uint8_t* c = kind == DEC_BC1 ? b : b + 8;
        bc_colors(c, pal, kind == DEC_BC1);
        uint32_t idx = c[4] | (c[5] << 8) | (c[6] << 16) | ((uint32_t)c[7] << 24);
        if (kind == DEC_BC2)
            for (int i = 0; i < 16; ++i)
                a[i] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
        else if (kind == DEC_BC3)
            bc_alpha(b, a);
        for (int i = 0; i < 16; ++i)
        {
            memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
            if (kind != DEC_BC1)
                px[i][3] = a[i];
        }
    }
    uint32_t vt = kind == DEC_BC4 ? 1 : kind == DEC_BC5 ? 2 : 4;
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x)
        {
            uint32_t X = bx * 4 + x, Y = by * 4 + y;
            if (X < w && Y < h)
                memcpy(img + ((size_t)Y * w + X) * vt, px[y * 4 + x], vt);
        }
}

/* --- uploads and copies ------------------------------------------------------------------------- */

void g9_tex_upload(G9Tex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h,
    uint32_t d, const void* src, uint32_t pitch, uint32_t slice)
{
    if (!t || level >= t->levels || t->f.depth)
        return;
    uint32_t lw, lh, ld;
    level_dims(t, level, &lw, &lh, &ld);
    if (x + w > lw || y + h > lh)
    {
        w = x < lw ? lw - x : 0;
        h = y < lh ? lh - y : 0;
    }
    if (!w || !h || !d)
        return;
    uint32_t rows, row_bytes, cw = w, ch = h;
    if (t->f.block && !t->f.decode)
    {
        /* whole blocks; a copy that is not a multiple of four reaches the level's edge */
        rows = (h + 3) / 4;
        row_bytes = ((w + 3) / 4) * t->f.block;
        cw = (w + 3) & ~3u, ch = (h + 3) & ~3u;
        if (x + cw > lw)
            cw = lw - x;
        if (y + ch > lh)
            ch = lh - y;
    }
    else
    {
        rows = h;
        row_bytes = w * t->f.vtexel;
    }
    uint32_t vslice = rows * row_bytes;
    VkBuffer buf;
    uint32_t off;
    uint8_t* dst = (uint8_t*)staging(vslice * d, &buf, &off);
    if (!dst)
        return;
    for (uint32_t k = 0; k < d; ++k)
    {
        const uint8_t* s0 = (const uint8_t*)src + (size_t)k * slice;
        uint8_t* o0 = dst + (size_t)k * vslice;
        if (t->f.decode)
        {
            for (uint32_t by = 0; by < (h + 3) / 4; ++by)
                for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx)
                    bc_block(t->f.decode, s0 + (size_t)by * pitch + (size_t)bx * t->f.block, o0, w, h, bx, by);
            continue;
        }
        for (uint32_t r = 0; r < rows; ++r)
        {
            const uint8_t* s = s0 + (size_t)r * pitch;
            uint8_t* o = o0 + (size_t)r * row_bytes;
            if (t->f.conv)
                convert_row(t->f.conv, s, o, w);
            else
                memcpy(o, s, row_bytes);
        }
    }
    end_pass();
    tex_prepare(t);
    uint32_t layer = t->type == G9_TEX_CUBE ? face : 0;
    barrier_image(t, t->rest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, level, 1, layer, 1);
    VkBufferImageCopy c = { 0 };
    c.bufferOffset = off;
    c.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 };
    c.imageOffset = (VkOffset3D){ (int32_t)x, (int32_t)y, (int32_t)z };
    c.imageExtent = (VkExtent3D){ cw, ch, d };
    vkCmdCopyBufferToImage(g_cb, buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier_image(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->rest, level, 1, layer, 1);
}

void g9_tex_read(G9Tex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || level >= t->levels || t->f.depth || t->f.decode)
        return;
    uint32_t w, h, d;
    level_dims(t, level, &w, &h, &d);
    uint32_t row = t->f.block ? ((w + 3) / 4) * t->f.block : w * t->f.vtexel;
    uint32_t rows = t->f.block ? (h + 3) / 4 : h;
    Alloc a;
    VkBuffer b = buffer_new((VkDeviceSize)row * rows, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &a);
    if (!b)
        return;
    end_pass();
    cmd();
    tex_prepare(t);
    uint32_t layer = t->type == G9_TEX_CUBE ? face : 0;
    barrier_image(t, t->rest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, level, 1, layer, 1);
    VkBufferImageCopy c = { 0 };
    c.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 };
    c.imageExtent = (VkExtent3D){ w, h, 1 };
    vkCmdCopyImageToBuffer(g_cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &c);
    barrier_image(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->rest, level, 1, layer, 1);
    barrier_full();
    submit(1, VK_NULL_HANDLE, VK_NULL_HANDLE);
    const uint8_t* s = a.b->map + a.off;
    uint32_t copy = row < pitch ? row : pitch;
    if (t->f.conv)
        copy = row; /* 16-bit formats come back as BGRA8: callers lock BGRA8 targets only */
    for (uint32_t r = 0; r < rows; ++r)
        memcpy((uint8_t*)dst + (size_t)r * pitch, s + (size_t)r * row, copy < pitch ? copy : pitch);
    vkDestroyBuffer(g_dev, b, NULL);
    mem_free(&a);
}

void g9_tex_gen_mips(G9Tex* t)
{
    if (!t || t->levels < 2 || t->f.block || t->f.depth)
        return;
    VkFormatFeatureFlags ff = fmt_features(t->f.vf);
    if (!(ff & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(ff & VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return;
    end_pass();
    cmd();
    tex_prepare(t);
    for (uint32_t l = 1; l < t->levels; ++l)
    {
        uint32_t sw, sh, sd, dw, dh, dd;
        level_dims(t, l - 1, &sw, &sh, &sd);
        level_dims(t, l, &dw, &dh, &dd);
        barrier_image(t, t->rest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l - 1, 1, 0, t->layers);
        barrier_image(t, t->rest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, l, 1, 0, t->layers);
        VkImageBlit b = { 0 };
        b.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, t->layers };
        b.srcOffsets[1] = (VkOffset3D){ (int32_t)sw, (int32_t)sh, (int32_t)sd };
        b.dstSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, l, 0, t->layers };
        b.dstOffsets[1] = (VkOffset3D){ (int32_t)dw, (int32_t)dh, (int32_t)dd };
        vkCmdBlitImage(g_cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
            t->nolinear ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        barrier_image(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->rest, l - 1, 1, 0, t->layers);
        barrier_image(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->rest, l, 1, 0, t->layers);
    }
}

/* --- buffers ------------------------------------------------------------------------------------ */

G9Buf* g9_buf_create(uint32_t size)
{
    if (!g_dev)
        return NULL;
    G9Buf* b = (G9Buf*)calloc(1, sizeof *b);
    b->size = size ? size : 16;
    b->buf = buffer_new((b->size + 3) & ~3u,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &b->mem);
    if (!b->buf)
    {
        free(b);
        return NULL;
    }
    return b;
}

void g9_buf_destroy(G9Buf* b)
{
    if (b)
        pending_add((Trash){ 1, b, 0, { 0 } });
}

void g9_buf_upload(G9Buf* b, uint32_t offset, const void* data, uint32_t size)
{
    if (!b || offset >= b->size)
        return;
    if (offset + size > b->size)
        size = b->size - offset;
    if (!size)
        return;
    uint32_t n = (size + 3) & ~3u;
    if (offset + n > ((b->size + 3) & ~3u))
        n = ((b->size + 3) & ~3u) - offset;
    VkBuffer src;
    uint32_t off;
    uint8_t* p = (uint8_t*)staging(n, &src, &off);
    if (!p)
        return;
    memcpy(p, data, size);
    end_pass();
    barrier_full();
    VkBufferCopy c = { off, offset, n };
    vkCmdCopyBuffer(g_cb, src, b->buf, 1, &c);
    barrier_full();
}

/* --- shaders: GLSL to SPIR-V -------------------------------------------------------------------- */

static pthread_mutex_t g_sm_lock = PTHREAD_MUTEX_INITIALIZER; /* sm_glsl.c's translations share its buffers */
static char g_preamble[2][512];                               /* vertex, pixel */
static char g_cache_dir[1024];
static const char* g_dump_dir;

static uint32_t* glsl_to_spirv(const char* src, int pixel, size_t* nwords, char* err, size_t errcap)
{
    glslang_input_t in;
    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = pixel ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_1;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_3;
    /* the preamble (float controls, no clip distances) goes after the #version line, where glslang
     * takes an #extension */
    const char* pre = g_preamble[pixel ? 1 : 0];
    const char* nl = strchr(src, '\n');
    char* full = NULL;
    if (*pre && nl && !strncmp(src, "#version", 8))
    {
        size_t a = (size_t)(nl + 1 - src), b = strlen(pre), c = strlen(nl + 1);
        full = (char*)malloc(a + b + c + 1);
        memcpy(full, src, a);
        memcpy(full + a, pre, b);
        memcpy(full + a + b, nl + 1, c + 1);
    }
    in.code = full ? full : src;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = (glslang_messages_t)(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
    in.resource = glslang_default_resource();
    glslang_shader_t* sh = glslang_shader_create(&in);
    uint32_t* out = NULL;
    if (!sh)
    {
        snprintf(err, errcap, "glslang_shader_create failed");
        free(full);
        return NULL;
    }
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in))
    {
        snprintf(err, errcap, "%s", glslang_shader_get_info_log(sh));
        glslang_shader_delete(sh);
        free(full);
        return NULL;
    }
    glslang_program_t* p = glslang_program_create();
    glslang_program_add_shader(p, sh);
    if (!glslang_program_link(p, in.messages))
        snprintf(err, errcap, "%s", glslang_program_get_info_log(p));
    else
    {
        glslang_spv_options_t o;
        memset(&o, 0, sizeof o); /* the SPIR-V optimizer on */
        glslang_program_SPIRV_generate_with_options(p, in.stage, &o);
        size_t n = glslang_program_SPIRV_get_size(p);
        out = (uint32_t*)malloc(n * 4);
        glslang_program_SPIRV_get(p, out);
        *nwords = n;
    }
    glslang_program_delete(p);
    glslang_shader_delete(sh);
    free(full);
    return out;
}

static uint32_t* spv_load(uint64_t key, size_t* nwords)
{
    if (!g_cache_dir[0])
        return NULL;
    char path[1200];
    snprintf(path, sizeof path, "%s/spv/%016llx.spv", g_cache_dir, (unsigned long long)key);
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t* w = NULL;
    if (n > 20 && !(n & 3))
    {
        w = (uint32_t*)malloc((size_t)n);
        if (fread(w, 1, (size_t)n, f) != (size_t)n || w[0] != 0x07230203u)
            free(w), w = NULL;
        else
            *nwords = (size_t)n / 4;
    }
    fclose(f);
    return w;
}

static void spv_store(uint64_t key, const uint32_t* w, size_t n)
{
    if (!g_cache_dir[0])
        return;
    char path[1200], tmp[1300];
    snprintf(path, sizeof path, "%s/spv/%016llx.spv", g_cache_dir, (unsigned long long)key);
    snprintf(tmp, sizeof tmp, "%s.%p.tmp", path, (void*)w);
    FILE* f = fopen(tmp, "wb");
    if (!f)
        return;
    int ok = fwrite(w, 4, n, f) == n;
    if (fclose(f) == 0 && ok)
        rename(tmp, path);
    else
        remove(tmp);
}

static void dump_source(const char* src, int pixel, uint32_t hash, const char* why)
{
    if (!g_dump_dir || !*g_dump_dir)
        return;
    char path[1200];
    snprintf(path, sizeof path, "%s/%s%s_%08x_%016llx.%s", g_dump_dir, why, pixel ? "ps" : "vs", hash,
        (unsigned long long)fnv64(src, strlen(src)), pixel ? "frag" : "vert");
    FILE* f = fopen(path, "w");
    if (f)
    {
        fputs(src, f);
        fclose(f);
    }
}

static int compiled_ready(Compiled* c, int wait)
{
    while (!__atomic_load_n(&c->state, __ATOMIC_ACQUIRE))
    {
        if (!wait)
            return 0;
        struct timespec ts = { 0, 100000 };
        nanosleep(&ts, NULL);
    }
    return 1;
}

static void compiled_done(Compiled* c) { __atomic_store_n(&c->state, 1, __ATOMIC_RELEASE); }

static uint32_t g_compiles, g_fn_reuse, g_spv_hits;

/* GLSL (owned: freed here) to a shader module */
static void compile_shader(Compiled* c, char* glsl, int pixel, uint32_t hash)
{
    uint64_t key = fnv64(glsl, strlen(glsl)) ^ fnv64(g_preamble[pixel], strlen(g_preamble[pixel]));
    size_t n = 0;
    uint32_t* spv = spv_load(key, &n);
    if (spv)
        __atomic_fetch_add(&g_spv_hits, 1, __ATOMIC_RELAXED);
    else
    {
        char err[2048];
        spv = glsl_to_spirv(glsl, pixel, &n, err, sizeof err);
        if (spv)
            spv_store(key, spv, n);
        else
        {
            rt_log("[gfx9] shader %08x: glslang: %.600s\n", hash, err);
            dump_source(glsl, pixel, hash, "fail_");
        }
    }
    if (spv)
    {
        VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        mi.codeSize = n * 4;
        mi.pCode = spv;
        if (vkCreateShaderModule(g_dev, &mi, NULL, &c->mod) != VK_SUCCESS)
            c->mod = VK_NULL_HANDLE;
        free(spv);
    }
    free(glsl);
    compiled_done(c);
}

/* --- worker threads: background compiles ---------------------------------------------------------- */

typedef struct PipeKey PipeKey;
static VkPipeline make_pipeline(const PipeKey* k, VkRenderPass rp, VkShaderModule vs, VkShaderModule ps);

typedef struct Job
{
    struct Job* next;
    int kind; /* 0 shader, 1 pipeline */
    Compiled* out;
    char* glsl;
    int pixel;
    uint32_t hash, hash2;
    PipeKey* key;
    VkRenderPass rp;
    Compiled *vc, *pc;
} Job;

static pthread_mutex_t g_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_job_cond = PTHREAD_COND_INITIALIZER;
static Job *g_job_head, *g_job_tail;
static int g_workers;
static uint32_t g_pipes_made, g_pipes_saved;
static uint32_t g_draw_errors;

static void run_pipeline_job(Job* j)
{
    compiled_ready(j->vc, 1);
    compiled_ready(j->pc, 1);
    VkPipeline p = VK_NULL_HANDLE;
    if (j->vc->mod && j->pc->mod)
        p = make_pipeline(j->key, j->rp, j->vc->mod, j->pc->mod);
    if (!p && __atomic_fetch_add(&g_draw_errors, 1, __ATOMIC_RELAXED) < 20)
        rt_log("[gfx9] pipeline (vs %08x, ps %08x) failed\n", j->hash, j->hash2);
    j->out->pipe = p;
    compiled_done(j->out);
    uint32_t made = __atomic_add_fetch(&g_pipes_made, 1, __ATOMIC_RELAXED);
    if (made % 100 == 0)
        rt_log("[gfx9] %u pipelines made\n", made);
}

static void run_job(Job* j)
{
    if (j->kind == 0)
        compile_shader(j->out, j->glsl, j->pixel, j->hash);
    else
        run_pipeline_job(j);
    free(j->key);
    free(j);
}

static void* worker(void* arg)
{
    (void)arg;
    for (;;)
    {
        pthread_mutex_lock(&g_job_lock);
        while (!g_job_head)
            pthread_cond_wait(&g_job_cond, &g_job_lock);
        Job* j = g_job_head;
        g_job_head = j->next;
        if (!g_job_head)
            g_job_tail = NULL;
        pthread_mutex_unlock(&g_job_lock);
        run_job(j);
    }
    return NULL;
}

static void job_submit(Job* j, int background)
{
    if (!background || !g_workers)
    {
        run_job(j);
        return;
    }
    j->next = NULL;
    pthread_mutex_lock(&g_job_lock);
    if (g_job_tail)
        g_job_tail->next = j;
    else
        g_job_head = j;
    g_job_tail = j;
    pthread_cond_signal(&g_job_cond);
    pthread_mutex_unlock(&g_job_lock);
}

G9Shader* g9_shader_create(const uint32_t* tokens, uint32_t ntokens)
{
    G9Shader* s = (G9Shader*)calloc(1, sizeof *s);
    s->tokens = (uint32_t*)malloc(4 * ntokens);
    memcpy(s->tokens, tokens, 4 * ntokens);
    s->ntokens = ntokens;
    pthread_mutex_lock(&g_sm_lock);
    int ok = sm_glsl_info(s->tokens, ntokens, &s->info);
    pthread_mutex_unlock(&g_sm_lock);
    if (!ok)
        rt_log("[gfx9] shader %08x: not translatable\n", s->info.hash);
    return s;
}

void g9_shader_destroy(G9Shader* s)
{
    if (!s)
        return;
    free(s->var); /* the modules stay in g_fns: the game makes the same shaders again */
    free(s->tokens);
    free(s);
}

/* Translated modules by shader content and variant, across shader objects (as on Metal). */
static Compiled* shader_variant(G9Shader* s, const SmOpts* o, int background)
{
    uint64_t key = fnv64(o, sizeof *o);
    for (uint32_t i = 0; i < s->nvar; ++i)
        if (s->var[i].key == key)
            return s->var[i].fn;
    if (!s->content)
        s->content = fnv64(s->tokens, 4ull * s->ntokens);
    struct { uint64_t content, opts; uint32_t ntokens, pad; } fk = { s->content, key, s->ntokens, 0 };
    uint64_t fh;
    Compiled* c = (Compiled*)cache_get(&g_fns, &fk, sizeof fk, &fh);
    if (c)
        g_fn_reuse++;
    else
    {
        c = (Compiled*)calloc(1, sizeof *c);
        cache_put(&g_fns, &fk, sizeof fk, fh, c);
        if (++g_compiles % 100 == 0)
            rt_log("[gfx9] %u shader variants compiled (%u reused from other shader objects, %u from the disk cache)\n",
                g_compiles, g_fn_reuse, g_spv_hits);
        static char src[1 << 20];
        char err[256];
        SmInfo info;
        pthread_mutex_lock(&g_sm_lock);
        size_t n = sm_glsl_translate(s->tokens, s->ntokens, o, &info, src, sizeof src, err, sizeof err);
        char* glsl = n ? (char*)malloc(n + 1) : NULL;
        if (glsl)
            memcpy(glsl, src, n + 1);
        pthread_mutex_unlock(&g_sm_lock);
        if (glsl)
        {
            if (g_dump_dir && *g_dump_dir)
                dump_source(glsl, info.pixel, s->info.hash, "");
            Job* j = (Job*)calloc(1, sizeof *j);
            j->kind = 0, j->out = c, j->glsl = glsl, j->pixel = info.pixel, j->hash = s->info.hash;
            job_submit(j, background);
        }
        else
        {
            rt_log("[gfx9] shader %08x: %s\n", s->info.hash, err);
            compiled_done(c);
        }
    }
    if (s->nvar == s->capvar)
    {
        s->capvar = s->capvar ? s->capvar * 2 : 4;
        s->var = (Variant*)realloc(s->var, s->capvar * sizeof *s->var);
    }
    s->var[s->nvar++] = (Variant){ key, c };
    return c;
}

/* --- descriptor sets, samplers, layouts ----------------------------------------------------------- */

static VkDescriptorSetLayout g_ubo_layout, g_tex_layout, g_util_layout;
static VkPipelineLayout g_layout, g_util_playout;
static VkDescriptorPool g_static_pool;
static VkPipelineCache g_pcache;
static VkSampler g_linear;
static VkBuffer g_default_attr;
static Alloc g_default_attr_mem;
static G9Tex* g_dummy[3]; /* 2D, cube, volume: what a sampler with no texture reads (zeros) */

static VkDescriptorSet dset_alloc(VkDescriptorSetLayout l)
{
    Frame* f = &g_fr[g_frame];
    for (;;)
    {
        if (f->cur_dpool == f->ndpools)
        {
            VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, DESC_SETS_PER_POOL * 8 };
            VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pi.maxSets = DESC_SETS_PER_POOL;
            pi.poolSizeCount = 1;
            pi.pPoolSizes = &ps;
            VkDescriptorPool p;
            if (vkCreateDescriptorPool(g_dev, &pi, NULL, &p) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            f->dpools = (VkDescriptorPool*)realloc(f->dpools, (f->ndpools + 1) * sizeof *f->dpools);
            f->dpools[f->ndpools++] = p;
        }
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = f->dpools[f->cur_dpool];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &l;
        VkDescriptorSet s;
        VkResult r = vkAllocateDescriptorSets(g_dev, &ai, &s);
        if (r == VK_SUCCESS)
            return s;
        f->cur_dpool++; /* full (or fragmented): the next pool */
    }
}

/* this frame's texture sets by what they hold: draws in a row often bind the same textures */
#define DSET_CACHE 2048
typedef struct DsetKey
{
    uint32_t mask;
    VkImageView view[G9_SAMPLERS];
    VkSampler samp[G9_SAMPLERS];
    VkImageLayout layout[G9_SAMPLERS];
} DsetKey;
typedef struct DsetEnt
{
    uint64_t h;
    uint32_t gen;
    DsetKey k;
    VkDescriptorSet set;
} DsetEnt;
static DsetEnt* g_dsets;
static uint32_t g_dset_gen = 1;

static void dset_reset(void) { g_dset_gen++; }

static VkDescriptorSet tex_set(const DsetKey* k)
{
    uint64_t h = fnv64(k, sizeof *k);
    DsetEnt* slot = NULL;
    for (uint32_t i = 0; i < 8; ++i)
    {
        DsetEnt* e = &g_dsets[(h + i) % DSET_CACHE];
        if (e->gen != g_dset_gen)
        {
            if (!slot)
                slot = e;
            break;
        }
        if (e->h == h && !memcmp(&e->k, k, sizeof *k))
            return e->set;
    }
    VkDescriptorSet s = dset_alloc(g_tex_layout);
    if (!s)
        return VK_NULL_HANDLE;
    VkDescriptorImageInfo ii[G9_SAMPLERS];
    VkWriteDescriptorSet w[G9_SAMPLERS];
    uint32_t n = 0;
    for (uint32_t b = 0; b < G9_SAMPLERS; ++b)
        if (k->mask & (1u << b))
        {
            ii[n] = (VkDescriptorImageInfo){ k->samp[b], k->view[b], k->layout[b] };
            w[n] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[n].dstSet = s;
            w[n].dstBinding = b;
            w[n].descriptorCount = 1;
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[n].pImageInfo = &ii[n];
            n++;
        }
    if (n)
        vkUpdateDescriptorSets(g_dev, n, w, 0, NULL);
    if (slot)
    {
        slot->h = h, slot->gen = g_dset_gen, slot->k = *k, slot->set = s;
    }
    return s;
}

static VkSamplerAddressMode address(uint32_t a)
{
    switch (a)
    {
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5: return g_mirror_once ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static VkSampler sampler(const G9Sampler* s, int compare_mode, int nolinear)
{
    struct { G9Sampler s; int cmp, nolinear; } k;
    memset(&k, 0, sizeof k);
    k.s = *s;
    k.s.srgb = 0;
    k.cmp = compare_mode;
    k.nolinear = nolinear;
    uint64_t h;
    VkSampler r = (VkSampler)cache_get(&g_samplers, &k, sizeof k, &h);
    if (r)
        return r;
    VkSamplerCreateInfo d = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    d.addressModeU = address(s->addr_u);
    d.addressModeV = address(s->addr_v);
    d.addressModeW = address(s->addr_w);
    d.magFilter = s->mag >= 2 && !nolinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    d.minFilter = s->min >= 2 && !nolinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    d.mipmapMode = s->mip >= 2 && !nolinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (s->mip == 0)
        d.minLod = 0, d.maxLod = 0.25f; /* not mipmapped: level 0 (Vulkan's way of saying so) */
    else
        d.minLod = s->max_level, d.maxLod = VK_LOD_CLAMP_NONE;
    if ((s->min == 3 || s->mag == 3) && g_feat.samplerAnisotropy && !nolinear && s->max_aniso > 1)
    {
        d.anisotropyEnable = VK_TRUE;
        float m = s->max_aniso > 16 ? 16.0f : s->max_aniso;
        d.maxAnisotropy = m < g_props.limits.maxSamplerAnisotropy ? m : g_props.limits.maxSamplerAnisotropy;
    }
    uint32_t bc = s->border;
    d.borderColor = (bc >> 24) == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                    : (bc & 0xFFFFFF) == 0xFFFFFF ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                  : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (compare_mode)
        d.compareEnable = VK_TRUE, d.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (vkCreateSampler(g_dev, &d, NULL, &r) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    cache_put(&g_samplers, &k, sizeof k, h, (void*)r);
    return r;
}

/* --- render passes and framebuffers --------------------------------------------------------------- */

typedef struct RPKey
{
    uint32_t color[G9_MAX_RT], depth;
    uint8_t ncolor, cload, dload, sload, present, pad[3];
} RPKey;

/* a render pass: the colour slots up to the last bound (unbound slots unused), then depth. Images
 * come in and go out in their resting layouts; the dependencies order the pass after everything
 * before it and before everything after, as D3D9's draws are */
static VkRenderPass render_pass(const RPKey* k)
{
    uint64_t h;
    VkRenderPass rp = (VkRenderPass)cache_get(&g_rps, k, sizeof *k, &h);
    if (rp)
        return rp;
    VkAttachmentDescription at[G9_MAX_RT + 1];
    VkAttachmentReference cref[G9_MAX_RT], dref;
    uint32_t n = 0;
    for (uint32_t i = 0; i < k->ncolor; ++i)
    {
        if (!k->color[i])
        {
            cref[i] = (VkAttachmentReference){ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
            continue;
        }
        VkAttachmentDescription a = { 0 };
        a.format = (VkFormat)k->color[i];
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = k->cload ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = k->present ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        a.finalLayout = k->present ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cref[i] = (VkAttachmentReference){ n, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        at[n++] = a;
    }
    VkSubpassDescription sp = { 0 };
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = k->ncolor;
    sp.pColorAttachments = cref;
    if (k->depth)
    {
        int st = k->depth == VK_FORMAT_D32_SFLOAT_S8_UINT || k->depth == VK_FORMAT_D24_UNORM_S8_UINT;
        VkAttachmentDescription a = { 0 };
        a.format = (VkFormat)k->depth;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = k->dload ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = !st ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : k->sload ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a.stencilStoreOp = st ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        dref = (VkAttachmentReference){ n, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        at[n++] = a;
        sp.pDepthStencilAttachment = &dref;
    }
    /* the passes before this one wrote attachments this one samples or loads, or sampled what this one
     * writes; transfers have barriers of their own. Fragment work only, so the next pass's vertex work can
     * start early (all graphics stages here cost MoltenVK 3 ms a frame in a town); a vertex shader's
     * texture written by the pass just before would want VERTEX_SHADER added */
    VkSubpassDependency dep[2] = { { 0 } };
    const VkPipelineStageFlags frag = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkAccessFlags writes = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkAccessFlags uses = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL, dep[0].dstSubpass = 0;
    dep[0].srcStageMask = frag, dep[0].dstStageMask = frag;
    dep[0].srcAccessMask = writes, dep[0].dstAccessMask = uses;
    dep[1].srcSubpass = 0, dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = frag, dep[1].dstStageMask = frag;
    dep[1].srcAccessMask = writes, dep[1].dstAccessMask = uses;
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = n;
    ci.pAttachments = at;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    ci.dependencyCount = 2;
    ci.pDependencies = dep;
    if (vkCreateRenderPass(g_dev, &ci, NULL, &rp) != VK_SUCCESS)
    {
        rt_log("[gfx9] cannot make a render pass\n");
        return VK_NULL_HANDLE;
    }
    cache_put(&g_rps, k, sizeof *k, h, (void*)rp);
    return rp;
}

typedef struct FbEnt
{
    VkRenderPass rp;
    VkImageView v[G9_MAX_RT + 1];
    G9Tex* t[G9_MAX_RT + 1];
    uint32_t n, w, h;
    VkFramebuffer fb;
} FbEnt;
static FbEnt* g_fbs;
static uint32_t g_nfbs, g_capfbs, g_fb_last;

static VkFramebuffer framebuffer(VkRenderPass rp, const VkImageView* v, G9Tex* const* t, uint32_t n, uint32_t w, uint32_t h)
{
    for (uint32_t k = 0; k < g_nfbs; ++k)
    {
        uint32_t i = (g_fb_last + k) % g_nfbs;
        FbEnt* e = &g_fbs[i];
        if (e->rp == rp && e->n == n && e->w == w && e->h == h && !memcmp(e->v, v, n * sizeof *v))
        {
            g_fb_last = i;
            return e->fb;
        }
    }
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = rp;
    fi.attachmentCount = n;
    fi.pAttachments = v;
    fi.width = w, fi.height = h, fi.layers = 1;
    VkFramebuffer fb;
    if (vkCreateFramebuffer(g_dev, &fi, NULL, &fb) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    if (g_nfbs == g_capfbs)
    {
        g_capfbs = g_capfbs ? g_capfbs * 2 : 64;
        g_fbs = (FbEnt*)realloc(g_fbs, g_capfbs * sizeof *g_fbs);
    }
    FbEnt* e = &g_fbs[g_nfbs];
    memset(e, 0, sizeof *e);
    e->rp = rp, e->n = n, e->w = w, e->h = h, e->fb = fb;
    memcpy(e->v, v, n * sizeof *v);
    memcpy(e->t, t, n * sizeof *t);
    g_fb_last = g_nfbs++;
    return fb;
}

/* a texture going away: the framebuffers made with it go too, once the GPU is past them */
static void fb_scrub(G9Tex* t)
{
    for (uint32_t i = 0; i < g_nfbs;)
    {
        FbEnt* e = &g_fbs[i];
        int uses = 0;
        for (uint32_t k = 0; k < e->n; ++k)
            uses |= e->t[k] == t;
        if (uses)
        {
            trash_add(&g_fr[g_frame], (Trash){ 2, NULL, (uint64_t)(uintptr_t)e->fb, { 0 } });
            *e = g_fbs[--g_nfbs];
            continue;
        }
        i++;
    }
    g_fb_last = 0;
}

/* --- targets ------------------------------------------------------------------------------------ */

static uint32_t g_pending_clear; /* D3DCLEAR_* to fold into the next pass's load operations */
static float g_clear_color[4], g_clear_z;
static uint32_t g_clear_stencil;
static uint32_t g_fb_w, g_fb_h;
static RPKey g_pass_key;

static int begin_pass(void);

void g9_set_targets(G9Tex* const color[G9_MAX_RT], const uint8_t face[G9_MAX_RT], const uint8_t level[G9_MAX_RT],
    G9Tex* depth)
{
    int same = depth == g_ds;
    for (int i = 0; i < G9_MAX_RT; ++i)
        same &= color[i] == g_rt[i] && (!color[i] || (face[i] == g_rt_face[i] && level[i] == g_rt_level[i]));
    if (same)
        return;
    if (g_pending_clear)
        begin_pass(); /* a clear of the old targets with no draw after it still happens */
    end_pass();
    for (int i = 0; i < G9_MAX_RT; ++i)
        g_rt[i] = color[i], g_rt_face[i] = face[i], g_rt_level[i] = level[i];
    g_ds = depth;
    g_targets_changed = 1;
    g_pending_clear = 0;
}

static void target_size(uint32_t* w, uint32_t* h)
{
    for (int i = 0; i < G9_MAX_RT; ++i)
        if (g_rt[i])
        {
            uint32_t d;
            level_dims(g_rt[i], g_rt_level[i], w, h, &d);
            return;
        }
    if (g_ds)
    {
        *w = g_ds->w, *h = g_ds->h;
        return;
    }
    *w = *h = 1;
}

/* the bound targets as a render pass key (load operations from the pending clear) */
static int pass_key(RPKey* k)
{
    memset(k, 0, sizeof *k);
    for (int i = 0; i < G9_MAX_RT; ++i)
    {
        G9Tex* t = g_rt[i];
        if (!t || t->fmt == F_NULL || t->f.depth)
            continue;
        k->color[i] = (uint32_t)t->f.vf;
        k->ncolor = (uint8_t)(i + 1);
    }
    if (g_ds && g_ds->f.depth)
        k->depth = (uint32_t)g_ds->f.vf;
    return k->ncolor || k->depth;
}

static void reset_bound_state(void);

static int begin_pass(void)
{
    if (g_in_pass)
        return 1;
    RPKey k;
    if (!pass_key(&k))
        return 0;
    cmd();
    VkImageView v[G9_MAX_RT + 1];
    G9Tex* tt[G9_MAX_RT + 1];
    VkClearValue cv[G9_MAX_RT + 1];
    uint32_t n = 0, fw = 0xFFFFFFFFu, fh = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < k.ncolor; ++i)
    {
        if (!k.color[i])
            continue;
        G9Tex* t = g_rt[i];
        tex_prepare(t);
        v[n] = att_view(t, g_rt_level[i], t->type == G9_TEX_CUBE ? g_rt_face[i] : 0);
        if (!v[n])
            return 0;
        tt[n] = t;
        uint32_t w, h, d;
        level_dims(t, g_rt_level[i], &w, &h, &d);
        fw = w < fw ? w : fw, fh = h < fh ? h : fh;
        memcpy(cv[n].color.float32, g_clear_color, sizeof g_clear_color);
        n++;
    }
    if (k.depth)
    {
        tex_prepare(g_ds);
        v[n] = att_view(g_ds, 0, 0);
        if (!v[n])
            return 0;
        tt[n] = g_ds;
        fw = g_ds->w < fw ? g_ds->w : fw, fh = g_ds->h < fh ? g_ds->h : fh;
        cv[n].depthStencil = (VkClearDepthStencilValue){ g_clear_z, g_clear_stencil };
        n++;
    }
    VkRenderPass compat = render_pass(&k);
    k.cload = (g_pending_clear & 1) != 0;
    k.dload = (g_pending_clear & 2) != 0;
    k.sload = (g_pending_clear & 4) != 0;
    VkRenderPass rp = render_pass(&k);
    VkFramebuffer fb = framebuffer(compat, v, tt, n, fw, fh);
    if (!rp || !fb)
        return 0;
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea.extent = (VkExtent2D){ fw, fh };
    bi.clearValueCount = n;
    bi.pClearValues = cv;
    Frame* pf = &g_fr[g_frame];
    if (g_prof > 1 && pf->qp_used && pf->npass < PROF_PASSES)
    {
        vkCmdWriteTimestamp(g_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pf->qp, 2 + 2 * pf->npass);
        pf->pass_key[pf->npass] = (fw << 17) | ((fh & 0xFFFF) << 1) | (k.depth ? 1 : 0);
        pf->pass_draws[pf->npass] = 0;
        g_pass_timed = 1;
    }
    vkCmdBeginRenderPass(g_cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    g_n_passes++;
    g_in_pass = 1;
    g_pending_clear = 0;
    g_targets_changed = 0;
    g_fb_w = fw, g_fb_h = fh;
    k.cload = k.dload = k.sload = 0;
    g_pass_key = k;
    return 1;
}

void g9_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
    const G9Viewport* vp, const int32_t* scissor)
{
    if (!g_dev)
        return;
    uint32_t tw, th;
    target_size(&tw, &th);
    int whole = !nrects && vp->x == 0 && vp->y == 0 && vp->w >= tw && vp->h >= th && !scissor;
    if (!g_ds)
        flags &= 1;
    else if (!g_ds->f.stencil)
        flags &= 3;
    if (!flags)
        return;
    float col[4] = { ((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f,
                     (color >> 24) / 255.0f };
    if (whole && !g_in_pass)
    {
        g_pending_clear |= flags;
        memcpy(g_clear_color, col, sizeof col);
        g_clear_z = z;
        g_clear_stencil = stencil;
        return;
    }
    if (g_pending_clear)
    {
        /* an earlier whole-target clear still pending: start its pass first */
        if (!begin_pass())
            return;
    }
    if (whole)
    {
        /* a whole-target clear after draws: end the pass and start a clearing one */
        end_pass();
        g_pending_clear = flags;
        memcpy(g_clear_color, col, sizeof col);
        g_clear_z = z;
        g_clear_stencil = stencil;
        return;
    }
    if (!begin_pass())
        return;
    /* each rectangle (or the viewport), clipped by the viewport and the scissor */
    VkClearAttachment ca[G9_MAX_RT + 1];
    uint32_t na = 0;
    if (flags & 1)
        for (uint32_t i = 0; i < g_pass_key.ncolor; ++i)
            if (g_pass_key.color[i])
            {
                ca[na].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                ca[na].colorAttachment = i;
                memcpy(ca[na].clearValue.color.float32, col, sizeof col);
                na++;
            }
    if ((flags & 6) && g_pass_key.depth)
    {
        ca[na].aspectMask = ((flags & 2) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((flags & 4) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca[na].colorAttachment = 0;
        ca[na].clearValue.depthStencil = (VkClearDepthStencilValue){ z, stencil };
        na++;
    }
    if (!na)
        return;
    int32_t one[4] = { (int32_t)vp->x, (int32_t)vp->y, (int32_t)(vp->x + vp->w), (int32_t)(vp->y + vp->h) };
    const int32_t* rs = nrects ? rects : one;
    uint32_t n = nrects ? nrects : 1;
    for (uint32_t i = 0; i < n; ++i)
    {
        int32_t l = rs[4 * i], t = rs[4 * i + 1], r = rs[4 * i + 2], b = rs[4 * i + 3];
        if (l < (int32_t)vp->x) l = (int32_t)vp->x;
        if (t < (int32_t)vp->y) t = (int32_t)vp->y;
        if (r > (int32_t)(vp->x + vp->w)) r = (int32_t)(vp->x + vp->w);
        if (b > (int32_t)(vp->y + vp->h)) b = (int32_t)(vp->y + vp->h);
        if (scissor)
        {
            if (l < scissor[0]) l = scissor[0];
            if (t < scissor[1]) t = scissor[1];
            if (r > scissor[2]) r = scissor[2];
            if (b > scissor[3]) b = scissor[3];
        }
        if (l < 0) l = 0;
        if (t < 0) t = 0;
        if (r > (int32_t)g_fb_w) r = (int32_t)g_fb_w;
        if (b > (int32_t)g_fb_h) b = (int32_t)g_fb_h;
        if (r <= l || b <= t)
            continue;
        VkClearRect cr = { { { l, t }, { (uint32_t)(r - l), (uint32_t)(b - t) } }, 0, 1 };
        vkCmdClearAttachments(g_cb, na, ca, 1, &cr);
    }
}

/* --- StretchRect and ColorFill ------------------------------------------------------------------- */

void g9_stretch(G9Tex* src, uint32_t sface, uint32_t slevel, const int32_t sr[4], G9Tex* dst, uint32_t dface,
    uint32_t dlevel, const int32_t dr[4], int linear)
{
    if (!src || !dst || !g_dev || slevel >= src->levels || dlevel >= dst->levels)
        return;
    uint32_t sw, sh, sd, dw, dh, dd;
    level_dims(src, slevel, &sw, &sh, &sd);
    level_dims(dst, dlevel, &dw, &dh, &dd);
    int32_t s[4] = { 0, 0, (int32_t)sw, (int32_t)sh }, d[4] = { 0, 0, (int32_t)dw, (int32_t)dh };
    if (sr)
        memcpy(s, sr, sizeof s);
    if (dr)
        memcpy(d, dr, sizeof d);
    uint32_t slayer = src->type == G9_TEX_CUBE ? sface : 0, dlayer = dst->type == G9_TEX_CUBE ? dface : 0;
    if (src == dst && slevel == dlevel && slayer == dlayer)
        return;
    int same = s[2] - s[0] == d[2] - d[0] && s[3] - s[1] == d[3] - d[1] && src->f.vf == dst->f.vf;
    if (!same && src->f.depth)
        return; /* depth copies are same size and format only */
    if (!same && (!(fmt_features(src->f.vf) & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
                     !(fmt_features(dst->f.vf) & VK_FORMAT_FEATURE_BLIT_DST_BIT) || src->f.block || dst->f.block))
    {
        static int told;
        if (!told++)
            rt_log("[gfx9] StretchRect from format %08x to %08x: no blit for these\n", src->fmt, dst->fmt);
        return;
    }
    end_pass();
    cmd();
    tex_prepare(src);
    tex_prepare(dst);
    barrier_image(src, src->rest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slevel, 1, slayer, 1);
    barrier_image(dst, dst->rest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dlevel, 1, dlayer, 1);
    if (same)
    {
        /* same size and format (and every depth copy): a copy */
        VkImageCopy c = { 0 };
        c.srcSubresource = (VkImageSubresourceLayers){ src->aspect & dst->aspect, slevel, slayer, 1 };
        c.dstSubresource = (VkImageSubresourceLayers){ src->aspect & dst->aspect, dlevel, dlayer, 1 };
        c.srcOffset = (VkOffset3D){ s[0], s[1], 0 };
        c.dstOffset = (VkOffset3D){ d[0], d[1], 0 };
        c.extent = (VkExtent3D){ (uint32_t)(s[2] - s[0]), (uint32_t)(s[3] - s[1]), 1 };
        if (c.extent.width && c.extent.height)
            vkCmdCopyImage(g_cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    else
    {
        /* scaled or converted */
        VkImageBlit b = { 0 };
        b.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, slevel, slayer, 1 };
        b.srcOffsets[0] = (VkOffset3D){ s[0], s[1], 0 };
        b.srcOffsets[1] = (VkOffset3D){ s[2], s[3], 1 };
        b.dstSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, dlevel, dlayer, 1 };
        b.dstOffsets[0] = (VkOffset3D){ d[0], d[1], 0 };
        b.dstOffsets[1] = (VkOffset3D){ d[2], d[3], 1 };
        vkCmdBlitImage(g_cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &b, linear && !src->nolinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    barrier_image(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src->rest, slevel, 1, slayer, 1);
    barrier_image(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst->rest, dlevel, 1, dlayer, 1);
    g_targets_changed = 1;
}

void g9_fill(G9Tex* t, uint32_t face, uint32_t level, const int32_t rect[4], uint32_t color)
{
    if (!t || !g_dev)
        return;
    G9Tex* save_rt[G9_MAX_RT];
    uint8_t save_face[G9_MAX_RT], save_level[G9_MAX_RT];
    G9Tex* save_ds = g_ds;
    memcpy(save_rt, g_rt, sizeof save_rt);
    memcpy(save_face, g_rt_face, sizeof save_face);
    memcpy(save_level, g_rt_level, sizeof save_level);
    G9Tex* one[G9_MAX_RT] = { t };
    uint8_t f[G9_MAX_RT] = { (uint8_t)face }, l[G9_MAX_RT] = { (uint8_t)level };
    g9_set_targets(one, f, l, NULL);
    uint32_t w, h, d;
    level_dims(t, level, &w, &h, &d);
    G9Viewport vp = { 0, 0, w, h, 0, 1 };
    g9_clear(rect ? 1 : 0, rect, 1, color, 0, 0, &vp, NULL);
    if (g_pending_clear)
        begin_pass();
    g9_set_targets(save_rt, save_face, save_level, save_ds);
}

/* --- pipelines ---------------------------------------------------------------------------------- */

struct PipeKey
{
    uint64_t vs, ps; /* the variants' keys, combined with the shader hashes */
    struct { uint16_t fmt; uint8_t binding, used; uint16_t offset, pad; } attr[16]; /* used: 1 a stream, 2 the default */
    uint32_t stride[G9_MAX_STREAMS];
    uint8_t step_instance[G9_MAX_STREAMS];
    uint16_t streams;
    uint8_t def_binding, ncolor;
    uint32_t color[G9_MAX_RT], depth;
    uint8_t blend, src, dst, op, sep, src_a, dst_a, op_a;
    uint8_t mask[G9_MAX_RT];
    uint8_t topology, cull, fill, z, zw, zf, st, two;
    uint8_t f[4], cf[4];
};

static VkFormat vertex_format(uint32_t type, uint8_t* is_int)
{
    *is_int = 0;
    switch (type)
    {
    case 0: return VK_FORMAT_R32_SFLOAT;
    case 1: return VK_FORMAT_R32G32_SFLOAT;
    case 2: return VK_FORMAT_R32G32B32_SFLOAT;
    case 3: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 4: return VK_FORMAT_B8G8R8A8_UNORM; /* D3DCOLOR */
    case 5: *is_int = 2; return VK_FORMAT_R8G8B8A8_UINT;
    case 6: *is_int = 1; return VK_FORMAT_R16G16_SINT;
    case 7: *is_int = 1; return VK_FORMAT_R16G16B16A16_SINT;
    case 8: return VK_FORMAT_R8G8B8A8_UNORM;
    case 9: return VK_FORMAT_R16G16_SNORM;
    case 10: return VK_FORMAT_R16G16B16A16_SNORM;
    case 11: return VK_FORMAT_R16G16_UNORM;
    case 12: return VK_FORMAT_R16G16B16A16_UNORM;
    case 13: *is_int = 3; return VK_FORMAT_R32_UINT; /* UDEC3, DEC3N: unpacked by the shader */
    case 14: *is_int = 4; return VK_FORMAT_R32_UINT;
    case 15: return VK_FORMAT_R16G16_SFLOAT;
    case 16: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static VkBlendFactor blend_factor(uint32_t f)
{
    switch (f)
    {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 14: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t op)
{
    switch (op)
    {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare(uint32_t f)
{
    switch (f)
    {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp stencil_op(uint32_t op)
{
    switch (op)
    {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static VkPipeline make_pipeline(const PipeKey* k, VkRenderPass rp, VkShaderModule vs, VkShaderModule ps)
{
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT, st[0].module = vs, st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, st[1].module = ps, st[1].pName = "main";
    VkVertexInputBindingDescription vb[G9_MAX_STREAMS + 1];
    VkVertexInputAttributeDescription va[16];
    uint32_t nb = 0, na = 0, has_def = 0;
    for (uint32_t r = 0; r < 16; ++r)
        if (k->attr[r].used)
        {
            va[na++] = (VkVertexInputAttributeDescription){ r, k->attr[r].binding, (VkFormat)k->attr[r].fmt, k->attr[r].offset };
            has_def |= k->attr[r].used == 2;
        }
    for (uint32_t s = 0; s < G9_MAX_STREAMS; ++s)
        if (k->streams & (1u << s))
            vb[nb++] = (VkVertexInputBindingDescription){ s, k->stride[s],
                k->step_instance[s] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX };
    if (has_def)
        vb[nb++] = (VkVertexInputBindingDescription){ k->def_binding, 0, VK_VERTEX_INPUT_RATE_VERTEX };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = nb, vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = na, vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = (VkPrimitiveTopology)k->topology;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = k->fill && g_feat.fillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    /* D3DCULL: NONE 1, CW 2 (cull clockwise), CCW 3; D3D9's triangles are clockwise when front, and the
     * picture in the framebuffer is D3D9's (sm_glsl.h) */
    rs.cullMode = k->cull == 2 ? VK_CULL_MODE_FRONT_BIT : k->cull == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = VK_TRUE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = k->z;
    ds.depthWriteEnable = k->zw;
    ds.depthCompareOp = k->z ? compare(k->zf) : VK_COMPARE_OP_ALWAYS;
    ds.stencilTestEnable = k->st;
    if (k->st)
    {
        /* D3D9's front face is clockwise; the counter-clockwise stencil operations apply to the back */
        ds.front.failOp = stencil_op(k->f[0]);
        ds.front.depthFailOp = stencil_op(k->f[1]);
        ds.front.passOp = stencil_op(k->f[2]);
        ds.front.compareOp = compare(k->f[3]);
        ds.back = ds.front;
        if (k->two)
        {
            ds.back.failOp = stencil_op(k->cf[0]);
            ds.back.depthFailOp = stencil_op(k->cf[1]);
            ds.back.passOp = stencil_op(k->cf[2]);
            ds.back.compareOp = compare(k->cf[3]);
        }
    }
    VkPipelineColorBlendAttachmentState cb[G9_MAX_RT];
    memset(cb, 0, sizeof cb);
    for (uint32_t i = 0; i < k->ncolor; ++i)
    {
        uint32_t src = g_feat.independentBlend ? i : 0;
        if (!k->color[i])
            continue;
        uint8_t m = k->mask[src];
        /* D3DCOLORWRITEENABLE: R 1, G 2, B 4, A 8, as Vulkan's */
        cb[i].colorWriteMask = m & 15;
        if (k->blend && (fmt_features((VkFormat)k->color[i]) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))
        {
            cb[i].blendEnable = VK_TRUE;
            cb[i].srcColorBlendFactor = blend_factor(k->src);
            cb[i].dstColorBlendFactor = blend_factor(k->dst);
            cb[i].colorBlendOp = blend_op(k->op);
            cb[i].srcAlphaBlendFactor = blend_factor(k->sep ? k->src_a : k->src);
            cb[i].dstAlphaBlendFactor = blend_factor(k->sep ? k->dst_a : k->dst);
            cb[i].alphaBlendOp = blend_op(k->sep ? k->op_a : k->op);
            if (k->src == 12) /* BOTHSRCALPHA */
                cb[i].srcColorBlendFactor = cb[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                cb[i].dstColorBlendFactor = cb[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            else if (k->src == 13) /* BOTHINVSRCALPHA */
                cb[i].srcColorBlendFactor = cb[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                cb[i].dstColorBlendFactor = cb[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            /* SRC_ALPHA_SATURATE is a colour factor only in Vulkan; for alpha it is one */
            if (cb[i].srcAlphaBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE)
                cb[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            if (cb[i].dstAlphaBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE)
                cb[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        }
    }
    if (!g_feat.independentBlend)
        for (uint32_t i = 1; i < k->ncolor; ++i)
            cb[i] = cb[0];
    VkPipelineColorBlendStateCreateInfo bs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    bs.attachmentCount = k->ncolor;
    bs.pAttachments = cb;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                             VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = sizeof dyn / sizeof dyn[0];
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &bs;
    pi.pDynamicState = &dy;
    pi.layout = g_layout;
    pi.renderPass = rp;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_dev, g_pcache, 1, &pi, NULL, &p) != VK_SUCCESS)
        p = VK_NULL_HANDLE;
    return p;
}

static int is_depth_format(const G9Tex* t) { return t && t->f.depth; }

static VkPrimitiveTopology draw_prim(uint32_t prim, uint32_t count, uint32_t* nverts)
{
    switch (prim)
    {
    case 1: *nverts = count; return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 2: *nverts = count * 2; return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: *nverts = count + 1; return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 5: *nverts = count + 2; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 6: *nverts = count * 3; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; /* fan: expanded to a list */
    default: *nverts = count * 3; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

/* --- per command buffer state, so a draw sets only what changed ---------------------------------- */

static struct
{
    VkPipeline pipe;
    VkViewport vp;
    VkRect2D sc;
    float bias[2];
    uint32_t blend, sref, sread, swrite;
    int valid;
} g_bound;

static void reset_bound_state(void) { memset(&g_bound, 0, sizeof g_bound); }

/* the constants of the last draw this frame, kept to skip copying the same again */
#define UBO_VS_SIZE ((uint32_t)sizeof(SmGlslVsConsts))
#define UBO_PS_SIZE ((uint32_t)sizeof(SmGlslPsConsts))
static struct
{
    uint8_t vs[sizeof(SmGlslVsConsts)], ps[sizeof(SmGlslPsConsts)], aux[sizeof(SmAux)];
    uint32_t vs_off, ps_off, aux_off;
    int valid;
} g_ubo;

static void ubo_reset(void) { g_ubo.valid = 0; }

static uint32_t ubo_put(uint8_t* last, uint32_t* last_off, const void* const* parts, const uint32_t* sizes, int nparts,
    uint32_t total)
{
    uint8_t tmp[sizeof(SmGlslVsConsts)];
    uint32_t o = 0;
    for (int i = 0; i < nparts; ++i)
        memcpy(tmp + o, parts[i], sizes[i]), o += sizes[i];
    if (g_ubo.valid && !memcmp(tmp, last, total))
        return *last_off;
    VkBuffer b;
    uint32_t off;
    uint8_t* p = (uint8_t*)ring(total, g_ubo_align, &b, &off);
    if (!p)
        return 0;
    memcpy(p, tmp, total);
    memcpy(last, tmp, total);
    *last_off = off;
    return off;
}

/* --- one-frame trace (XIV_GFXTRACE=<dir>; touch <dir>/req to trace the next frame), as on Metal but
 * shorter: the 2D A8R8G8B8 render targets the frame samples, at its end, as <dir>/rt_<n>_<w>x<h>.ppm
 * (n in the order the frame first samples them) and their alpha as .pgm; draws.txt a line per draw */
static const char* g_trace_dir;
static FILE* g_trace;
static uint32_t g_trace_draw, g_trace_nrt;
static G9Tex* g_trace_rt[32];

static void trace_rt(G9Tex* t)
{
    if (!g_trace || !t || t->fmt != F_A8R8G8B8 || t->type != G9_TEX_2D || t->use != G9_USE_RT)
        return;
    for (uint32_t i = 0; i < g_trace_nrt; ++i)
        if (g_trace_rt[i] == t)
            return;
    if (g_trace_nrt < 32)
        g_trace_rt[g_trace_nrt++] = t;
}

static void trace_frame_end(void)
{
    if (g_trace)
    {
        fclose(g_trace);
        g_trace = NULL;
        for (uint32_t i = 0; i < g_trace_nrt; ++i)
        {
            G9Tex* t = g_trace_rt[i];
            uint8_t* px = (uint8_t*)calloc((size_t)t->w * t->h, 4);
            g9_tex_read(t, 0, 0, px, t->w * 4);
            char q[1200];
            snprintf(q, sizeof q, "%s/rt_%02u_%ux%u.ppm", g_trace_dir, i, t->w, t->h);
            FILE* f = fopen(q, "wb");
            if (f)
            {
                fprintf(f, "P6 %u %u 255\n", t->w, t->h);
                for (size_t k = 0; k < (size_t)t->w * t->h; ++k)
                {
                    uint8_t rgb[3] = { px[4 * k + 2], px[4 * k + 1], px[4 * k] };
                    fwrite(rgb, 1, 3, f);
                }
                fclose(f);
            }
            snprintf(q, sizeof q, "%s/rt_%02u_%ux%u_alpha.pgm", g_trace_dir, i, t->w, t->h);
            if ((f = fopen(q, "wb")))
            {
                fprintf(f, "P5 %u %u 255\n", t->w, t->h);
                for (size_t k = 0; k < (size_t)t->w * t->h; ++k)
                    fwrite(&px[4 * k + 3], 1, 1, f);
                fclose(f);
            }
            free(px);
        }
        rt_log("[gfx9] trace: %u draws, %u render targets in %s\n", g_trace_draw, g_trace_nrt, g_trace_dir);
        g_trace_nrt = 0;
    }
    if (!g_trace_dir && !(g_trace_dir = getenv("XIV_GFXTRACE")))
        g_trace_dir = "";
    if (!*g_trace_dir)
        return;
    char p[1200];
    snprintf(p, sizeof p, "%s/req", g_trace_dir);
    if (remove(p) != 0)
        return;
    snprintf(p, sizeof p, "%s/draws.txt", g_trace_dir);
    g_trace = fopen(p, "w");
    g_trace_draw = 0;
}

static void trace_draw(const G9Draw* d, const SmOpts* po, const PipeKey* k)
{
    const SmInfo *vi = &d->vs->info, *pi = &d->ps->info;
    fprintf(g_trace, "#%u vs %08x ps %08x prim %u n %u inst %u rt %u/%u/%u/%u ds %u blend %d %d %d %d z %d/%d/%d cull %d "
                     "atest %d/%u mask %x vp %ux%u",
        g_trace_draw, vi->hash, pi->hash, d->prim, d->count, d->instances, k->color[0], k->color[1], k->color[2], k->color[3],
        k->depth, d->rs.blend, d->rs.src, d->rs.dst, d->rs.op, d->rs.zenable, d->rs.zwrite, d->rs.zfunc, d->rs.cull,
        d->rs.alpha_func, d->rs.alpha_ref, d->rs.write_mask[0], d->vp.w, d->vp.h);
    trace_rt(g_rt[0]);
    for (uint32_t s = 0; s < G9_PS_SAMPLERS; ++s)
    {
        if (!pi->sampler[s])
            continue;
        G9Tex* t = d->tex[s];
        trace_rt(t);
        if (t)
            fprintf(g_trace, " | s%u fmt %u %ux%u L%u%s%s", s, t->fmt, t->w, t->h, t->levels,
                (po->shadow_mask >> s) & 1 ? " SHADOW" : "", (po->depth_mask >> s) & 1 ? " DEPTH" : "");
        else
            fprintf(g_trace, " | s%u (none)", s);
    }
    fprintf(g_trace, "\n");
    g_trace_draw++;
}

/* --- draws -------------------------------------------------------------------------------------- */

void g9_draw(const G9Draw* d)
{
    if (!g_dev || !d->vs || !d->ps || !d->count || !d->vp.w || !d->vp.h)
        return;
    RPKey pk;
    if (!pass_key(&pk))
        return;
    /* --- the shader variants this draw needs (as gfx9_metal.m) --- */
    SmOpts vo, po;
    memset(&vo, 0, sizeof vo);
    memset(&po, 0, sizeof po);
    PipeKey k;
    memset(&k, 0, sizeof k);
    uint32_t streams_used = 0, need_def = 0;
    const SmInfo* vi = &d->vs->info;
    for (uint32_t i = 0; i < vi->nin; ++i)
    {
        const SmDecl* in = &vi->in[i];
        uint32_t reg = in->reg & 15;
        const G9Elem* e = NULL;
        for (uint32_t j = 0; j < d->nel; ++j)
            if (d->el[j].usage == in->usage && d->el[j].index == in->index)
            {
                e = &d->el[j];
                break;
            }
        if (!e && in->usage == SM_POSITION) /* POSITIONT feeds a shader's position too */
            for (uint32_t j = 0; j < d->nel; ++j)
                if (d->el[j].usage == SM_POSITIONT && d->el[j].index == in->index)
                    e = &d->el[j];
        uint8_t is_int;
        VkFormat f = e ? vertex_format(e->type, &is_int) : VK_FORMAT_UNDEFINED;
        if (e && e->stream < G9_MAX_STREAMS && d->stream[e->stream].stride && f != VK_FORMAT_UNDEFINED)
        {
            vo.attr_int[reg] = is_int;
            streams_used |= 1u << e->stream;
            k.attr[reg].fmt = (uint16_t)f, k.attr[reg].binding = e->stream, k.attr[reg].offset = e->offset;
            k.attr[reg].used = 1;
        }
        else
        {
            /* nothing supplies it: (0, 0, 0, 1) from the default buffer, stride 0 */
            k.attr[reg].fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
            k.attr[reg].used = 2;
            need_def = 1;
        }
    }
    uint32_t def = 15;
    if (need_def)
    {
        while (def > 0 && (streams_used & (1u << def)))
            def--;
        k.def_binding = (uint8_t)def;
        for (uint32_t r = 0; r < 16; ++r)
            if (k.attr[r].used == 2)
                k.attr[r].binding = (uint8_t)def;
    }
    for (uint32_t s = 0; s < G9_MAX_STREAMS; ++s)
        if (streams_used & (1u << s))
        {
            uint32_t stride = d->stream[s].stride;
            if (g_stride_align > 1)
                stride = (stride + g_stride_align - 1) / g_stride_align * g_stride_align;
            k.stride[s] = stride;
            if (d->stream[s].freq & 0x80000000u) /* D3DSTREAMSOURCE_INSTANCEDATA */
            {
                k.step_instance[s] = 1;
                static int told;
                if ((d->stream[s].freq & 0x3FFFFFFFu) > 1 && !told++)
                    rt_log("[gfx9] instance data every %u instances: not supported, every instance used\n",
                        d->stream[s].freq & 0x3FFFFFFFu);
            }
        }
    k.streams = (uint16_t)streams_used;
    /* what the pixel shader reads that the vertex shader does not write */
    const SmInfo* pi = &d->ps->info;
    for (uint32_t i = 0; i < pi->nin; ++i)
    {
        int found = 0;
        for (uint32_t j = 0; j < vi->nout; ++j)
            found |= vi->out[j].usage == pi->in[i].usage && vi->out[j].index == pi->in[i].index;
        if (!found && vo.nvs_extra < 16)
            vo.vs_extra[vo.nvs_extra++] = pi->in[i];
    }
    po.alpha_func = d->rs.alpha_func;
    po.no_depth_out = pi->writes_depth && !g_ds;
    for (uint32_t s = 0; s < G9_PS_SAMPLERS; ++s)
        if (is_depth_format(d->tex[s]) && pi->sampler[s] == 2)
        {
            if (d->tex[s]->f.depth_read)
                po.depth_mask |= (uint16_t)(1u << s);
            else
                po.shadow_mask |= (uint16_t)(1u << s);
        }
    int background = d->rs.zenable && g_ds; /* a draw of the 3D scene: background compiles */
    uint64_t tc = g_prof ? now_ns() : 0;
    Compiled *vc = shader_variant(d->vs, &vo, background), *pc = shader_variant(d->ps, &po, background);
    if (!background && (!compiled_ready(vc, 1) || !compiled_ready(pc, 1) || !vc->mod || !pc->mod))
        return;
    if (background && compiled_ready(vc, 0) && compiled_ready(pc, 0) && (!vc->mod || !pc->mod))
        return;
    k.vs = fnv64(&vo, sizeof vo) ^ ((uint64_t)vi->hash << 32);
    k.ps = fnv64(&po, sizeof po) ^ ((uint64_t)pi->hash << 32);
    k.ncolor = pk.ncolor;
    for (int i = 0; i < G9_MAX_RT; ++i)
    {
        k.color[i] = pk.color[i];
        k.mask[i] = pk.color[i] ? d->rs.write_mask[i] : 0;
    }
    k.depth = pk.depth;
    if (d->rs.blend)
    {
        k.blend = 1, k.src = d->rs.src, k.dst = d->rs.dst, k.op = d->rs.op;
        k.sep = d->rs.sep_alpha;
        if (k.sep)
            k.src_a = d->rs.src_a, k.dst_a = d->rs.dst_a, k.op_a = d->rs.op_a;
    }
    uint32_t n;
    k.topology = (uint8_t)draw_prim(d->prim, d->count, &n);
    k.cull = d->rs.cull;
    k.fill = d->rs.fill == 2;
    int has_stencil = g_ds && g_ds->f.stencil;
    k.z = g_ds && d->rs.zenable;
    k.zw = k.z && d->rs.zwrite;
    k.zf = k.z ? d->rs.zfunc : 8;
    k.st = has_stencil && d->rs.stencil;
    if (k.st)
    {
        k.two = d->rs.two_sided;
        k.f[0] = d->rs.sfail, k.f[1] = d->rs.szfail, k.f[2] = d->rs.spass, k.f[3] = d->rs.sfunc;
        if (k.two)
            k.cf[0] = d->rs.ccw_sfail, k.cf[1] = d->rs.ccw_szfail, k.cf[2] = d->rs.ccw_spass, k.cf[3] = d->rs.ccw_sfunc;
    }

    /* --- the pipeline --- */
    uint64_t h;
    Compiled* pcomp = (Compiled*)cache_get(&g_pipes, &k, sizeof k, &h);
    if (!pcomp)
    {
        pcomp = (Compiled*)calloc(1, sizeof *pcomp);
        cache_put(&g_pipes, &k, sizeof k, h, pcomp);
        Job* j = (Job*)calloc(1, sizeof *j);
        j->kind = 1, j->out = pcomp, j->vc = vc, j->pc = pc, j->hash = vi->hash, j->hash2 = pi->hash;
        j->key = (PipeKey*)malloc(sizeof k);
        *j->key = k;
        j->rp = render_pass(&pk);
        job_submit(j, background);
    }
    int ready = compiled_ready(pcomp, !background);
    if (g_prof)
        g_t_compile += now_ns() - tc;
    if (!ready)
        return;
    VkPipeline pipe = pcomp->pipe;
    if (!pipe)
        return;

    /* --- what the draw copies, reserved at once --- */
    uint32_t need = UBO_VS_SIZE + UBO_PS_SIZE + (uint32_t)sizeof(SmAux) + 4 * g_ubo_align;
    for (uint32_t s = 0; s < G9_MAX_STREAMS; ++s)
        if (streams_used & (1u << s))
        {
            const G9Stream* st = &d->stream[s];
            if (k.stride[s] != st->stride && st->data)
                need += (st->size / st->stride) * k.stride[s] + 16;
            else if (!st->buf && st->data)
                need += st->size + 16;
        }
    if (d->index_size || d->prim == 6)
        need += n * 4 + 16;
    ring_ensure(need);

    /* --- constants --- */
    uint32_t tw, th;
    target_size(&tw, &th);
    float vw = (float)d->vp.w, vh = (float)d->vp.h;
    /* D3D9's pixel centres are on integer coordinates, Vulkan's on half-integers: geometry moves half a
     * pixel right and down (sm_glsl.h) */
    SmAux aux;
    memset(&aux, 0, sizeof aux);
    aux.half_pixel[0] = 1.0f / (vw >= 1.0f ? vw : 1.0f);
    aux.half_pixel[1] = -1.0f / (vh >= 1.0f ? vh : 1.0f);
    aux.alpha_ref[0] = (d->rs.alpha_ref & 255) / 255.0f;
    for (uint32_t s = 0; s < G9_PS_SAMPLERS; ++s)
        aux.lod_bias[s] = d->samp[s].lod_bias;
    memcpy(aux.clip, d->clip, sizeof aux.clip);
    aux.clip_on[0] = d->clip_on;
    aux.rscale[0] = aux.rscale[1] = 1.0f;
    const void* vparts[3] = { d->vsc, d->vsi, d->vsb };
    const uint32_t vsizes[3] = { G9_VS_FLOATS * 16, 16 * 16, 16 * 4 };
    const void* pparts[3] = { d->psc, d->psi, d->psb };
    const uint32_t psizes[3] = { G9_PS_FLOATS * 16, 16 * 16, 16 * 4 };
    const void* aparts[1] = { &aux };
    const uint32_t asizes[1] = { (uint32_t)sizeof aux };
    uint32_t dyn[3];
    dyn[0] = ubo_put(g_ubo.vs, &g_ubo.vs_off, vparts, vsizes, 3, UBO_VS_SIZE);
    dyn[1] = ubo_put(g_ubo.ps, &g_ubo.ps_off, pparts, psizes, 3, UBO_PS_SIZE);
    dyn[2] = ubo_put(g_ubo.aux, &g_ubo.aux_off, aparts, asizes, 1, (uint32_t)sizeof aux);
    g_ubo.valid = 1;

    /* --- vertices and indices into the ring --- */
    VkBuffer vbuf[G9_MAX_STREAMS + 1];
    VkDeviceSize voff[G9_MAX_STREAMS + 1];
    for (uint32_t s = 0; s < G9_MAX_STREAMS; ++s)
    {
        vbuf[s] = g_default_attr, voff[s] = 0;
        if (!(streams_used & (1u << s)))
            continue;
        const G9Stream* st = &d->stream[s];
        if (k.stride[s] != st->stride)
        {
            /* the device's stride alignment: the vertices copied out at the padded stride */
            if (!st->data)
                return;
            uint32_t padded = k.stride[s], nv = st->size / st->stride, off;
            uint8_t* p = (uint8_t*)ring(nv * padded, 16, &vbuf[s], &off);
            if (!p)
                return;
            for (uint32_t v = 0; v < nv; ++v)
                memcpy(p + v * padded, (const uint8_t*)st->data + v * st->stride, st->stride);
            voff[s] = off;
        }
        else if (st->buf)
            vbuf[s] = st->buf->buf, voff[s] = st->offset;
        else if (st->data)
        {
            uint32_t off;
            void* p = ring(st->size, 16, &vbuf[s], &off);
            if (!p)
                return;
            memcpy(p, st->data, st->size);
            voff[s] = off;
        }
        else
            return;
    }
    uint32_t inst = d->instances ? d->instances : 1;
    VkBuffer ib = VK_NULL_HANDLE;
    uint32_t ioff = 0, first_index = 0;
    VkIndexType itype = VK_INDEX_TYPE_UINT32;
    if (d->index_size)
    {
        uint32_t isz = d->index_size;
        if (d->prim == 6 || !d->ibuf || (d->ibuf_offset % isz))
        {
            /* fans become lists; unaligned or client indices go through the ring */
            const uint8_t* src = (const uint8_t*)d->idata;
            if (!src)
                return;
            uint32_t* out = (uint32_t*)ring(n * 4, 16, &ib, &ioff);
            if (!out)
                return;
            for (uint32_t i = 0; i < n; ++i)
            {
                uint32_t at = d->prim == 6 ? (i % 3 == 0 ? 0 : i / 3 + i % 3) : i;
                out[i] = isz == 2 ? ((const uint16_t*)src)[d->first + at] : ((const uint32_t*)src)[d->first + at];
            }
        }
        else
        {
            ib = d->ibuf->buf;
            ioff = d->ibuf_offset;
            first_index = d->first;
            itype = isz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        }
    }
    else if (d->prim == 6)
    {
        uint32_t* out = (uint32_t*)ring(n * 4, 16, &ib, &ioff);
        if (!out)
            return;
        for (uint32_t i = 0; i < n; ++i)
            out[i] = d->first + (i % 3 == 0 ? 0 : i / 3 + i % 3);
    }

    /* --- textures --- */
    DsetKey dk;
    memset(&dk, 0, sizeof dk);
    for (uint32_t b = 0; b < G9_SAMPLERS; ++b)
    {
        int vs = b >= G9_PS_SAMPLERS;
        uint32_t s = vs ? b - G9_PS_SAMPLERS : b;
        uint8_t type = vs ? vi->sampler[s] : pi->sampler[s];
        if (!type || (vs && s >= G9_VS_SAMPLERS))
            continue;
        G9Tex* t = d->tex[b];
        int want = type == 3 ? G9_TEX_CUBE : type == 4 ? G9_TEX_3D : G9_TEX_2D;
        int shadow = !vs && ((po.shadow_mask >> s) & 1), depth = !vs && ((po.depth_mask >> s) & 1);
        if (!t || t->type != want || !t->view || (t->f.depth && !shadow && !depth))
            t = g_dummy[want == G9_TEX_CUBE ? 1 : want == G9_TEX_3D ? 2 : 0];
        if (!t->inited)
        {
            end_pass();
            tex_prepare(t);
        }
        VkImageView v = t->view;
        if (!vs && d->samp[s].srgb && t->f.vf == VK_FORMAT_B8G8R8A8_UNORM && !t->f.decode && t->type == G9_TEX_2D)
        {
            if (!t->srgb)
                t->srgb = view_new(t, VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0,
                    t->levels, 0, 1, 1);
            if (t->srgb)
                v = t->srgb;
        }
        else if (!vs && d->samp[s].srgb && t->f.vf == VK_FORMAT_B8G8R8A8_UNORM && !t->f.decode)
        {
            if (!t->srgb)
                t->srgb = view_new(t, VK_FORMAT_B8G8R8A8_SRGB,
                    t->type == G9_TEX_CUBE ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_ASPECT_COLOR_BIT, 0,
                    t->levels, 0, t->layers, 1);
            if (t->srgb)
                v = t->srgb;
        }
        dk.mask |= 1u << b;
        dk.view[b] = v;
        dk.samp[b] = sampler(&d->samp[b], shadow, t->nolinear);
        dk.layout[b] = t->rest;
    }
    VkDescriptorSet tset = tex_set(&dk);
    if (!tset)
        return;

    /* --- the pass and the commands --- */
    if (!begin_pass())
        return;
    if (g_trace)
        trace_draw(d, &po, &k);
    VkCommandBuffer cb = g_cb;
    if (g_bound.pipe != pipe)
    {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        g_bound.pipe = pipe;
    }
    VkDescriptorSet sets[2] = { g_fr[g_frame].ubo_set, tset };
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 2, sets, 3, dyn);
    VkViewport vp = { (float)d->vp.x, (float)d->vp.y, vw, vh, d->vp.minz, d->vp.maxz };
    if (!g_bound.valid || memcmp(&vp, &g_bound.vp, sizeof vp))
        vkCmdSetViewport(cb, 0, 1, &vp), g_bound.vp = vp;
    VkRect2D sc = { { 0, 0 }, { g_fb_w, g_fb_h } };
    if (d->rs.scissor)
    {
        int32_t l = d->scissor[0] > 0 ? d->scissor[0] : 0, t = d->scissor[1] > 0 ? d->scissor[1] : 0;
        int32_t r = d->scissor[2] < (int32_t)g_fb_w ? d->scissor[2] : (int32_t)g_fb_w;
        int32_t b = d->scissor[3] < (int32_t)g_fb_h ? d->scissor[3] : (int32_t)g_fb_h;
        if (r <= l || b <= t)
            return; /* nothing visible */
        sc = (VkRect2D){ { l, t }, { (uint32_t)(r - l), (uint32_t)(b - t) } };
    }
    if (!g_bound.valid || memcmp(&sc, &g_bound.sc, sizeof sc))
        vkCmdSetScissor(cb, 0, 1, &sc), g_bound.sc = sc;
    float bias[2] = { d->rs.depth_bias * 16777215.0f, d->rs.slope_bias };
    if (!g_bound.valid || memcmp(bias, g_bound.bias, sizeof bias))
        vkCmdSetDepthBias(cb, bias[0], 0.0f, bias[1]), memcpy(g_bound.bias, bias, sizeof bias);
    uint32_t bf = d->rs.blend_factor;
    if (!g_bound.valid || bf != g_bound.blend)
    {
        float c[4] = { ((bf >> 16) & 255) / 255.0f, ((bf >> 8) & 255) / 255.0f, (bf & 255) / 255.0f, (bf >> 24) / 255.0f };
        vkCmdSetBlendConstants(cb, c);
        g_bound.blend = bf;
    }
    uint32_t sref = d->rs.stencil_ref & 0xFF;
    if (!g_bound.valid || sref != g_bound.sref)
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sref), g_bound.sref = sref;
    if (!g_bound.valid || d->rs.sread != g_bound.sread)
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, d->rs.sread), g_bound.sread = d->rs.sread;
    if (!g_bound.valid || d->rs.swrite != g_bound.swrite)
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, d->rs.swrite), g_bound.swrite = d->rs.swrite;
    g_bound.valid = 1;
    for (uint32_t s = 0; s <= G9_MAX_STREAMS - 1; ++s)
        if ((streams_used & (1u << s)) || (need_def && s == def))
            vkCmdBindVertexBuffers(cb, s, 1, &vbuf[s], &voff[s]);
    if (ib)
    {
        vkCmdBindIndexBuffer(cb, ib, ioff, itype);
        vkCmdDrawIndexed(cb, n, inst, first_index, d->index_size ? d->base_vertex : 0, 0);
    }
    else
        vkCmdDraw(cb, n, inst, d->first, 0);
    g_n_draws++;
    if (g_pass_timed)
        g_fr[g_frame].pass_draws[g_fr[g_frame].npass]++;
}

/* --- the swapchain and present ------------------------------------------------------------------ */

static VkSwapchainKHR g_swap;
static VkFormat g_swap_format;
static VkExtent2D g_swap_extent;
static uint32_t g_swap_req_w, g_swap_req_h; /* the window's size the swapchain was made for */
static uint32_t g_nswap;
static VkImage g_swap_img[8];
static VkImageView g_swap_view[8];
static VkFramebuffer g_swap_fb[8];
static VkSemaphore g_swap_done[8];
static VkRenderPass g_present_rp;
static VkPipeline g_present_pipe;
static VkShaderModule g_util_vs, g_util_fs;

static const char UTIL_VS[] =
    "#version 450\n"
    "layout(push_constant) uniform P { vec4 r; vec4 uv; } p;\n"
    "layout(location = 0) out vec2 o_uv;\n"
    "void main() {\n"
    "    vec2 c = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);\n"
    "    gl_Position = vec4(mix(p.r.xy, p.r.zw, c), 0.0, 1.0);\n"
    "    o_uv = mix(p.uv.xy, p.uv.zw, c);\n"
    "}\n";
static const char UTIL_FS[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D t;\n"
    "layout(location = 0) in vec2 i_uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = texture(t, i_uv); }\n";

static VkShaderModule util_module(const char* src, int pixel)
{
    char err[1024];
    size_t n = 0;
    uint32_t* spv = glsl_to_spirv(src, pixel, &n, err, sizeof err);
    if (!spv)
    {
        rt_log("[gfx9] utility shader: %s\n", err);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = n * 4;
    mi.pCode = spv;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g_dev, &mi, NULL, &m);
    free(spv);
    return m;
}

/* a full-screen-quad pipeline in rp: its shaders, layout and blending (NULL: none) */
static VkPipeline util_pipeline_ex(VkRenderPass rp, VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout,
    const VkPipelineColorBlendAttachmentState* blend)
{
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT, st[0].module = vs, st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, st[1].module = fs, st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ca = { 0 };
    ca.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo bs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    bs.attachmentCount = 1;
    bs.pAttachments = blend ? blend : &ca;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pColorBlendState = &bs;
    pi.pDynamicState = &dy;
    pi.layout = layout;
    pi.renderPass = rp;
    VkPipeline p = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(g_dev, g_pcache, 1, &pi, NULL, &p);
    return p;
}

static VkPipeline util_pipeline(VkRenderPass rp) { return util_pipeline_ex(rp, g_util_vs, g_util_fs, g_util_playout, NULL); }

#include "gfx9_touch_vk.h" /* tests/ios_touch.c's controls */

static void swap_destroy(void)
{
    for (uint32_t i = 0; i < g_nswap; ++i)
    {
        vkDestroyFramebuffer(g_dev, g_swap_fb[i], NULL);
        vkDestroyImageView(g_dev, g_swap_view[i], NULL);
        vkDestroySemaphore(g_dev, g_swap_done[i], NULL);
    }
    g_nswap = 0;
}

static int swap_create(uint32_t w, uint32_t h)
{
    g_swap_req_w = w, g_swap_req_h = h;
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surface, &caps);
    VkExtent2D ext = { w ? w : 1280, h ? h : 720 };
    if (caps.currentExtent.width != 0xFFFFFFFFu)
        ext = caps.currentExtent;
    if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
    if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    if (caps.maxImageExtent.width && ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
    if (caps.maxImageExtent.height && ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    if (!ext.width || !ext.height)
        return 0;
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64)
        nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &nf, fmts);
    VkSurfaceFormatKHR sf = nf ? fmts[0] : (VkSurfaceFormatKHR){ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    for (uint32_t i = 0; i < nf; ++i)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            sf = fmts[i];
            break;
        }
        else if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            sf = fmts[i];
    uint32_t np = 0;
    VkPresentModeKHR modes[16];
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &np, NULL);
    if (np > 16)
        np = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &np, modes);
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
    if (!g_vsync)
        for (uint32_t i = 0; i < np; ++i)
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR || (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR && pm == VK_PRESENT_MODE_FIFO_KHR))
                pm = modes[i];
    uint32_t count = caps.minImageCount + 1;
    if (count < 3)
        count = 3;
    if (caps.maxImageCount && count > caps.maxImageCount)
        count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = g_surface;
    ci.minImageCount = count;
    ci.imageFormat = sf.format;
    ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                         : caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                        : (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                                                                                            : VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    ci.presentMode = pm;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g_swap;
    VkSwapchainKHR sw;
    VkResult r = vkCreateSwapchainKHR(g_dev, &ci, NULL, &sw);
    if (g_swap)
        vkDestroySwapchainKHR(g_dev, g_swap, NULL);
    g_swap = VK_NULL_HANDLE;
    if (r != VK_SUCCESS)
    {
        rt_log("[gfx9] vkCreateSwapchainKHR: %d\n", r);
        return 0;
    }
    g_swap = sw;
    g_swap_extent = ext;
    vkGetSwapchainImagesKHR(g_dev, sw, &g_nswap, NULL);
    if (g_nswap > 8)
        g_nswap = 8;
    vkGetSwapchainImagesKHR(g_dev, sw, &g_nswap, g_swap_img);
    if (sf.format != g_swap_format || !g_present_rp)
    {
        g_swap_format = sf.format;
        RPKey k;
        memset(&k, 0, sizeof k);
        k.color[0] = (uint32_t)sf.format, k.ncolor = 1, k.cload = 1, k.present = 1;
        g_present_rp = render_pass(&k);
        g_present_pipe = util_pipeline(g_present_rp);
    }
    for (uint32_t i = 0; i < g_nswap; ++i)
    {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = g_swap_img[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = sf.format;
        vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(g_dev, &vi, NULL, &g_swap_view[i]);
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = g_present_rp;
        fi.attachmentCount = 1;
        fi.pAttachments = &g_swap_view[i];
        fi.width = ext.width, fi.height = ext.height, fi.layers = 1;
        vkCreateFramebuffer(g_dev, &fi, NULL, &g_swap_fb[i]);
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(g_dev, &si, NULL, &g_swap_done[i]);
    }
    rt_log("[gfx9] swapchain %ux%u, %u images, format %d, %s\n", ext.width, ext.height, g_nswap, sf.format,
        pm == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : pm == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate");
    return 1;
}

static void swap_recreate(void)
{
    submit(1, VK_NULL_HANDLE, VK_NULL_HANDLE);
    vkDeviceWaitIdle(g_dev);
    swap_destroy();
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels((SDL_Window*)g_sdl_window, &w, &h);
    swap_create((uint32_t)(w > 0 ? w : 1), (uint32_t)(h > 0 ? h : 1));
}

/* --- pipeline cache on disk ------------------------------------------------------------------------ */

static void pcache_save(void)
{
    if (!g_cache_dir[0] || !g_pcache)
        return;
    size_t n = 0;
    if (vkGetPipelineCacheData(g_dev, g_pcache, &n, NULL) != VK_SUCCESS || !n)
        return;
    void* data = malloc(n);
    if (vkGetPipelineCacheData(g_dev, g_pcache, &n, data) == VK_SUCCESS)
    {
        char path[1200], tmp[1300];
        snprintf(path, sizeof path, "%s/pipelines.bin", g_cache_dir);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE* f = fopen(tmp, "wb");
        if (f)
        {
            int ok = fwrite(data, 1, n, f) == n;
            if (fclose(f) == 0 && ok)
                rename(tmp, path);
        }
    }
    free(data);
}

void g9_present(G9Tex* bb)
{
    if (!g_dev)
        return;
    if (g_pending_clear)
        begin_pass();
    end_pass();
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels((SDL_Window*)g_sdl_window, &w, &h);
    if (w > 0 && h > 0 && ((uint32_t)w != g_swap_req_w || (uint32_t)h != g_swap_req_h))
    {
        rt_log("[gfx9] window %dx%d pixels, back buffer %ux%u\n", w, h, bb ? bb->w : 0, bb ? bb->h : 0);
        swap_recreate();
    }
    if (!g_swap)
    {
        submit(0, VK_NULL_HANDLE, VK_NULL_HANDLE);
        return;
    }
    VkCommandBuffer cb = cmd();
    Frame* f = &g_fr[g_frame];
    uint32_t img = 0;
    uint64_t t0 = g_prof ? now_ns() : 0;
    VkResult r = vkAcquireNextImageKHR(g_dev, g_swap, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &img);
    if (g_prof)
        g_t_acquire += now_ns() - t0;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR))
    {
        swap_recreate();
        return;
    }
    if (bb)
        tex_prepare(bb);
    int touch = touch_count() > 0 && touch_ready(g_present_rp);
    VkClearValue cv = { { { 0, 0, 0, 1 } } };
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = g_present_rp;
    bi.framebuffer = g_swap_fb[img];
    bi.renderArea.extent = g_swap_extent;
    bi.clearValueCount = 1;
    bi.pClearValues = &cv;
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    if (bb && bb->view && g_present_pipe)
    {
        float dw = (float)g_swap_extent.width, dh = (float)g_swap_extent.height;
        VkViewport vp = { 0, 0, dw, dh, 0, 1 };
        VkRect2D sc = { { 0, 0 }, g_swap_extent };
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_present_pipe);
        VkDescriptorSet ds = dset_alloc(g_util_layout);
        VkDescriptorImageInfo ii = { g_linear, bb->view, bb->rest };
        VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wr.dstSet = ds;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &ii;
        vkUpdateDescriptorSets(g_dev, 1, &wr, 0, NULL);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_util_playout, 0, 1, &ds, 0, NULL);
        /* the back buffer scaled to the window, aspect kept */
        float s = fminf(dw / bb->w, dh / bb->h), qw = bb->w * s / dw, qh = bb->h * s / dh;
        float pc[8] = { -qw, -qh, qw, qh, 0, 0, 1, 1 };
        vkCmdPushConstants(cb, g_util_playout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
        vkCmdDraw(cb, 4, 1, 0, 0);
    }
    if (touch)
    {
        /* the shapes come in the window's pixels (ios_touch.c's points times its density) */
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels((SDL_Window*)g_sdl_window, &pw, &ph);
        draw_touch(cb, g_swap_extent.width, g_swap_extent.height, pw > 0 ? (float)g_swap_extent.width / (float)pw : 1.0f);
    }
    vkCmdEndRenderPass(cb);
    reset_bound_state();
    VkSemaphore done = g_swap_done[img];
    submit(0, f->acquired, done);
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swap;
    pi.pImageIndices = &img;
    t0 = g_prof ? now_ns() : 0;
    r = vkQueuePresentKHR(g_queue, &pi);
    if (g_prof)
    {
        static uint32_t frames;
        g_t_present += now_ns() - t0;
        if (++frames % 600 == 0)
        {
            rt_log("[gfx9] GPU %.2f ms a command buffer\n", g_n_gpu ? g_t_gpu / 1e6 / g_n_gpu : 0.0);
            if (g_prof > 1)
                prof_pass_report((uint32_t)(g_n_gpu ? g_n_gpu : 1));
            g_t_gpu = g_n_gpu = 0;
            rt_log("[gfx9] per frame: fence %.2f ms, acquire %.2f, submit %.2f (%.1f), present %.2f, compile waits %.2f; "
                   "%.0f passes, %.0f draws, %.0f barriers\n",
                g_t_fence / 6e8, g_t_acquire / 6e8, g_t_submit / 6e8, g_n_submits / 600.0, g_t_present / 6e8,
                g_t_compile / 6e8, g_n_passes / 600.0, g_n_draws / 600.0, g_n_barriers / 600.0);
            g_t_fence = g_t_acquire = g_t_submit = g_t_present = g_t_compile = g_n_submits = g_n_passes = g_n_draws = g_n_barriers = 0;
        }
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
        swap_recreate();
    trace_frame_end();
    uint32_t made = __atomic_load_n(&g_pipes_made, __ATOMIC_RELAXED);
    if (made >= g_pipes_saved + 200)
    {
        g_pipes_saved = made;
        pcache_save();
    }
}

void g9_overlay(G9Tex* backbuffer) { (void)backbuffer; }

void g9_resize(uint32_t w, uint32_t h) { (void)w, (void)h; /* the swapchain follows the window; present scales into it */ }

void g9_finish(void)
{
    if (g_dev)
        submit(1, VK_NULL_HANDLE, VK_NULL_HANDLE);
}

void (*g9_pace_hook)(uint32_t fps);
uint64_t g9_pace_refresh_ns(void) { return 0; }

/* the navmesh overlay (navdraw.c, compiled into the Metal back end): not drawn here */
volatile int navdraw_on;
int navdraw_key(int sc, int down, int shift)
{
    (void)sc, (void)down, (void)shift;
    return 0;
}

/* --- init --------------------------------------------------------------------------------------- */

static VkBool32 VKAPI_PTR debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
    (void)type, (void)user;
    static uint32_t n;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && n++ < 400)
        rt_log("[vk] %s\n", data->pMessage);
    return VK_FALSE;
}

typedef struct SurfArgs
{
    SDL_Window* w;
    int ok;
} SurfArgs;

static void surface_main(void* p)
{
    SurfArgs* a = (SurfArgs*)p;
#if defined(__APPLE__)
    /* SDL's Metal view (as the Metal back end makes it) and MoltenVK's surface on its layer */
    SDL_MetalView view = SDL_Metal_CreateView(a->w);
    void* layer = view ? SDL_Metal_GetLayer(view) : NULL;
    PFN_vkCreateMetalSurfaceEXT create = (PFN_vkCreateMetalSurfaceEXT)g_gipa(g_inst, "vkCreateMetalSurfaceEXT");
    VkMetalSurfaceCreateInfoEXT ci = { VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT };
    ci.pLayer = layer;
    a->ok = layer && create && create(g_inst, &ci, NULL, &g_surface) == VK_SUCCESS;
#else
    a->ok = SDL_Vulkan_CreateSurface(a->w, g_inst, NULL, &g_surface);
#endif
}

static int has_ext(const VkExtensionProperties* e, uint32_t n, const char* name)
{
    for (uint32_t i = 0; i < n; ++i)
        if (!strcmp(e[i].extensionName, name))
            return 1;
    return 0;
}

static void cache_dir_init(void)
{
    const char* d = getenv("XIV_VK_CACHE");
    if (d && !*d)
        return; /* XIV_VK_CACHE= : no disk cache */
    if (d)
        snprintf(g_cache_dir, sizeof g_cache_dir, "%s", d);
    else
    {
#if defined(__APPLE__)
        const char* home = getenv("HOME");
        if (!home)
            return;
        char p[900];
        snprintf(p, sizeof p, "%s/Library/Caches/FFXIV", home);
        mkdir(p, 0755);
        snprintf(g_cache_dir, sizeof g_cache_dir, "%s/vulkan", p);
#else
        char* pref = SDL_GetPrefPath("", "ffxiv");
        if (!pref)
            return;
        snprintf(g_cache_dir, sizeof g_cache_dir, "%svulkan", pref);
        SDL_free(pref);
#endif
    }
    mkdir(g_cache_dir, 0755);
    char p[1100];
    snprintf(p, sizeof p, "%s/spv", g_cache_dir);
    mkdir(p, 0755);
}

int g9_init(void* sdl_window, int vsync)
{
    g_sdl_window = sdl_window;
    g_vsync = vsync;
    g_prof = getenv("XIV_VK_PROF") ? atoi(getenv("XIV_VK_PROF")) : 0;
#if defined(__APPLE__)
    /* SDL is built without its Vulkan support on Apple (the Metal back end needs none): the loader is
     * linked in (Homebrew's vulkan-loader with MoltenVK), or found by name */
    g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr");
    static const char* const libs[] = { "libvulkan.1.dylib", "/opt/homebrew/lib/libvulkan.1.dylib", "libMoltenVK.dylib" };
    for (int i = 0; i < 3 && !g_gipa; ++i)
    {
        void* h = dlopen(libs[i], RTLD_NOW | RTLD_LOCAL);
        if (h)
            g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    }
    /* MoltenVK encodes a command buffer into Metal inside vkQueueSubmit, on the calling thread unless
     * told to submit from its own (9 ms a frame in a town, on the render thread); nothing here depends
     * on which */
    setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0", 0);
    /* and binds descriptor sets through Metal argument buffers by default, which cost the GPU 5 ms a
     * frame in a town (a set of twenty textures per draw) */
    setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 0);
    uint32_t nsdl = 2;
    static const char* const sdl_ext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME };
#else
    if (!SDL_Vulkan_LoadLibrary(NULL))
    {
        rt_log("[gfx9] no Vulkan loader: %s\n", SDL_GetError());
        return 0;
    }
    g_gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    uint32_t nsdl = 0;
    const char* const* sdl_ext = SDL_Vulkan_GetInstanceExtensions(&nsdl);
#endif
    if (!g_gipa)
    {
        rt_log("[gfx9] no Vulkan loader\n");
        return 0;
    }
#define VKF_LOAD_G(f) f = (PFN_##f)g_gipa(VK_NULL_HANDLE, #f);
    VKF_GLOBAL(VKF_LOAD_G)

    /* the instance */
    const char* iext[16];
    uint32_t niext = 0;
    for (uint32_t i = 0; i < nsdl && niext < 12; ++i)
        iext[niext++] = sdl_ext[i];
    uint32_t nav = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &nav, NULL);
    VkExtensionProperties* av = (VkExtensionProperties*)calloc(nav + 1, sizeof *av);
    vkEnumerateInstanceExtensionProperties(NULL, &nav, av);
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    if (has_ext(av, nav, "VK_KHR_portability_enumeration"))
    {
        iext[niext++] = "VK_KHR_portability_enumeration";
        ici.flags |= 0x00000001; /* VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR */
    }
    const char* val = getenv("XIV_VK_VALIDATE");
    int validate = val && strcmp(val, "0");
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (validate && has_ext(av, nav, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        iext[niext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    free(av);
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "FINAL FANTASY XIV";
    app.pEngineName = "gfx9_vulkan";
    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = niext;
    ici.ppEnabledExtensionNames = iext;
    if (validate)
        ici.enabledLayerCount = 1, ici.ppEnabledLayerNames = &layer;
    VkResult r = vkCreateInstance(&ici, NULL, &g_inst);
    if (r != VK_SUCCESS && validate)
    {
        rt_log("[gfx9] no validation layer (%d)\n", r);
        ici.enabledLayerCount = 0;
        r = vkCreateInstance(&ici, NULL, &g_inst);
    }
    if (r != VK_SUCCESS)
    {
        rt_log("[gfx9] vkCreateInstance: %d\n", r);
        return 0;
    }
#define VKF_LOAD_I(f) f = (PFN_##f)g_gipa(g_inst, #f);
    VKF_INSTANCE(VKF_LOAD_I)
    p_vkCreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)g_gipa(g_inst, "vkCreateDebugUtilsMessengerEXT");
    if (validate && p_vkCreateDebugUtilsMessengerEXT)
    {
        VkDebugUtilsMessengerCreateInfoEXT di = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        di.pfnUserCallback = debug_cb;
        VkDebugUtilsMessengerEXT m;
        p_vkCreateDebugUtilsMessengerEXT(g_inst, &di, NULL, &m);
    }

    /* the window's surface (made on the main thread, where AppKit wants its views made) */
    SurfArgs sa = { (SDL_Window*)sdl_window, 0 };
    sdl_main_run(surface_main, &sa);
    if (!sa.ok)
    {
        rt_log("[gfx9] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return 0;
    }

    /* a device with a queue that draws and presents */
    uint32_t nphys = 0;
    vkEnumeratePhysicalDevices(g_inst, &nphys, NULL);
    VkPhysicalDevice phys[8];
    if (nphys > 8)
        nphys = 8;
    vkEnumeratePhysicalDevices(g_inst, &nphys, phys);
    for (uint32_t i = 0; i < nphys && !g_phys; ++i)
    {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &nq, NULL);
        VkQueueFamilyProperties qp[16];
        if (nq > 16)
            nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &nq, qp);
        for (uint32_t q = 0; q < nq; ++q)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(phys[i], q, g_surface, &present);
            if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
            {
                g_phys = phys[i], g_qfam = q;
                break;
            }
        }
    }
    if (!g_phys)
    {
        rt_log("[gfx9] no Vulkan device can draw to the window\n");
        return 0;
    }
    vkGetPhysicalDeviceProperties(g_phys, &g_props);
    vkGetPhysicalDeviceMemoryProperties(g_phys, &g_memprops);
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(g_phys, &have);
    uint32_t ndev = 0;
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &ndev, NULL);
    VkExtensionProperties* dext = (VkExtensionProperties*)calloc(ndev + 1, sizeof *dext);
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &ndev, dext);
    const char* want[8];
    uint32_t nwant = 0;
    want[nwant++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (has_ext(dext, ndev, "VK_KHR_portability_subset"))
        want[nwant++] = "VK_KHR_portability_subset", g_portability = 1;
    if (has_ext(dext, ndev, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME))
        want[nwant++] = VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, g_mirror_once = 1;
    if (has_ext(dext, ndev, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME))
        want[nwant++] = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME, g_format_list = 1;
    int float_ext = has_ext(dext, ndev, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
    if (float_ext)
        want[nwant++] = VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME;
    free(dext);
    /* float controls: D3D9's NaN and infinities kept through the compiler's optimisations, where the
     * device can promise that (Metal's safe math mode) */
    if (float_ext || g_props.apiVersion >= VK_API_VERSION_1_2)
    {
        VkPhysicalDeviceFloatControlsPropertiesKHR fc = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES_KHR };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        p2.pNext = &fc;
        vkGetPhysicalDeviceProperties2(g_phys, &p2);
        g_float_controls = float_ext && fc.shaderSignedZeroInfNanPreserveFloat32;
    }
    memset(&g_feat, 0, sizeof g_feat);
    const char* nobc = getenv("XIV_VK_NOBC");
    g_feat.textureCompressionBC = have.textureCompressionBC && !(nobc && strcmp(nobc, "0"));
    g_feat.shaderClipDistance = have.shaderClipDistance;
    g_feat.samplerAnisotropy = have.samplerAnisotropy;
    g_feat.fillModeNonSolid = have.fillModeNonSolid;
    g_feat.largePoints = have.largePoints;
    g_feat.independentBlend = have.independentBlend;
    g_bc = g_feat.textureCompressionBC;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = g_qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = nwant;
    dci.ppEnabledExtensionNames = want;
    dci.pEnabledFeatures = &g_feat;
    r = vkCreateDevice(g_phys, &dci, NULL, &g_dev);
    if (r != VK_SUCCESS)
    {
        rt_log("[gfx9] vkCreateDevice: %d\n", r);
        g_dev = VK_NULL_HANDLE;
        return 0;
    }
#define VKF_LOAD_D(f) f = (PFN_##f)vkGetDeviceProcAddr(g_dev, #f);
    VKF_DEVICE(VKF_LOAD_D)
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
    g_ubo_align = (uint32_t)g_props.limits.minUniformBufferOffsetAlignment;
    if (g_ubo_align < 16)
        g_ubo_align = 16;

    /* depth formats: D3D9's 24-bit depth as 32-bit float, as on Metal, where the device has it */
    VkFormatFeatureFlags dsf = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((fmt_features(VK_FORMAT_D32_SFLOAT_S8_UINT) & dsf) != dsf)
        g_ds_format = VK_FORMAT_D24_UNORM_S8_UINT;
    if ((fmt_features(VK_FORMAT_D32_SFLOAT) & dsf) != dsf)
        g_d_format = VK_FORMAT_X8_D24_UNORM_PACK32;
    if (!(buf_features(VK_FORMAT_B8G8R8A8_UNORM) & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
        rt_log("[gfx9] the device has no BGRA8 vertex format: D3DCOLOR vertices will be wrong\n");

    /* the shader compiler */
    glslang_initialize_process();
    snprintf(g_preamble[0], sizeof g_preamble[0], "%s", g_feat.shaderClipDistance ? "" : "#define SM_NO_CLIP_DISTANCE 1\n");
    if (g_float_controls)
        for (int i = 0; i < 2; ++i)
            strncat(g_preamble[i],
                "#extension GL_EXT_spirv_intrinsics : require\n"
                "spirv_execution_mode(extensions = [\"SPV_KHR_float_controls\"], capabilities = [4466], 4461, 32);\n",
                sizeof g_preamble[i] - strlen(g_preamble[i]) - 1);
    g_dump_dir = getenv("XIV_VK_DUMP");
    cache_dir_init();

    /* frames */
    for (int i = 0; i < FRAMES; ++i)
    {
        Frame* f = &g_fr[i];
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = g_qfam;
        vkCreateCommandPool(g_dev, &pi, NULL, &f->pool);
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = f->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(g_dev, &ai, &f->cb);
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(g_dev, &fi, NULL, &f->fence);
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(g_dev, &si, NULL, &f->acquired);
        if (g_prof)
        {
            VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2 + 2 * PROF_PASSES;
            vkCreateQueryPool(g_dev, &qi, NULL, &f->qp);
        }
        f->ring = buffer_new(RING_SIZE,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &f->ring_mem);
        if (!f->ring)
        {
            rt_log("[gfx9] cannot make the upload ring\n");
            return 0;
        }
        f->ring_map = f->ring_mem.b->map + f->ring_mem.off;
    }

    /* layouts: set 0 the constants (dynamic uniform buffers in the ring), set 1 the textures */
    VkDescriptorSetLayoutBinding ub[3] = {
        { SM_GLSL_UBO_VS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
        { SM_GLSL_UBO_PS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { SM_GLSL_UBO_AUX, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 3;
    li.pBindings = ub;
    vkCreateDescriptorSetLayout(g_dev, &li, NULL, &g_ubo_layout);
    VkDescriptorSetLayoutBinding tb[G9_SAMPLERS];
    for (uint32_t i = 0; i < G9_SAMPLERS; ++i)
        tb[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            i < G9_PS_SAMPLERS ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT, NULL };
    li.bindingCount = G9_SAMPLERS;
    li.pBindings = tb;
    vkCreateDescriptorSetLayout(g_dev, &li, NULL, &g_tex_layout);
    VkDescriptorSetLayoutBinding utb = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    li.bindingCount = 1;
    li.pBindings = &utb;
    vkCreateDescriptorSetLayout(g_dev, &li, NULL, &g_util_layout);
    VkDescriptorSetLayout sl[2] = { g_ubo_layout, g_tex_layout };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sl;
    vkCreatePipelineLayout(g_dev, &pli, NULL, &g_layout);
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 32 };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &g_util_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(g_dev, &pli, NULL, &g_util_playout);
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * FRAMES };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.maxSets = FRAMES;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    vkCreateDescriptorPool(g_dev, &dpi, NULL, &g_static_pool);
    for (int i = 0; i < FRAMES; ++i)
    {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g_static_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g_ubo_layout;
        vkAllocateDescriptorSets(g_dev, &ai, &g_fr[i].ubo_set);
        VkDescriptorBufferInfo bi[3] = { { g_fr[i].ring, 0, UBO_VS_SIZE }, { g_fr[i].ring, 0, UBO_PS_SIZE },
                                         { g_fr[i].ring, 0, sizeof(SmAux) } };
        VkWriteDescriptorSet w[3];
        for (int b = 0; b < 3; ++b)
        {
            w[b] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[b].dstSet = g_fr[i].ubo_set;
            w[b].dstBinding = (uint32_t)b;
            w[b].descriptorCount = 1;
            w[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            w[b].pBufferInfo = &bi[b];
        }
        vkUpdateDescriptorSets(g_dev, 3, w, 0, NULL);
    }
    g_dsets = (DsetEnt*)calloc(DSET_CACHE, sizeof *g_dsets);

    /* the pipeline cache, from the last run */
    {
        VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        void* data = NULL;
        if (g_cache_dir[0])
        {
            char path[1200];
            snprintf(path, sizeof path, "%s/pipelines.bin", g_cache_dir);
            FILE* f = fopen(path, "rb");
            if (f)
            {
                fseek(f, 0, SEEK_END);
                long n = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (n > 0 && (data = malloc((size_t)n)) && fread(data, 1, (size_t)n, f) == (size_t)n)
                    ci.initialDataSize = (size_t)n, ci.pInitialData = data;
                fclose(f);
            }
        }
        if (vkCreatePipelineCache(g_dev, &ci, NULL, &g_pcache) != VK_SUCCESS)
        {
            ci.initialDataSize = 0, ci.pInitialData = NULL;
            vkCreatePipelineCache(g_dev, &ci, NULL, &g_pcache);
        }
        free(data);
    }

    /* samplers, the default attribute, the textures a missing one reads, the present pipeline */
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.25f;
    vkCreateSampler(g_dev, &sci, NULL, &g_linear);
    g_default_attr = buffer_new(16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g_default_attr_mem);
    static const float def[4] = { 0, 0, 0, 1 };
    memcpy(g_default_attr_mem.b->map + g_default_attr_mem.off, def, sizeof def);
    static const uint8_t zero[16] = { 0 };
    g_dummy[0] = g9_tex_create(G9_TEX_2D, F_A8R8G8B8, 1, 1, 1, 1, G9_USE_SAMPLE);
    g_dummy[1] = g9_tex_create(G9_TEX_CUBE, F_A8R8G8B8, 1, 1, 1, 1, G9_USE_SAMPLE);
    g_dummy[2] = g9_tex_create(G9_TEX_3D, F_A8R8G8B8, 1, 1, 1, 1, G9_USE_SAMPLE);
    g9_tex_upload(g_dummy[0], 0, 0, 0, 0, 0, 1, 1, 1, zero, 4, 4);
    for (uint32_t f = 0; f < 6; ++f)
        g9_tex_upload(g_dummy[1], f, 0, 0, 0, 0, 1, 1, 1, zero, 4, 4);
    g9_tex_upload(g_dummy[2], 0, 0, 0, 0, 0, 1, 1, 1, zero, 4, 4);
    g_util_vs = util_module(UTIL_VS, 0);
    g_util_fs = util_module(UTIL_FS, 1);
    if (!g_util_vs || !g_util_fs)
        return 0;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels((SDL_Window*)sdl_window, &w, &h);
    if (!swap_create((uint32_t)(w > 0 ? w : 1280), (uint32_t)(h > 0 ? h : 720)) || !g_present_pipe)
        return 0;

    /* background compiles */
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int nw = cpus > 4 ? 3 : cpus > 2 ? 2 : 1;
    for (int i = 0; i < nw; ++i)
    {
        pthread_t th;
        if (pthread_create(&th, NULL, worker, NULL) == 0)
        {
            pthread_detach(th);
            g_workers++;
        }
    }
    rt_log("[gfx9] Vulkan on %s (API %u.%u.%u, driver %08x): BC %s, clip distances %s, float controls %s, %d compile "
           "threads, cache %s\n",
        g_props.deviceName, VK_API_VERSION_MAJOR(g_props.apiVersion), VK_API_VERSION_MINOR(g_props.apiVersion),
        VK_API_VERSION_PATCH(g_props.apiVersion), g_props.driverVersion, g_bc ? "sampled" : "decoded on upload",
        g_feat.shaderClipDistance ? "on" : "off", g_float_controls ? "on" : "off", g_workers,
        g_cache_dir[0] ? g_cache_dir : "off");
    return 1;
}
