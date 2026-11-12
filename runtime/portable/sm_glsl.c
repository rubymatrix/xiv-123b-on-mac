/* Direct3D 9 shader bytecode to GLSL 450 for Vulkan. See sm_glsl.h.
 *
 * This is sm_msl.c with a GLSL emitter: the parsing, the register model and every D3D9 rule are
 * the same, so a fix to one belongs in the other. The bytecode is parsed into instructions, then
 * written out as straight-line GLSL over register arrays (r[], v[], o[]) that the SPIR-V compiler
 * and the driver scalarise: every D3D instruction becomes one block computing a vec4 and storing
 * it through the destination's write mask. Flow control maps onto GLSL's (if/for/break);
 * subroutines (call/callnz/label) are inlined at each call.
 *
 * Where GLSL differs from MSL: vector comparisons are functions (lessThan, greaterThanEqual...),
 * select is mix with a bvec4, there are no (void) casts or brace initialisers, the constants and
 * the auxiliary values are uniform blocks rather than buffer arguments, textures are combined
 * image samplers, the stage interface is by location (sm_glsl_location) rather than by name, and
 * a constant index past the end of an array is a compile error rather than a read past a buffer.
 *
 * Token layout (the D3D9 DDK, "Shader Code Tokens"):
 *   instruction  opcode 0-15, controls 16-23, length 24-27 (SM2+), predicated 28
 *   destination  register 0-10 and type 28-30 | 11-12 (<< 3), relative 13, write mask 16-19,
 *                modifiers 20-23 (saturate 1, partial precision 2, centroid 4), bit 31 set
 *   source       register and type as above, relative 13 (an address token follows),
 *                swizzle 16-23 (2 bits per component), modifier 24-27, bit 31 set
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sm_glsl.h"

/* register types (D3DSHADER_PARAM_REGISTER_TYPE) */
enum
{
    R_TEMP = 0,
    R_INPUT = 1,
    R_CONST = 2,
    R_ADDR = 3, /* vertex: a0; pixel: t# (TEXTURE) */
    R_RASTOUT = 4,
    R_ATTROUT = 5,
    R_OUTPUT = 6, /* vertex 3.0: o#; earlier: oT# (TEXCRDOUT) */
    R_CONSTINT = 7,
    R_COLOROUT = 8,
    R_DEPTHOUT = 9,
    R_SAMPLER = 10,
    R_CONST2 = 11,
    R_CONST3 = 12,
    R_CONST4 = 13,
    R_CONSTBOOL = 14,
    R_LOOP = 15,
    R_TEMPFLOAT16 = 16,
    R_MISCTYPE = 17,
    R_LABEL = 18,
    R_PREDICATE = 19,
};

/* opcodes (D3DSHADER_INSTRUCTION_OPCODE_TYPE) */
enum
{
    OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX, OP_SLT,
    OP_SGE, OP_EXP, OP_LOG, OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4x4, OP_M4x3, OP_M3x4, OP_M3x3, OP_M3x2, OP_CALL,
    OP_CALLNZ, OP_LOOP, OP_RET, OP_ENDLOOP, OP_LABEL, OP_DCL, OP_POW, OP_CRS, OP_SGN, OP_ABS, OP_NRM, OP_SINCOS,
    OP_REP, OP_ENDREP, OP_IF, OP_IFC, OP_ELSE, OP_ENDIF, OP_BREAK, OP_BREAKC, OP_MOVA, OP_DEFB, OP_DEFI,
    OP_TEXCOORD = 64, OP_TEXKILL = 65, OP_TEX = 66, OP_EXPP = 78, OP_LOGP = 79, OP_CND = 80, OP_DEF = 81,
    OP_CMP = 88, OP_BEM = 89, OP_DP2ADD = 90, OP_DSX = 91, OP_DSY = 92, OP_TEXLDD = 93, OP_SETP = 94,
    OP_TEXLDL = 95, OP_BREAKP = 96, OP_PHASE = 0xFFFD, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF,
};

typedef struct Param
{
    uint32_t reg, type, mask, mod, swz;
    int rel;              /* relative addressing */
    uint32_t rel_type, rel_reg, rel_comp;
} Param;

typedef struct Ins
{
    uint32_t op, ctl, pred;
    Param dst, p, src[4];
    unsigned nsrc;
    int has_dst;
    const uint32_t* raw; /* the first token after the opcode */
    unsigned len;
} Ins;

#define MAX_INS 8192
#define MAX_DEF 256

typedef struct Ctx
{
    SmInfo* info;
    const SmOpts* opts;
    Ins* ins;
    unsigned nins;
    /* def / defi / defb */
    struct { uint32_t reg; float v[4]; } fdef[MAX_DEF];
    unsigned nfdef;
    struct { uint32_t reg; int32_t v[4]; } idef[16];
    unsigned nidef;
    struct { uint32_t reg, v; } bdef[16];
    unsigned nbdef;
    uint8_t out_used[16];       /* vs 3.0: o# declared */
    SmDecl out_decl[16];        /* vs 3.0: o# -> semantic */
    SmDecl in_decl[16];         /* v# -> semantic (both stages) */
    uint8_t in_used[16];
    uint8_t tex_used[8];        /* ps 2: t# read */
    uint8_t rel_const;          /* c[] read with relative addressing */
    uint8_t vpos, vface;
    /* output */
    char* out;
    size_t cap, len;
    int overflow;
    char* err;
    size_t errcap;
    int failed;
    int depth;  /* indentation */
    int loops;  /* nesting, for unique loop variables */
    int inline_depth;
} Ctx;

static void emit(Ctx* c, const char* fmt, ...)
{
    if (c->len >= c->cap)
    {
        c->overflow = 1;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(c->out + c->len, c->cap - c->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= c->cap - c->len)
    {
        c->overflow = 1;
        c->len = c->cap;
        return;
    }
    c->len += (size_t)n;
}

static void line(Ctx* c, const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    emit(c, "%*s%s\n", 4 * (c->depth + 1), "", buf);
}

static void fail(Ctx* c, const char* fmt, ...)
{
    if (c->failed)
        return;
    c->failed = 1;
    if (c->err && c->errcap)
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->err, c->errcap, fmt, ap);
        va_end(ap);
    }
}


/* --- parsing ------------------------------------------------------------------------------------ */

static uint32_t reg_type(uint32_t t) { return ((t >> 28) & 7u) | ((t >> 8) & 0x18u); }

static const uint32_t* parse_param(const uint32_t* p, const uint32_t* end, Param* out, int dst, int sm3)
{
    memset(out, 0, sizeof *out);
    if (p >= end)
        return NULL;
    uint32_t t = *p++;
    out->reg = t & 0x7FF;
    out->type = reg_type(t);
    out->rel = (t >> 13) & 1;
    if (dst)
    {
        out->mask = (t >> 16) & 0xF;
        out->mod = (t >> 20) & 0xF;
        out->swz = 0xE4;
    }
    else
    {
        out->swz = (t >> 16) & 0xFF;
        out->mod = (t >> 24) & 0xF;
        out->mask = 0xF;
    }
    if (out->rel)
    {
        if (sm3 || !dst)
        {
            if (p >= end)
                return NULL;
            uint32_t a = *p++;
            out->rel_type = reg_type(a);
            out->rel_reg = a & 0x7FF;
            out->rel_comp = (a >> 16) & 3; /* the replicated component */
        }
        else
            out->rel_type = R_ADDR;
    }
    return p;
}

static int op_has_dst(uint32_t op)
{
    switch (op)
    {
    case OP_NOP: case OP_CALL: case OP_CALLNZ: case OP_LOOP: case OP_RET: case OP_ENDLOOP: case OP_LABEL:
    case OP_REP: case OP_ENDREP: case OP_IF: case OP_IFC: case OP_ELSE: case OP_ENDIF: case OP_BREAK:
    case OP_BREAKC: case OP_BREAKP: case OP_PHASE:
        return 0;
    default:
        return 1;
    }
}

static int parse(Ctx* c, const uint32_t* tok, size_t n)
{
    SmInfo* info = c->info;
    uint32_t ver = tok[0];
    info->pixel = (ver >> 16) == 0xFFFF;
    info->major = (ver >> 8) & 0xFF;
    info->minor = ver & 0xFF;
    if (info->major < 2 || info->major > 3)
    {
        fail(c, "shader model %u.%u is not supported", info->major, info->minor);
        return 0;
    }
    int sm3 = info->major >= 3;
    const uint32_t* end = tok + n;
    const uint32_t* p = tok + 1;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ tok[i]) * 16777619u;
    info->hash = h;
    while (p < end)
    {
        uint32_t t = *p++;
        uint32_t op = t & 0xFFFF;
        if (t == 0x0000FFFFu)
            return 1;
        if (op == OP_COMMENT)
        {
            p += (t >> 16) & 0x7FFF;
            continue;
        }
        unsigned len = (t >> 24) & 0xF;
        const uint32_t* next = p + len;
        if (next > end)
        {
            fail(c, "truncated instruction");
            return 0;
        }
        if (c->nins >= MAX_INS)
        {
            fail(c, "too many instructions");
            return 0;
        }
        Ins* in = &c->ins[c->nins];
        memset(in, 0, sizeof *in);
        in->op = op;
        in->ctl = (t >> 16) & 0xFF;
        in->pred = (t >> 28) & 1;
        in->raw = p;
        in->len = len;
        if (op == OP_DCL)
        {
            uint32_t d = p[0];
            const uint32_t* q = parse_param(p + 1, next, &in->dst, 1, sm3);
            if (!q)
                return fail(c, "bad dcl"), 0;
            uint32_t usage = d & 0x1F, index = (d >> 16) & 0xF;
            uint32_t r = in->dst.reg;
            if (in->dst.type == R_SAMPLER && r < 16)
                info->sampler[r] = (uint8_t)((d >> 27) & 0xF);
            else if (in->dst.type == R_INPUT && r < 16)
            {
                c->in_decl[r] = (SmDecl){ (uint8_t)usage, (uint8_t)index, (uint8_t)r, 1 };
                if (info->pixel && !sm3) /* ps 2: v# are colors */
                    c->in_decl[r] = (SmDecl){ SM_COLOR, (uint8_t)r, (uint8_t)r, 1 };
                c->in_used[r] = 1;
            }
            else if (in->dst.type == R_ADDR && info->pixel && r < 8) /* ps 2: t# */
                c->tex_used[r] = 1;
            else if (in->dst.type == R_OUTPUT && sm3 && !info->pixel && r < 16)
            {
                c->out_decl[r] = (SmDecl){ (uint8_t)usage, (uint8_t)index, (uint8_t)r, 1 };
                c->out_used[r] = 1;
            }
            else if (in->dst.type == R_MISCTYPE)
            {
                if (r == 0)
                    c->vpos = 1;
                else
                    c->vface = 1;
            }
            p = next;
            continue; /* not kept as an instruction */
        }
        if (op == OP_DEF || op == OP_DEFI || op == OP_DEFB)
        {
            Param d;
            parse_param(p, next, &d, 1, sm3);
            if (op == OP_DEF && c->nfdef < MAX_DEF)
            {
                c->fdef[c->nfdef].reg = d.reg;
                memcpy(c->fdef[c->nfdef].v, p + 1, 16);
                c->nfdef++;
            }
            else if (op == OP_DEFI && c->nidef < 16)
            {
                c->idef[c->nidef].reg = d.reg;
                memcpy(c->idef[c->nidef].v, p + 1, 16);
                c->nidef++;
            }
            else if (op == OP_DEFB && c->nbdef < 16)
            {
                c->bdef[c->nbdef].reg = d.reg;
                c->bdef[c->nbdef].v = p[1];
                c->nbdef++;
            }
            p = next;
            continue;
        }
        const uint32_t* q = p;
        in->has_dst = op_has_dst(op);
        if (in->has_dst)
        {
            q = parse_param(q, next, &in->dst, 1, sm3);
            if (!q)
                return fail(c, "bad destination (op %u)", op), 0;
        }
        if (in->pred)
        {
            q = parse_param(q, next, &in->p, 0, sm3);
            if (!q)
                return fail(c, "bad predicate (op %u)", op), 0;
        }
        while (q && q < next && in->nsrc < 4)
            q = parse_param(q, next, &in->src[in->nsrc++], 0, sm3);
        if (!q)
            return fail(c, "bad source (op %u)", op), 0;
        /* registers read, for the interface */
        for (unsigned s = 0; s < in->nsrc; ++s)
        {
            Param* sp = &in->src[s];
            if (sp->type == R_CONST && sp->rel)
                c->rel_const = 1;
            if (info->pixel && !sm3 && sp->type == R_ADDR && sp->reg < 8)
                c->tex_used[sp->reg] = 1;
            if (info->pixel && !sm3 && sp->type == R_INPUT && sp->reg < 2 && !c->in_used[sp->reg])
            {
                c->in_decl[sp->reg] = (SmDecl){ SM_COLOR, (uint8_t)sp->reg, (uint8_t)sp->reg, 1 };
                c->in_used[sp->reg] = 1;
            }
            if (sp->type == R_MISCTYPE)
                (sp->reg ? (c->vface = 1) : (c->vpos = 1));
        }
        if (in->has_dst && info->pixel && in->dst.type == R_COLOROUT)
            info->color_outputs |= (uint8_t)(1u << (in->dst.reg & 3));
        if (in->has_dst && info->pixel && in->dst.type == R_DEPTHOUT)
            info->writes_depth = 1;
        c->nins++;
        p = next;
    }
    fail(c, "no END token");
    return 0;
}


/* --- the stage interface ------------------------------------------------------------------------ */

static const char* semantic(unsigned usage, unsigned index, char buf[24])
{
    static const char* const names[] = { "position", "blendweight", "blendindices", "normal", "psize", "texcoord",
                                         "tangent", "binormal", "tessfactor", "positiont", "color", "fog", "depth",
                                         "sample" };
    snprintf(buf, 24, "%s%u", usage < 14 ? names[usage] : "unknown", index);
    return buf;
}

int sm_glsl_location(unsigned usage, unsigned index)
{
    switch (usage)
    {
    case SM_TEXCOORD: return index < 16 ? (int)index : -1;
    case SM_COLOR: return index < 4 ? 16 + (int)index : -1;
    case SM_FOG: return index == 0 ? 20 : -1;
    case SM_NORMAL: return index < 2 ? 21 + (int)index : -1;
    case SM_TANGENT: return index == 0 ? 23 : -1;
    case SM_BINORMAL: return index == 0 ? 24 : -1;
    case SM_POSITION: return index >= 1 && index <= 2 ? 24 + (int)index : -1;
    case SM_BLENDWEIGHT: return index == 0 ? 27 : -1;
    case SM_BLENDINDICES: return index == 0 ? 28 : -1;
    case SM_DEPTH: return index == 0 ? 29 : -1;
    case SM_PSIZE: return -1;
    default: return -1;
    }
}

/* --- expressions -------------------------------------------------------------------------------- */

static const char* comp(unsigned i)
{
    static const char* const c[4] = { "x", "y", "z", "w" };
    return c[i & 3];
}

static void swizzle_str(uint32_t swz, char out[6])
{
    out[0] = '.';
    for (int i = 0; i < 4; ++i)
        out[1 + i] = "xyzw"[(swz >> (2 * i)) & 3];
    out[5] = 0;
}

static int find_fdef(const Ctx* c, uint32_t reg)
{
    for (int i = (int)c->nfdef - 1; i >= 0; --i)
        if (c->fdef[i].reg == reg)
            return i;
    return -1;
}

/* A float as a GLSL float expression: %.9g round-trips a float, inf and NaN as their bits. */
static void fnum(float f, char out[48])
{
    if (isnan(f))
        snprintf(out, 48, "uintBitsToFloat(0x7fc00000u)");
    else if (isinf(f))
        snprintf(out, 48, "uintBitsToFloat(%s)", f > 0 ? "0x7f800000u" : "0xff800000u");
    else
    {
        snprintf(out, 48, "%.9g", f);
        if (!strpbrk(out, ".e"))
            strcat(out, ".0");
    }
}

static void vec4_lit(const float v[4], char* out, size_t cap)
{
    char a[48], b[48], d[48], e[48];
    fnum(v[0], a), fnum(v[1], b), fnum(v[2], d), fnum(v[3], e);
    snprintf(out, cap, "vec4(%s, %s, %s, %s)", a, b, d, e);
}

static unsigned nconst(const Ctx* c) { return c->info->pixel ? 224 : 256; }

/* The base register expression (no swizzle, no modifier): a vec4, or a bool for b#. */
static void reg_expr(Ctx* c, const Param* p, char* out, size_t cap)
{
    char rel[64] = "";
    if (p->rel)
    {
        if (p->rel_type == R_LOOP)
            snprintf(rel, sizeof rel, "aL + ");
        else
            snprintf(rel, sizeof rel, "a0.%s + ", comp(p->rel_comp));
    }
    switch (p->type)
    {
    case R_TEMP:
    case R_TEMPFLOAT16:
        if (p->reg >= 32)
            fail(c, "temporary r%u", p->reg);
        snprintf(out, cap, "r[%u]", p->reg & 31);
        break;
    case R_INPUT:
        if (p->rel)
            snprintf(out, cap, "v[clamp(%s%u, 0, 15)]", rel, p->reg);
        else
            snprintf(out, cap, "v[%u]", p->reg & 15);
        break;
    case R_CONST:
    case R_CONST2:
    case R_CONST3:
    case R_CONST4:
    {
        uint32_t reg = p->reg + (p->type == R_CONST2 ? 2048 : p->type == R_CONST3 ? 4096 : p->type == R_CONST4 ? 6144 : 0);
        if (p->rel)
            snprintf(out, cap, "C(%s%u)", rel, reg);
        else
        {
            int d = find_fdef(c, reg);
            if (d >= 0)
                vec4_lit(c->fdef[d].v, out, cap);
            else if (reg < nconst(c))
                snprintf(out, cap, "c[%u]", reg);
            else
                snprintf(out, cap, "vec4(0.0)"); /* past the constants: GLSL refuses the index */
        }
        break;
    }
    case R_ADDR:
        if (c->info->pixel)
            snprintf(out, cap, "tc[%u]", p->reg & 7); /* ps 2: texture coordinates */
        else
            snprintf(out, cap, "vec4(a0)");
        break;
    case R_CONSTINT:
        snprintf(out, cap, "vec4(I(%u))", p->reg);
        break;
    case R_CONSTBOOL:
        snprintf(out, cap, "B(%u)", p->reg);
        break;
    case R_LOOP:
        snprintf(out, cap, "vec4(aL)");
        break;
    case R_SAMPLER:
        snprintf(out, cap, "s%u", p->reg);
        break;
    case R_MISCTYPE:
        snprintf(out, cap, p->reg ? "vface" : "vpos");
        break;
    case R_PREDICATE:
        snprintf(out, cap, "vec4(p0)");
        break;
    case R_OUTPUT:
        snprintf(out, cap, "o[%u]", p->reg & 15);
        break;
    case R_COLOROUT:
        snprintf(out, cap, "oC[%u]", p->reg & 3);
        break;
    default:
        fail(c, "source register type %u", p->type);
        snprintf(out, cap, "vec4(0.0)");
    }
}

/* A source operand as a vec4 expression: register, swizzle, modifier. The not modifier (13, on a
 * predicate or bool) is left to the instruction that reads it. */
static void src_expr(Ctx* c, const Param* p, char* out, size_t cap)
{
    char base[160], sw[6];
    reg_expr(c, p, base, sizeof base);
    if (p->type == R_CONSTBOOL || p->type == R_SAMPLER)
    {
        snprintf(out, cap, "%s", base);
        return;
    }
    swizzle_str(p->swz, sw);
    char v[200];
    snprintf(v, sizeof v, "(%s)%s", base, p->swz == 0xE4 ? "" : sw);
    switch (p->mod)
    {
    case 0: case 13: snprintf(out, cap, "%s", v); break;
    case 1: snprintf(out, cap, "(-%s)", v); break;
    case 2: snprintf(out, cap, "(%s - 0.5)", v); break;
    case 3: snprintf(out, cap, "(-(%s - 0.5))", v); break;
    case 4: snprintf(out, cap, "(2.0 * %s - 1.0)", v); break;
    case 5: snprintf(out, cap, "(-(2.0 * %s - 1.0))", v); break;
    case 6: snprintf(out, cap, "(1.0 - %s)", v); break;
    case 7: snprintf(out, cap, "(2.0 * %s)", v); break;
    case 8: snprintf(out, cap, "(-2.0 * %s)", v); break;
    case 11: snprintf(out, cap, "abs(%s)", v); break;
    case 12: snprintf(out, cap, "(-abs(%s))", v); break;
    default:
        fail(c, "source modifier %u", p->mod);
        snprintf(out, cap, "%s", v);
    }
}

/* The destination register lvalue. */
static int dst_lvalue(Ctx* c, const Param* d, char* out, size_t cap)
{
    SmInfo* info = c->info;
    switch (d->type)
    {
    case R_TEMP:
    case R_TEMPFLOAT16:
        if (d->reg >= 32)
            fail(c, "temporary r%u", d->reg);
        snprintf(out, cap, "r[%u]", d->reg & 31);
        return 1;
    case R_ADDR:
        if (info->pixel)
            snprintf(out, cap, "tc[%u]", d->reg & 7);
        else
            snprintf(out, cap, "a0f"); /* vs: mov a0 (rounded into a0 after the store) */
        return 1;
    case R_RASTOUT:
        snprintf(out, cap, d->reg == 0 ? "oPos" : d->reg == 1 ? "oFog" : "oPts");
        return 1;
    case R_ATTROUT:
        snprintf(out, cap, "oD[%u]", d->reg & 1);
        return 1;
    case R_OUTPUT:
        if (info->major >= 3)
        {
            if (d->rel)
                snprintf(out, cap, "o[clamp(aL + %u, 0, 15)]", d->reg);
            else
                snprintf(out, cap, "o[%u]", d->reg & 15);
        }
        else
            snprintf(out, cap, "oT[%u]", d->reg & 7);
        return 1;
    case R_COLOROUT:
        snprintf(out, cap, "oC[%u]", d->reg & 3);
        return 1;
    case R_DEPTHOUT:
        snprintf(out, cap, "oDepth");
        return 1;
    case R_PREDICATE:
        snprintf(out, cap, "p0");
        return 1;
    default:
        fail(c, "destination register type %u", d->type);
        snprintf(out, cap, "r[0]");
        return 0;
    }
}

/* Stores vec4 expression `val` (a bvec4 for p0) through d's write mask (and saturate, and the
 * predicate). */
static void store(Ctx* c, const Ins* in, const char* val)
{
    char lv[96];
    const Param* d = &in->dst;
    dst_lvalue(c, d, lv, sizeof lv);
    int scalar_out = d->type == R_DEPTHOUT || (d->type == R_RASTOUT && d->reg != 0);
    if (d->type == R_PREDICATE)
    {
        line(c, "{ bvec4 t = %s;", val);
        for (int k = 0; k < 4; ++k)
            if (d->mask & (1u << k))
                line(c, "  p0.%s = t.%s;", comp((unsigned)k), comp((unsigned)k));
        line(c, "}");
        return;
    }
    if (d->mod & 1)
        line(c, "{ vec4 t = clamp(%s, 0.0, 1.0);", val);
    else
        line(c, "{ vec4 t = %s;", val);
    if (scalar_out)
        line(c, "  %s = t.x;", lv);
    else
        for (int k = 0; k < 4; ++k)
        {
            if (!(d->mask & (1u << k)))
                continue;
            if (in->pred)
            {
                unsigned pc = (in->p.swz >> (2 * k)) & 3;
                line(c, "  if (%sp0.%s) %s.%s = t.%s;", in->p.mod == 13 ? "!" : "", comp(pc), lv, comp((unsigned)k),
                    comp((unsigned)k));
            }
            else
                line(c, "  %s.%s = t.%s;", lv, comp((unsigned)k), comp((unsigned)k));
        }
    if (d->type == R_ADDR && !c->info->pixel)
        line(c, "  a0 = ivec4(floor(a0f + 0.5));");
    line(c, "}");
}

/* scalar comparisons (ifc, breakc) */
static const char* cmp_op(uint32_t ctl)
{
    switch (ctl & 7)
    {
    case 1: return ">";
    case 2: return "==";
    case 3: return ">=";
    case 4: return "<";
    case 5: return "!=";
    case 6: return "<=";
    default: return "!=";
    }
}

/* the same as component-wise functions (setp) */
static const char* cmp_fn(uint32_t ctl)
{
    switch (ctl & 7)
    {
    case 1: return "greaterThan";
    case 2: return "equal";
    case 3: return "greaterThanEqual";
    case 4: return "lessThan";
    case 5: return "notEqual";
    case 6: return "lessThanEqual";
    default: return "notEqual";
    }
}

/* A predicate or bool condition: `b` is src_expr of p, negated by the not modifier. */
static void cond_expr(const Param* p, const char* b, char* out, size_t cap)
{
    int neg = p->mod == 13;
    if (p->type == R_PREDICATE)
        snprintf(out, cap, "(%s).x %s 0.0", b, neg ? "==" : "!=");
    else if (p->type == R_CONSTBOOL)
        snprintf(out, cap, "%s%s", neg ? "!" : "", b);
    else
        snprintf(out, cap, "(%s).x %s 0.0", b, neg ? "==" : "!=");
}

/* --- instructions ------------------------------------------------------------------------------- */

static void translate_range(Ctx* c, unsigned from, unsigned to);

static int label_start(Ctx* c, uint32_t label)
{
    for (unsigned i = 0; i < c->nins; ++i)
        if (c->ins[i].op == OP_LABEL && c->ins[i].nsrc && c->ins[i].src[0].reg == label)
            return (int)i + 1;
    return -1;
}

static unsigned label_end(Ctx* c, unsigned start)
{
    for (unsigned i = start; i < c->nins; ++i)
        if (c->ins[i].op == OP_RET || c->ins[i].op == OP_LABEL)
            return i;
    return c->nins;
}

static void inline_call(Ctx* c, uint32_t label)
{
    int s = label_start(c, label);
    if (s < 0)
    {
        fail(c, "call to a missing label %u", label);
        return;
    }
    if (c->inline_depth >= 8)
    {
        fail(c, "subroutines nest too deep");
        return;
    }
    c->inline_depth++;
    line(c, "{ /* l%u */", label);
    c->depth++;
    translate_range(c, (unsigned)s, label_end(c, (unsigned)s));
    c->depth--;
    line(c, "}");
    c->inline_depth--;
}

enum { TS_PLAIN, TS_BIAS, TS_LOD, TS_GRAD };

/* texld and its forms. arg: the bias (TS_BIAS) or level (TS_LOD) expression; ddx, ddy: the
 * gradients (TS_GRAD). */
static void texture_sample(Ctx* c, const Ins* in, const char* coord, unsigned s, int mode, const char* arg,
    const char* ddx, const char* ddy)
{
    unsigned type = c->info->sampler[s & 15];
    int pixel = c->info->pixel;
    int shadow = pixel && c->opts && (c->opts->shadow_mask >> s) & 1;
    int depth = pixel && c->opts && (c->opts->depth_mask >> s) & 1;
    int vol = type == 3 || type == 4;
    const char* cc = vol ? ".xyz" : ".xy";
    char bias[64];
    snprintf(bias, sizeof bias, "aux.lod_bias[%u].%s", (s & 15) >> 2, comp(s));
    char t[1200], v[1300];
    if (shadow && !vol)
    {
        /* a depth texture: the comparison D3D9 drivers make for shadow maps (ref = z) */
        snprintf(v, sizeof v, "vec4(texture(s%u, vec3((%s).xy, clamp((%s).z, 0.0, 1.0))))", s, coord, coord);
        store(c, in, v);
        return;
    }
    if (mode == TS_GRAD)
        snprintf(t, sizeof t, "textureGrad(s%u, (%s)%s, (%s)%s, (%s)%s)", s, coord, cc, ddx, cc, ddy, cc);
    else if (mode == TS_LOD)
        snprintf(t, sizeof t, "textureLod(s%u, (%s)%s, %s)", s, coord, cc, arg);
    else if (mode == TS_BIAS)
        snprintf(t, sizeof t, "texture(s%u, (%s)%s, %s)", s, coord, cc, arg);
    else if (!pixel)
        snprintf(t, sizeof t, "textureLod(s%u, (%s)%s, 0.0)", s, coord, cc); /* no derivatives: level 0 */
    else if (depth)
        snprintf(t, sizeof t, "texture(s%u, (%s)%s)", s, coord, cc);
    else
        snprintf(t, sizeof t, "texture(s%u, (%s)%s, %s)", s, coord, cc, bias);
    if (depth && !vol)
        snprintf(v, sizeof v, "vec4(%s.x)", t); /* a depth texture read for its values (INTZ): the depth in every component */
    else
        snprintf(v, sizeof v, "%s", t);
    store(c, in, v);
}

static void one(Ctx* c, unsigned idx)
{
    const Ins* in = &c->ins[idx];
    char s0[512] = "", s1[512] = "", s2[512] = "", v[2048];
    if (in->nsrc > 0 && in->op != OP_LOOP && in->op != OP_REP && in->op != OP_CALL && in->op != OP_CALLNZ &&
        in->op != OP_LABEL && in->op != OP_IF && in->op != OP_TEX && in->op != OP_TEXLDL && in->op != OP_TEXLDD)
        src_expr(c, &in->src[0], s0, sizeof s0);
    if (in->nsrc > 1 && in->op != OP_TEX && in->op != OP_TEXLDL && in->op != OP_TEXLDD && in->op != OP_CALLNZ &&
        in->op != OP_LOOP)
        src_expr(c, &in->src[1], s1, sizeof s1);
    if (in->nsrc > 2 && in->op != OP_TEXLDD)
        src_expr(c, &in->src[2], s2, sizeof s2);

    switch (in->op)
    {
    case OP_NOP:
    case OP_PHASE:
        return;
    case OP_MOV:
        store(c, in, s0);
        return;
    case OP_MOVA:
        line(c, "{ vec4 t = %s;", s0);
        for (int k = 0; k < 4; ++k)
            if (in->dst.mask & (1u << k))
                line(c, "  a0.%s = int(floor(t.%s + 0.5));", comp((unsigned)k), comp((unsigned)k));
        line(c, "}");
        return;
    case OP_ADD: snprintf(v, sizeof v, "%s + %s", s0, s1); break;
    case OP_SUB: snprintf(v, sizeof v, "%s - %s", s0, s1); break;
    case OP_MUL: snprintf(v, sizeof v, "%s * %s", s0, s1); break;
    case OP_MAD: snprintf(v, sizeof v, "%s * %s + %s", s0, s1, s2); break;
    case OP_MIN: snprintf(v, sizeof v, "min(%s, %s)", s0, s1); break;
    case OP_MAX: snprintf(v, sizeof v, "max(%s, %s)", s0, s1); break;
    case OP_SLT: snprintf(v, sizeof v, "vec4(lessThan(%s, %s))", s0, s1); break;
    case OP_SGE: snprintf(v, sizeof v, "vec4(greaterThanEqual(%s, %s))", s0, s1); break;
    case OP_DP3: snprintf(v, sizeof v, "vec4(dot((%s).xyz, (%s).xyz))", s0, s1); break;
    case OP_DP4: snprintf(v, sizeof v, "vec4(dot(%s, %s))", s0, s1); break;
    case OP_RCP: snprintf(v, sizeof v, "vec4(1.0 / (%s).x)", s0); break;
    case OP_RSQ: snprintf(v, sizeof v, "vec4(inversesqrt(abs((%s).x)))", s0); break;
    case OP_EXP:
    case OP_EXPP: snprintf(v, sizeof v, "vec4(exp2((%s).x))", s0); break;
    case OP_LOG:
    case OP_LOGP: snprintf(v, sizeof v, "vec4(log2(abs((%s).x)))", s0); break;
    case OP_POW: snprintf(v, sizeof v, "vec4(Pow((%s).x, (%s).x))", s0, s1); break;
    case OP_FRC: snprintf(v, sizeof v, "fract(%s)", s0); break;
    case OP_ABS: snprintf(v, sizeof v, "abs(%s)", s0); break;
    case OP_SGN: snprintf(v, sizeof v, "sign(%s)", s0); break;
    case OP_NRM: snprintf(v, sizeof v, "Nrm(%s)", s0); break;
    case OP_CRS: snprintf(v, sizeof v, "vec4(cross((%s).xyz, (%s).xyz), 0.0)", s0, s1); break;
    case OP_LRP: snprintf(v, sizeof v, "mix(%s, %s, %s)", s2, s1, s0); break;
    case OP_CMP: snprintf(v, sizeof v, "mix(%s, %s, greaterThanEqual(%s, vec4(0.0)))", s2, s1, s0); break;
    case OP_CND: snprintf(v, sizeof v, "mix(%s, %s, greaterThan(%s, vec4(0.5)))", s2, s1, s0); break;
    case OP_DP2ADD: snprintf(v, sizeof v, "vec4(dot((%s).xy, (%s).xy) + (%s).x)", s0, s1, s2); break;
    case OP_DSX:
    case OP_DSY:
        if (!c->info->pixel)
        {
            fail(c, "derivatives in a vertex shader");
            return;
        }
        snprintf(v, sizeof v, "%s(%s)", in->op == OP_DSX ? "dFdx" : "dFdy", s0);
        break;
    case OP_SINCOS: snprintf(v, sizeof v, "vec4(cos((%s).x), sin((%s).x), 0.0, 0.0)", s0, s0); break;
    case OP_LIT:
        snprintf(v, sizeof v, "Lit(%s)", s0);
        break;
    case OP_DST:
        snprintf(v, sizeof v, "vec4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", s0, s1, s0, s1);
        break;
    case OP_M4x4:
    case OP_M4x3:
    case OP_M3x4:
    case OP_M3x3:
    case OP_M3x2:
    {
        unsigned rows = in->op == OP_M4x4 || in->op == OP_M3x4 ? 4 : in->op == OP_M3x2 ? 2 : 3;
        int three = in->op == OP_M3x4 || in->op == OP_M3x3 || in->op == OP_M3x2;
        char rowe[4][256];
        for (unsigned r = 0; r < rows; ++r)
        {
            Param p = in->src[1];
            p.reg += r;
            src_expr(c, &p, rowe[r], sizeof rowe[r]);
        }
        int n = snprintf(v, sizeof v, "vec4(");
        for (unsigned r = 0; r < 4; ++r)
        {
            if (r < rows)
                n += snprintf(v + n, sizeof v - (size_t)n, three ? "dot((%s).xyz, (%s).xyz)" : "dot(%s, %s)", s0, rowe[r]);
            else
                n += snprintf(v + n, sizeof v - (size_t)n, "0.0");
            n += snprintf(v + n, sizeof v - (size_t)n, r < 3 ? ", " : ")");
        }
        break;
    }
    case OP_SETP:
        snprintf(v, sizeof v, "%s(%s, %s)", cmp_fn(in->ctl), s0, s1);
        store(c, in, v);
        return;
    case OP_TEX:
    {
        char coord[512];
        src_expr(c, &in->src[0], coord, sizeof coord);
        unsigned s = in->src[1].reg;
        if (in->ctl == 1) /* texldp */
        {
            char pc[700];
            snprintf(pc, sizeof pc, "(%s) / (%s).w", coord, coord);
            texture_sample(c, in, pc, s, TS_PLAIN, NULL, NULL, NULL);
        }
        else if (in->ctl == 2) /* texldb */
        {
            char b[600];
            snprintf(b, sizeof b, "(%s).w + aux.lod_bias[%u].%s", coord, (s & 15) >> 2, comp(s));
            texture_sample(c, in, coord, s, TS_BIAS, b, NULL, NULL);
        }
        else
            texture_sample(c, in, coord, s, TS_PLAIN, NULL, NULL, NULL);
        return;
    }
    case OP_TEXLDL:
    {
        char coord[512], l[600];
        src_expr(c, &in->src[0], coord, sizeof coord);
        snprintf(l, sizeof l, "(%s).w", coord);
        texture_sample(c, in, coord, in->src[1].reg, TS_LOD, l, NULL, NULL);
        return;
    }
    case OP_TEXLDD:
    {
        char coord[512], dx[512], dy[512];
        src_expr(c, &in->src[0], coord, sizeof coord);
        src_expr(c, &in->src[2], dx, sizeof dx);
        src_expr(c, &in->src[3], dy, sizeof dy);
        texture_sample(c, in, coord, in->src[1].reg, TS_GRAD, NULL, dx, dy);
        return;
    }
    case OP_TEXKILL:
    {
        char r[96];
        dst_lvalue(c, &in->dst, r, sizeof r);
        char cond[256] = "";
        int n = 0;
        for (int k = 0; k < 4; ++k)
            if (in->dst.mask & (1u << k))
                n += snprintf(cond + n, sizeof cond - (size_t)n, "%s%s.%s < 0.0", n ? " || " : "", r, comp((unsigned)k));
        line(c, "if (%s) discard;", n ? cond : "false");
        return;
    }
    case OP_IF:
    {
        char b[256], e[300];
        src_expr(c, &in->src[0], b, sizeof b);
        cond_expr(&in->src[0], b, e, sizeof e);
        line(c, "if (%s) {", e);
        c->depth++;
        return;
    }
    case OP_IFC:
        line(c, "if ((%s).x %s (%s).x) {", s0, cmp_op(in->ctl), s1);
        c->depth++;
        return;
    case OP_ELSE:
        c->depth--;
        line(c, "} else {");
        c->depth++;
        return;
    case OP_ENDIF:
        c->depth--;
        line(c, "}");
        return;
    case OP_ENDREP:
        c->depth--;
        line(c, "}");
        c->loops--;
        return;
    case OP_ENDLOOP:
        c->depth--;
        line(c, "}");
        line(c, "aL = aL_save%d; }", c->loops);
        c->loops--;
        return;
    case OP_LOOP:
    {
        c->loops++;
        unsigned i = in->src[1].reg;
        line(c, "{ int aL_save%d = aL; ivec4 L%d = I(%u); aL = L%d.y;", c->loops, c->loops, i, c->loops);
        line(c, "for (int k%d = 0; k%d < L%d.x; ++k%d, aL += L%d.z) {", c->loops, c->loops, c->loops, c->loops, c->loops);
        c->depth++;
        return;
    }
    case OP_REP:
    {
        c->loops++;
        unsigned i = in->src[0].reg;
        line(c, "for (int k%d = 0; k%d < I(%u).x; ++k%d) {", c->loops, c->loops, i, c->loops);
        c->depth++;
        return;
    }
    case OP_BREAK:
        line(c, "break;");
        return;
    case OP_BREAKC:
        line(c, "if ((%s).x %s (%s).x) break;", s0, cmp_op(in->ctl), s1);
        return;
    case OP_BREAKP:
    {
        char e[600];
        cond_expr(&in->src[0], s0, e, sizeof e);
        line(c, "if (%s) break;", e);
        return;
    }
    case OP_CALL:
        inline_call(c, in->src[0].reg);
        return;
    case OP_CALLNZ:
    {
        char b[256], e[300];
        src_expr(c, &in->src[1], b, sizeof b);
        cond_expr(&in->src[1], b, e, sizeof e);
        line(c, "if (%s) {", e);
        c->depth++;
        inline_call(c, in->src[0].reg);
        c->depth--;
        line(c, "}");
        return;
    }
    case OP_RET:
    case OP_LABEL:
        return; /* handled by the range walk */
    default:
        fail(c, "opcode %u is not supported", in->op);
        return;
    }
    store(c, in, v);
}

static void translate_range(Ctx* c, unsigned from, unsigned to)
{
    for (unsigned i = from; i < to && !c->failed; ++i)
        one(c, i);
}

/* --- the program -------------------------------------------------------------------------------- */

static void prelude(Ctx* c)
{
    int pixel = c->info->pixel;
    emit(c, "#version 450\n\n");
    emit(c, "layout(std140, set = %d, binding = %d) uniform %s { vec4 c[%u]; ivec4 ci[16]; uvec4 cb[4]; };\n",
        SM_GLSL_SET_UBO, pixel ? SM_GLSL_UBO_PS : SM_GLSL_UBO_VS, pixel ? "SmPs" : "SmVs", nconst(c));
    emit(c, "layout(std140, set = %d, binding = %d) uniform SmAux { vec4 half_pixel; vec4 alpha_ref; vec4 lod_bias[4]; "
            "vec4 clip[6]; uvec4 clip_on; vec4 rscale; } aux;\n",
        SM_GLSL_SET_UBO, SM_GLSL_UBO_AUX);
    for (unsigned s = 0; s < 16; ++s)
    {
        unsigned type = c->info->sampler[s];
        if (!type)
            continue;
        if (!pixel && s >= 4)
        {
            fail(c, "vertex sampler s%u", s);
            return;
        }
        int shadow = pixel && c->opts && (c->opts->shadow_mask >> s) & 1;
        const char* tt = type == 3 ? "samplerCube" : type == 4 ? "sampler3D" : shadow ? "sampler2DShadow" : "sampler2D";
        emit(c, "layout(set = %d, binding = %d) uniform %s s%u;\n", SM_GLSL_SET_TEX,
            pixel ? SM_GLSL_TEX_PS(s) : SM_GLSL_TEX_VS(s), tt, s);
    }
    emit(c, "\n");
    /* D3D9 hardware gives pow(x, 0) = 1, 0^0 included (pow(0, 0) is undefined in GLSL) */
    emit(c, "float Pow(float x, float y) { return y == 0.0 ? 1.0 : pow(abs(x), y); }\n");
    emit(c, "vec4 Nrm(vec4 v) { float d = dot(v.xyz, v.xyz); return d > 0.0 ? v * inversesqrt(d) : v; }\n");
    emit(c, "vec4 Lit(vec4 s) { vec4 r = vec4(1.0, 0.0, 0.0, 1.0); if (s.x > 0.0) { r.y = s.x; "
            "if (s.y > 0.0) r.z = pow(s.y, clamp(s.w, -127.9961, 127.9961)); } return r; }\n");
    /* defi/defb override the buffers */
    emit(c, "ivec4 I(int n) {\n    switch (n) {\n");
    for (unsigned i = 0; i < c->nidef; ++i)
        emit(c, "    case %u: return ivec4(%d, %d, %d, %d);\n", c->idef[i].reg, c->idef[i].v[0], c->idef[i].v[1],
            c->idef[i].v[2], c->idef[i].v[3]);
    emit(c, "    default: return ci[n & 15];\n    }\n}\n");
    emit(c, "bool B(int n) {\n    switch (n) {\n");
    for (unsigned i = 0; i < c->nbdef; ++i)
        emit(c, "    case %u: return %s;\n", c->bdef[i].reg, c->bdef[i].v ? "true" : "false");
    emit(c, "    default: return cb[(n >> 2) & 3][n & 3] != 0u;\n    }\n}\n");
    /* relative constant reads: def'd registers win, as on the hardware */
    emit(c, "vec4 C(int n) {\n    switch (n) {\n");
    if (c->rel_const)
        for (unsigned i = 0; i < c->nfdef; ++i)
        {
            char v[256];
            vec4_lit(c->fdef[i].v, v, sizeof v);
            emit(c, "    case %u: return %s;\n", c->fdef[i].reg, v);
        }
    emit(c, "    default: return (n >= 0 && n < %u) ? c[n] : vec4(0.0);\n    }\n}\n\n", nconst(c));
}

static void add_out(SmInfo* info, uint8_t usage, uint8_t index, uint8_t reg)
{
    for (unsigned i = 0; i < info->nout; ++i)
        if (info->out[i].usage == usage && info->out[i].index == index)
            return;
    if (info->nout < 16)
        info->out[info->nout++] = (SmDecl){ usage, index, reg, 1 };
}

/* A varying's location, failing the translation for a semantic without one. */
static int varying(Ctx* c, unsigned usage, unsigned index)
{
    int l = sm_glsl_location(usage, index);
    if (l < 0)
    {
        char name[24];
        fail(c, "no varying location for %s", semantic(usage, index, name));
    }
    return l;
}

/* the vs_extra semantics not already written */
static int extra_needed(const Ctx* c, const SmDecl* d)
{
    for (unsigned k = 0; k < c->info->nout; ++k)
        if (c->info->out[k].usage == d->usage && c->info->out[k].index == d->index)
            return 0;
    return !(d->usage == SM_POSITION && d->index == 0) && d->usage != SM_PSIZE;
}

static void vertex_program(Ctx* c)
{
    SmInfo* info = c->info;
    int sm3 = info->major >= 3;
    char name[24];
    /* inputs */
    for (unsigned r = 0; r < 16; ++r)
        if (c->in_used[r])
        {
            unsigned k = c->opts ? c->opts->attr_int[r] : 0;
            emit(c, "layout(location = %u) in %s vin%u;\n", r, k == 1 ? "ivec4" : k == 2 ? "uvec4" : k >= 3 ? "uint" : "vec4",
                r);
        }
    for (unsigned r = 0; r < 16; ++r)
        if (c->in_used[r])
            info->in[info->nin++] = c->in_decl[r];
    /* outputs, by semantic */
    if (sm3)
    {
        for (unsigned r = 0; r < 16; ++r)
            if (c->out_used[r])
                add_out(info, c->out_decl[r].usage, c->out_decl[r].index, (uint8_t)r);
    }
    else
    {
        add_out(info, SM_POSITION, 0, 0);
        for (unsigned i = 0; i < 2; ++i)
            add_out(info, SM_COLOR, (uint8_t)i, (uint8_t)i);
        for (unsigned i = 0; i < 8; ++i)
            add_out(info, SM_TEXCOORD, (uint8_t)i, (uint8_t)i);
        add_out(info, SM_FOG, 0, 0);
    }
    for (unsigned i = 0; i < info->nout; ++i)
    {
        SmDecl* d = &info->out[i];
        if ((d->usage == SM_POSITION && d->index == 0) || d->usage == SM_PSIZE)
            continue;
        emit(c, "layout(location = %d) out vec4 o_%s;\n", varying(c, d->usage, d->index), semantic(d->usage, d->index, name));
    }
    if (c->opts)
        for (unsigned i = 0; i < c->opts->nvs_extra; ++i)
        {
            const SmDecl* d = &c->opts->vs_extra[i];
            if (extra_needed(c, d))
                emit(c, "layout(location = %d) out vec4 o_%s;\n", varying(c, d->usage, d->index),
                    semantic(d->usage, d->index, name));
        }
    emit(c, "#ifndef SM_NO_CLIP_DISTANCE\nout float gl_ClipDistance[6];\n#endif\n\n");

    emit(c, "void main()\n{\n");
    emit(c, "    vec4 r[32], v[16], o[16];\n");
    emit(c, "    for (int i = 0; i < 32; ++i) r[i] = vec4(0.0);\n");
    emit(c, "    for (int i = 0; i < 16; ++i) v[i] = o[i] = vec4(0.0);\n");
    emit(c, "    ivec4 a0 = ivec4(0); vec4 a0f = vec4(0.0); int aL = 0; bvec4 p0 = bvec4(false);\n");
    emit(c, "    vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0), oD[2], oT[8]; float oFog = 0.0, oPts = 1.0;\n");
    emit(c, "    oD[0] = oD[1] = vec4(0.0);\n    for (int i = 0; i < 8; ++i) oT[i] = vec4(0.0);\n");
    for (unsigned r = 0; r < 16; ++r)
    {
        if (!c->in_used[r])
            continue;
        unsigned k = c->opts ? c->opts->attr_int[r] : 0;
        if (k == 3) /* UDEC3 */
            emit(c, "    v[%u] = vec4(vec3(uvec3(vin%u, vin%u >> 10, vin%u >> 20) & 1023u), 1.0);\n", r, r, r, r);
        else if (k == 4) /* DEC3N: each signed 10 bits over 511 */
            emit(c, "    v[%u] = vec4(max(vec3(ivec3(uvec3(vin%u, vin%u >> 10, vin%u >> 20) << 22) >> 22) / 511.0, "
                    "-1.0), 1.0);\n", r, r, r, r);
        else
            emit(c, "    v[%u] = vec4(vin%u);\n", r, r);
    }
    /* the main program ends at the first ret before any label */
    unsigned end = c->nins;
    for (unsigned i = 0; i < c->nins; ++i)
        if (c->ins[i].op == OP_RET || c->ins[i].op == OP_LABEL)
        {
            end = i;
            break;
        }
    translate_range(c, 0, end);
    emit(c, "    vec4 pos = vec4(0.0, 0.0, 0.0, 1.0);\n");
    if (sm3)
    {
        int wrote_psize = 0;
        for (unsigned r = 0; r < 16; ++r)
        {
            if (!c->out_used[r])
                continue;
            SmDecl* d = &c->out_decl[r];
            if (d->usage == SM_POSITION && d->index == 0)
                emit(c, "    pos = o[%u];\n", r);
            else if (d->usage == SM_PSIZE)
            {
                if (!wrote_psize)
                    emit(c, "    gl_PointSize = o[%u].x;\n", r);
                wrote_psize = 1;
            }
            else
                emit(c, "    o_%s = o[%u];\n", semantic(d->usage, d->index, name), r);
        }
        if (!wrote_psize)
            emit(c, "    gl_PointSize = 1.0;\n");
    }
    else
    {
        emit(c, "    pos = oPos;\n");
        for (unsigned i = 0; i < 2; ++i)
            emit(c, "    o_color%u = oD[%u];\n", i, i);
        for (unsigned i = 0; i < 8; ++i)
            emit(c, "    o_texcoord%u = oT[%u];\n", i, i);
        emit(c, "    o_fog0 = vec4(oFog);\n");
        emit(c, "    gl_PointSize = oPts;\n");
    }
    if (c->opts)
        for (unsigned i = 0; i < c->opts->nvs_extra; ++i)
        {
            const SmDecl* d = &c->opts->vs_extra[i];
            if (extra_needed(c, d))
                emit(c, "    o_%s = vec4(0.0);\n", semantic(d->usage, d->index, name));
        }
    emit(c, "#ifndef SM_NO_CLIP_DISTANCE\n"
            "    for (int i = 0; i < 6; ++i)\n"
            "        gl_ClipDistance[i] = ((aux.clip_on.x >> uint(i)) & 1u) != 0u ? dot(pos, aux.clip[i]) : 1.0;\n"
            "#endif\n");
    /* D3D9's pixel centres: the half-pixel shift Direct3D 10+ and Vulkan do not have; then Vulkan's
     * clip space, whose y points down */
    emit(c, "    pos.xy += aux.half_pixel.xy * pos.w;\n");
    emit(c, "    gl_Position = vec4(pos.x, -pos.y, pos.z, pos.w);\n");
    emit(c, "}\n");
}

static void pixel_program(Ctx* c)
{
    SmInfo* info = c->info;
    int sm3 = info->major >= 3;
    char name[24];
    uint32_t declared = 0; /* locations, once each */
    if (sm3)
    {
        for (unsigned r = 0; r < 16; ++r)
            if (c->in_used[r])
            {
                SmDecl* d = &c->in_decl[r];
                if (d->usage == SM_POSITION && d->index == 0)
                    continue;
                int l = varying(c, d->usage, d->index);
                if (l >= 0 && !(declared >> l & 1))
                {
                    emit(c, "layout(location = %d) in vec4 i_%s;\n", l, semantic(d->usage, d->index, name));
                    declared |= 1u << l;
                    info->in[info->nin++] = *d;
                }
            }
    }
    else
    {
        for (unsigned r = 0; r < 2; ++r)
            if (c->in_used[r])
            {
                emit(c, "layout(location = %d) in vec4 i_color%u;\n", sm_glsl_location(SM_COLOR, r), r);
                info->in[info->nin++] = (SmDecl){ SM_COLOR, (uint8_t)r, (uint8_t)r, 1 };
            }
        for (unsigned r = 0; r < 8; ++r)
            if (c->tex_used[r])
            {
                emit(c, "layout(location = %d) in vec4 i_texcoord%u;\n", sm_glsl_location(SM_TEXCOORD, r), r);
                info->in[info->nin++] = (SmDecl){ SM_TEXCOORD, (uint8_t)r, (uint8_t)r, 1 };
            }
    }
    for (unsigned i = 0; i < 4; ++i)
        if (info->color_outputs & (1u << i))
            emit(c, "layout(location = %u) out vec4 out_c%u;\n", i, i);
    if (!info->color_outputs)
        emit(c, "layout(location = 0) out vec4 out_c0;\n");
    int depth_out = info->writes_depth && !(c->opts && c->opts->no_depth_out);
    emit(c, "\nvoid main()\n{\n");
    emit(c, "    vec4 r[32], v[16], tc[8], oC[4]; float oDepth = gl_FragCoord.z;\n");
    emit(c, "    for (int i = 0; i < 32; ++i) r[i] = vec4(0.0);\n");
    emit(c, "    for (int i = 0; i < 16; ++i) v[i] = vec4(0.0);\n");
    emit(c, "    for (int i = 0; i < 8; ++i) tc[i] = vec4(0.0);\n");
    emit(c, "    for (int i = 0; i < 4; ++i) oC[i] = vec4(0.0);\n");
    emit(c, "    int aL = 0; bvec4 p0 = bvec4(false); ivec4 a0 = ivec4(0);\n");
    if (c->vpos)
        emit(c, "    vec4 vpos = vec4(gl_FragCoord.xy * aux.rscale.xy - 0.5, 0.0, 0.0);\n");
    if (c->vface)
        emit(c, "    vec4 vface = vec4(gl_FrontFacing ? 1.0 : -1.0);\n");
    if (sm3)
    {
        for (unsigned r = 0; r < 16; ++r)
            if (c->in_used[r])
            {
                SmDecl* d = &c->in_decl[r];
                if (d->usage == SM_POSITION && d->index == 0)
                    emit(c, "    v[%u] = gl_FragCoord;\n", r);
                else
                    emit(c, "    v[%u] = i_%s;\n", r, semantic(d->usage, d->index, name));
            }
    }
    else
    {
        for (unsigned r = 0; r < 2; ++r)
            if (c->in_used[r])
                emit(c, "    v[%u] = i_color%u;\n", r, r);
        for (unsigned r = 0; r < 8; ++r)
            if (c->tex_used[r])
                emit(c, "    tc[%u] = i_texcoord%u;\n", r, r);
    }
    unsigned end = c->nins;
    for (unsigned i = 0; i < c->nins; ++i)
        if (c->ins[i].op == OP_RET || c->ins[i].op == OP_LABEL)
        {
            end = i;
            break;
        }
    translate_range(c, 0, end);
    /* alpha test: the fixed-function stage D3D9 still runs after a pixel shader */
    if (c->opts && c->opts->alpha_func == 1) /* NEVER */
        emit(c, "    discard;\n");
    else if (c->opts && c->opts->alpha_func > 1 && c->opts->alpha_func < 8)
    {
        static const char* const ops[] = { "", "", "<", "==", "<=", ">", "!=", ">=" };
        emit(c, "    if (!(oC[0].w %s aux.alpha_ref.x)) discard;\n", ops[c->opts->alpha_func]);
    }
    for (unsigned i = 0; i < 4; ++i)
        if (info->color_outputs & (1u << i))
            emit(c, "    out_c%u = oC[%u];\n", i, i);
    if (!info->color_outputs)
        emit(c, "    out_c0 = oC[0];\n");
    if (depth_out)
        emit(c, "    gl_FragDepth = oDepth;\n");
    emit(c, "}\n");
}

static Ins g_ins_pool[MAX_INS];

int sm_glsl_info(const uint32_t* tokens, size_t ntokens, SmInfo* info)
{
    static char scratch[1 << 18];
    return sm_glsl_translate(tokens, ntokens, NULL, info, scratch, sizeof scratch, NULL, 0) != 0;
}

size_t sm_glsl_translate(const uint32_t* tokens, size_t ntokens, const SmOpts* opts, SmInfo* info, char* out,
    size_t cap, char* err, size_t errcap)
{
    static Ctx ctx; /* large: the instruction array (callers serialise translations) */
    Ctx* c = &ctx;
    memset(c, 0, sizeof *c);
    memset(info, 0, sizeof *info);
    c->info = info;
    c->opts = opts;
    c->ins = g_ins_pool;
    c->out = out;
    c->cap = cap;
    c->err = err;
    c->errcap = errcap;
    if (err && errcap)
        err[0] = 0;
    if (!ntokens)
        return fail(c, "no tokens"), 0;
    if (!parse(c, tokens, ntokens))
        return 0;
    if (!cap)
        return 0;
    prelude(c);
    if (info->pixel)
        pixel_program(c);
    else
        vertex_program(c);
    if (c->overflow)
        fail(c, "output buffer too small");
    if (c->loops != 0 && !c->failed)
        fail(c, "unbalanced flow control");
    return c->failed ? 0 : c->len;
}
