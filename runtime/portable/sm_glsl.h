/* Direct3D 9 shader bytecode (vs/ps 2.0, 2.x, 3.0) to GLSL 450 for Vulkan, to be compiled to
 * SPIR-V with glslang. The parsing and the D3D9 semantics are sm_msl.c's; only the output language
 * and the interface layout differ. The types (SmInfo, SmOpts, SmAux, SmDecl and the usages) are
 * sm_msl.h's and mean the same here, so a back end fills SmOpts and SmAux exactly as gfx9_metal.m
 * does. Every function here has its own name, so both translators can be linked into one program.
 *
 * One call translates one shader into one GLSL source with main(). The Vulkan interface:
 *
 *   descriptor set 0 (SM_GLSL_SET_UBO), uniform buffers in std140 layout, best as
 *   VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC so one set serves every draw:
 *     binding 0  SM_GLSL_UBO_VS, vertex stage. SmGlslVsConsts: 256 float4 constants (c#), then 16
 *                int4 (i#), then 16 uint (b#, nonzero is true; the shader reads them as uvec4[4], the
 *                same bytes). These are d->vsc, d->vsi and d->vsb of a G9Draw copied one after the
 *                other, 4416 bytes.
 *     binding 1  SM_GLSL_UBO_PS, fragment stage. SmGlslPsConsts: 224 float4, 16 int4, 16 uint
 *                (d->psc, d->psi, d->psb), 3904 bytes.
 *     binding 2  SM_GLSL_UBO_AUX, both stages. SmAux as it is (224 bytes): the half-pixel offset,
 *                the alpha reference, the sampler LOD biases, the user clip planes and the render
 *                scale for vPos. Metal's buffers SM_BUF_FLOAT, SM_BUF_INT, SM_BUF_BOOL and SM_BUF_AUX
 *                hold the same bytes.
 *   descriptor set 1 (SM_GLSL_SET_TEX), one VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER per sampler:
 *     binding s       pixel sampler s# (0-15): G9Draw.tex[s] and samp[s]
 *     binding 16 + s  vertex sampler s# (0-3), D3DVERTEXTEXTURESAMPLER0-3: G9Draw.tex[16 + s]
 *     Only the samplers a shader declares (SmInfo.sampler) are in its source, typed from dcl: 2D
 *     sampler2D, cube samplerCube, volume sampler3D. A 2D sampler in SmOpts.shadow_mask is a
 *     sampler2DShadow (the image view of a depth format, the VkSampler with compareEnable and
 *     VK_COMPARE_OP_LESS_OR_EQUAL, as gfx9_metal.m makes it); one in SmOpts.depth_mask is a plain
 *     sampler2D on the depth aspect whose red channel is replicated to all four components.
 *   vertex inputs   v# is layout(location = #): SmInfo.in[i].reg, with the usage and index it was
 *                   declared with, as Metal's [[attribute(#)]]. The type is vec4, or ivec4 / uvec4
 *                   for SmOpts.attr_int 1 / 2 (a VK_FORMAT_*_SINT / *_UINT format), or one uint
 *                   (VK_FORMAT_R32_UINT) for 3 (UDEC3) and 4 (DEC3N), unpacked in the shader.
 *   varyings        one vec4 per D3D semantic at a fixed location, sm_glsl_location(usage, index), the
 *                   same in every vertex and pixel shader, so any pair links. POSITION0 is gl_Position
 *                   and gl_FragCoord, PSIZE is gl_PointSize. SM2's oT#/oD#/oFog and t#/v# are
 *                   TEXCOORD#, COLOR# and FOG0. A pixel shader may read a semantic the vertex shader
 *                   does not write: translate the vertex shader again with it in SmOpts.vs_extra
 *                   (written 0), as on Metal. The table: TEXCOORD0-15 at 0-15, COLOR0-3 at 16-19,
 *                   FOG0 20, NORMAL0-1 21-22, TANGENT0 23, BINORMAL0 24, POSITION1-2 25-26,
 *                   BLENDWEIGHT0 27, BLENDINDICES0 28, DEPTH0 29; any other semantic fails the
 *                   translation. The game's shaders use TEXCOORD0-7, COLOR0-1, NORMAL0 and FOG0.
 *   pixel outputs   oC# is layout(location = #); oDepth is gl_FragDepth.
 *
 * Positions. D3D9 and Vulkan both take clip-space depth 0 to 1, so z passes through. D3D9 puts
 * pixel centres on integer coordinates and Vulkan, like Metal, on half-integers, so the vertex
 * shader moves x and y by SmAux.half_pixel * w as the Metal shaders do; that happens in D3D9's clip
 * space, so the host fills half_pixel exactly as gfx9_metal.m does (1/width, -1/height of the
 * viewport). Vulkan's clip space has y pointing down where D3D9's points up, so the vertex shader
 * then negates gl_Position.y. The image lands in the framebuffer the way D3D9 lays it out (row 0 at
 * the top, which is also v = 0 when a render target is sampled), the viewport is the D3D9 one with
 * a positive height, and no negative-height viewport is wanted. gl_FragCoord then matches D3D9's
 * window coordinates (vPos subtracts the half pixel back). Because the picture is D3D9's, so is the
 * winding seen in the framebuffer: D3D9's clockwise front faces are VK_FRONT_FACE_CLOCKWISE, and
 * D3DCULL_CW culls VK_CULL_MODE_FRONT_BIT, D3DCULL_CCW VK_CULL_MODE_BACK_BIT, as on Metal.
 *
 * sRGB and blending are not in the shaders: as on Metal, a sampler with D3DSAMP_SRGBTEXTURE reads
 * through an sRGB view of the image. D3DRS_SRGBWRITEENABLE is not applied: gfx9_metal.m keys its
 * pipelines by it but draws to the plain target, and gfx9_vulkan.c does the same. The alpha test is
 * (SmOpts.alpha_func, SmAux.alpha_ref).
 *
 * User clip planes write gl_ClipDistance[6], which wants the shaderClipDistance feature. Compiling
 * the vertex shaders with SM_NO_CLIP_DISTANCE defined (glslangValidator -DSM_NO_CLIP_DISTANCE)
 * leaves them out for a device without it.
 *
 * Every vertex shader writes gl_PointSize (1, or its PSIZE), which Vulkan needs for point lists.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sm_msl.h"

#define SM_GLSL_SET_UBO 0
#define SM_GLSL_UBO_VS 0
#define SM_GLSL_UBO_PS 1
#define SM_GLSL_UBO_AUX 2
#define SM_GLSL_SET_TEX 1
#define SM_GLSL_TEX_PS(s) (s)
#define SM_GLSL_TEX_VS(s) (16 + (s))
#define SM_GLSL_VARYINGS 32 /* locations sm_glsl_location returns: 0 to SM_GLSL_VARYINGS - 1 */

/* The two constant buffers as C structures (std140 makes these exact). */
typedef struct SmGlslVsConsts
{
    float c[256][4];
    int32_t i[16][4];
    uint32_t b[16];
} SmGlslVsConsts;

typedef struct SmGlslPsConsts
{
    float c[224][4];
    int32_t i[16][4];
    uint32_t b[16];
} SmGlslPsConsts;

/* The location of a semantic between the stages, or -1 for one no location is kept for (POSITION0
 * and PSIZE are built-ins). */
int sm_glsl_location(unsigned usage, unsigned index);
/* As sm_info: parses the declarations into info; 0 if the tokens are not a shader this takes. */
int sm_glsl_info(const uint32_t* tokens, size_t ntokens, SmInfo* info);
/* The whole translation into out (NUL-terminated). Returns the length, or 0 with a reason in err.
 * info->pixel says which glslang stage it is (frag or vert). Callers serialise translations. */
size_t sm_glsl_translate(const uint32_t* tokens, size_t ntokens, const SmOpts* opts, SmInfo* info, char* out,
    size_t cap, char* err, size_t errcap);
